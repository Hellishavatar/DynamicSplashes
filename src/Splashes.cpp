#include "Splashes.h"
#include "Settings.h"

#include <RE/B/BSAnimationGraphEvent.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TES.h>
#include <RE/T/TESWaterObject.h>
#include <RE/T/TESWaterSystem.h>

#include <intrin.h>
#include <mutex>
#include <numbers>
#include <optional>

namespace
{
	constexpr float       kRippleScale = 0.01f;
	constexpr float       kDabSpacing = 8.0f;
	constexpr float       kSplashSizeHeavy = 50.0f;
	constexpr auto        kRippleStep = std::chrono::milliseconds(25);
	constexpr auto        kEntryDelay = std::chrono::milliseconds(50);
	constexpr std::size_t kMaxPendingSplashes = 32;
	constexpr float       kPlayerSplashReach = 256.0f;
	constexpr float       kFallSpeedFull = 1000.0f;
	constexpr float       kRunSpeedFull = 500.0f;
	constexpr auto        kMotionWindow = std::chrono::milliseconds(250);
	constexpr float       kTeleportDistance = 512.0f;
	constexpr float       kWaterAbove = 300.0f;
	constexpr float       kEntryFallSpeed = 150.0f;
	constexpr float       kEntryWaterBelow = 64.0f;
	constexpr auto        kEntryWait = std::chrono::milliseconds(150);
	constexpr auto        kTrackedWait = std::chrono::milliseconds(60);
	constexpr float       kJumpDepthFull = 120.0f;
	constexpr float       kJumpRiseMin = 16.0f;
	constexpr float       kJumpDescent = 8.0f;
	constexpr float       kJumpClearance = 32.0f;
	constexpr auto        kJumpTrack = std::chrono::seconds(3);
	constexpr float       kJumpOutSplashSize = 30.0f;
	constexpr std::array  kDiscFade{ 1.0f, 0.6f, 0.35f, 0.15f };
	constexpr float       kDiscFeather = 0.5f;

	struct PendingSplash
	{
		RE::NiPoint3                          pos;
		float                                 radius{ 0.0f };
		float                                 stretch{ 1.0f };
		float                                 dirX{ 1.0f };
		float                                 dirY{ 0.0f };
		std::chrono::steady_clock::time_point stepTime{};
		std::size_t                           stage{ 0 };
	};

	struct MotionSample
	{
		std::chrono::steady_clock::time_point time;
		RE::NiPoint3                          pos;
	};

	std::mutex                 splashLock;
	std::vector<PendingSplash> pendingSplashes;
	std::vector<MotionSample>  playerMotion;
	std::uintptr_t             actorSplashReturn{ 0 };
	std::atomic<std::int64_t>  lastPlayerEntrySplash{ 0 };
	std::atomic<bool>          jumpStarted{ false };

	std::int64_t Ticks(std::chrono::steady_clock::time_point a_time)
	{
		return a_time.time_since_epoch().count();
	}

	bool NearPlayer(const RE::NiPoint3& a_pos)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return false;
		}
		const auto  position = player->GetPosition();
		const float dx = position.x - a_pos.x;
		const float dy = position.y - a_pos.y;
		return dx * dx + dy * dy <= kPlayerSplashReach * kPlayerSplashReach;
	}

	bool UnderLodRoot(const RE::NiAVObject* a_object)
	{
		auto* tes = RE::TES::GetSingleton();
		auto* root = tes ? tes->objLODWaterRoot : nullptr;
		int   depth = 0;
		for (auto* parent = a_object ? a_object->parent : nullptr; root && parent && depth < 6; parent = parent->parent, ++depth) {
			if (parent == root) {
				return true;
			}
		}
		return false;
	}

	std::optional<float> SurfaceBetween(float a_x, float a_y, float a_low, float a_high)
	{
		auto* system = RE::TESWaterSystem::GetSingleton();
		if (!system) {
			return std::nullopt;
		}
		std::optional<float> best;
		RE::BSSpinLockGuard  guard(system->lock);
		for (const auto& object : system->waterObjects) {
			auto* shape = object ? object->shape.get() : nullptr;
			if (!shape || !shape->parent || UnderLodRoot(shape) || std::abs(object->plane.normal.z - 1.0f) > 1e-3f) {
				continue;
			}
			const auto& bound = shape->worldBound;
			const float dx = bound.center.x - a_x;
			const float dy = bound.center.y - a_y;
			const float height = object->plane.constant;
			if (dx * dx + dy * dy <= bound.radius * bound.radius && height >= a_low && height <= a_high && (!best || height > *best)) {
				best = height;
			}
		}
		return best;
	}

	RE::NiPoint3 PlayerVelocity(RE::PlayerCharacter* a_player)
	{
		RE::NiPoint3 velocity;
		a_player->GetLinearVelocity(velocity);
		std::scoped_lock lock(splashLock);
		float fall = 0.0f;
		float bestX = 0.0f;
		float bestY = 0.0f;
		for (std::size_t i = 1; i < playerMotion.size(); ++i) {
			const float seconds = std::chrono::duration<float>(playerMotion[i].time - playerMotion[i - 1].time).count();
			if (seconds <= 0.0f) {
				continue;
			}
			fall = (std::max)(fall, (playerMotion[i - 1].pos.z - playerMotion[i].pos.z) / seconds);
			const float x = (playerMotion[i].pos.x - playerMotion[i - 1].pos.x) / seconds;
			const float y = (playerMotion[i].pos.y - playerMotion[i - 1].pos.y) / seconds;
			if (std::hypot(x, y) > std::hypot(bestX, bestY)) {
				bestX = x;
				bestY = y;
			}
		}
		if (fall > std::abs(velocity.z)) {
			velocity.z = -fall;
		}
		if (std::hypot(bestX, bestY) > std::hypot(velocity.x, velocity.y)) {
			velocity.x = bestX;
			velocity.y = bestY;
		}
		return velocity;
	}

	void ShapeAlongMotion(PendingSplash& a_splash, const RE::NiPoint3& a_velocity)
	{
		const float run = std::hypot(a_velocity.x, a_velocity.y);
		if (run > 1.0f) {
			a_splash.stretch = 1.0f + 0.8f * (std::min)(run / kRunSpeedFull, 1.0f);
			a_splash.dirX = a_velocity.x / run;
			a_splash.dirY = a_velocity.y / run;
		}
	}

	void Queue(const PendingSplash& a_splash)
	{
		std::scoped_lock lock(splashLock);
		if (pendingSplashes.size() < kMaxPendingSplashes) {
			pendingSplashes.push_back(a_splash);
		}
	}

	void QueueEntryRipples(const RE::NiPoint3& a_pos, float a_size)
	{
		const float   weight = std::clamp(a_size / kSplashSizeHeavy, 0.1f, 1.5f);
		PendingSplash splash{ a_pos, 8.0f + 28.0f * weight };
		splash.stepTime = std::chrono::steady_clock::now() + kEntryDelay;
		if (NearPlayer(a_pos)) {
			auto*      player = RE::PlayerCharacter::GetSingleton();
			const auto velocity = PlayerVelocity(player);
			splash.radius *= std::clamp(0.8f + 0.8f * std::abs(velocity.z) / kFallSpeedFull, 0.8f, 1.6f);
			ShapeAlongMotion(splash, velocity);
		}
		Queue(splash);
	}

	struct Hook_WaterSplash
	{
		static void thunk(float a_size, RE::NiPoint3* a_pos)
		{
			const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
			func(a_size, a_pos);
			if (caller != actorSplashReturn || !a_pos || !(a_size > 0.0f)) {
				return;
			}
			if (NearPlayer(*a_pos)) {
				lastPlayerEntrySplash.store(Ticks(std::chrono::steady_clock::now()), std::memory_order_release);
			}
			if (Settings::GetSingleton().ripples) {
				QueueEntryRipples(*a_pos, a_size);
			}
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	float GameLikeSplashSize(float a_fall)
	{
		return std::clamp(72.0f + 0.08f * a_fall, 72.0f, 100.0f);
	}

	void PlayGameSplash(float a_size, const RE::NiPoint3& a_pos)
	{
		if (Hook_WaterSplash::func.address()) {
			auto pos = a_pos;
			Hook_WaterSplash::func(a_size, &pos);
		}
	}

	void AddMissedSplash(const RE::NiPoint3& a_pos, float a_size)
	{
		lastPlayerEntrySplash.store(Ticks(std::chrono::steady_clock::now()), std::memory_order_release);
		PlayGameSplash(a_size, a_pos);
		QueueEntryRipples(a_pos, a_size);
	}

	void AddDab(RE::TESWaterSystem* a_system, const PendingSplash& a_splash, float a_along, float a_across)
	{
		const RE::NiPoint3 point{
			a_splash.pos.x + a_along * a_splash.dirX - a_across * a_splash.dirY,
			a_splash.pos.y + a_along * a_splash.dirY + a_across * a_splash.dirX,
			a_splash.pos.z
		};
		a_system->AddRipple(point, kRippleScale);
	}

	void AddDisc(RE::TESWaterSystem* a_system, const PendingSplash& a_splash, float a_radius)
	{
		const float front = a_radius * a_splash.stretch;
		const int   ahead = static_cast<int>(front / kDabSpacing);
		const int   behind = static_cast<int>(a_radius / kDabSpacing);
		for (int iy = -behind; iy <= behind; ++iy) {
			for (int ix = -behind; ix <= ahead; ++ix) {
				const float u = static_cast<float>(ix) * kDabSpacing;
				const float v = static_cast<float>(iy) * kDabSpacing;
				const float reach = u > 0.0f ? front : a_radius;
				const float edge = (u * u) / (reach * reach) + (v * v) / (a_radius * a_radius);
				if (edge <= 1.0f && (edge <= kDiscFeather || ((ix + iy) & 1) == 0)) {
					AddDab(a_system, a_splash, u, v);
				}
			}
		}
	}

	void AddRing(RE::TESWaterSystem* a_system, const PendingSplash& a_splash, float a_radius)
	{
		const float front = a_radius * a_splash.stretch;
		const float perimeter = std::numbers::pi_v<float> * (0.5f * (front + a_radius) + a_radius);
		const int   count = std::clamp(static_cast<int>(std::ceil(perimeter / kDabSpacing)), 8, 96);
		for (int i = 0; i < count; ++i) {
			const float angle = 2.0f * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(count);
			const float c = std::cos(angle);
			AddDab(a_system, a_splash, (c > 0.0f ? front : a_radius) * c, a_radius * std::sin(angle));
		}
	}

	class JumpSink : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
		{
			static const RE::BSFixedString jumpUp{ "JumpUp" };
			if (a_event && a_event->tag == jumpUp) {
				jumpStarted.store(true, std::memory_order_release);
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	JumpSink jumpSink;

	struct Entry
	{
		RE::NiPoint3                          pos;
		float                                 fall{ 0.0f };
		std::chrono::steady_clock::time_point time{};
		std::chrono::milliseconds             wait{ kEntryWait };
		bool                                  pending{ false };
	};

	struct Jump
	{
		float                                 startZ{ 0.0f };
		float                                 peakZ{ 0.0f };
		float                                 surface{ 0.0f };
		std::chrono::steady_clock::time_point time{};
		bool                                  active{ false };
	};

	Entry entry;
	Jump  jump;
	bool  wasInWater{ false };

	void TrackPlayer(RE::PlayerCharacter* a_player, std::chrono::steady_clock::time_point a_now)
	{
		const auto& settings = Settings::GetSingleton();
		const auto  position = a_player->GetPosition();
		{
			std::scoped_lock lock(splashLock);
			if (!playerMotion.empty() && playerMotion.back().pos.GetDistance(position) > kTeleportDistance) {
				playerMotion.clear();
			}
			playerMotion.push_back({ a_now, position });
			std::erase_if(playerMotion, [&](const MotionSample& a_sample) { return a_now - a_sample.time > kMotionWindow; });
		}

		const bool inWater = a_player->IsInWater();
		if (inWater && !wasInWater && settings.missedSplashes) {
			const float fall = -PlayerVelocity(a_player).z;
			if (fall >= kEntryFallSpeed) {
				entry = { position, fall, a_now, kEntryWait, true };
			}
		}
		wasInWater = inWater;

		if (entry.pending && a_now - entry.time >= entry.wait) {
			entry.pending = false;
			const bool gameSplashed = lastPlayerEntrySplash.load(std::memory_order_acquire) >= Ticks(entry.time - kEntryWait);
			if (!gameSplashed && settings.missedSplashes) {
				if (const auto surface = SurfaceBetween(entry.pos.x, entry.pos.y, entry.pos.z - kEntryWaterBelow, entry.pos.z + kWaterAbove)) {
					AddMissedSplash({ entry.pos.x, entry.pos.y, *surface }, GameLikeSplashSize(entry.fall));
				}
			}
		}

		if (jump.active) {
			jump.peakZ = (std::max)(jump.peakZ, position.z);
			const float rise = jump.peakZ - jump.startZ;
			const float land = (std::min)(jump.surface, jump.peakZ - kJumpDescent);
			if (a_now - jump.time > kJumpTrack || jump.peakZ > jump.surface + kJumpClearance) {
				jump.active = false;
			} else if (rise >= kJumpRiseMin && position.z <= land) {
				jump.active = false;
				const float fall = (std::max)(-PlayerVelocity(a_player).z, 0.0f);
				if (settings.missedSplashes) {
					if (jump.peakZ > jump.surface) {
						entry = { position, fall, a_now, kTrackedWait, true };
					} else {
						AddMissedSplash({ position.x, position.y, jump.surface }, GameLikeSplashSize(fall));
					}
				}
			}
		}

		if (!jumpStarted.exchange(false, std::memory_order_acq_rel)) {
			return;
		}
		if (!(settings.jumpOutSplashes || settings.missedSplashes) || !inWater) {
			return;
		}
		const auto surface = SurfaceBetween(position.x, position.y, position.z, position.z + kWaterAbove);
		if (!surface) {
			return;
		}
		jump = { position.z, position.z, *surface, a_now, true };
		if (!settings.jumpOutSplashes) {
			return;
		}
		const RE::NiPoint3 at{ position.x, position.y, *surface };
		const bool         swimming = a_player->AsActorState()->IsSwimming();
		if (!swimming) {
			PlayGameSplash(kJumpOutSplashSize, at);
		}
		const float   weight = std::clamp((*surface - position.z) / kJumpDepthFull, 0.2f, 0.8f);
		PendingSplash splash{ at, 8.0f + 28.0f * weight };
		ShapeAlongMotion(splash, PlayerVelocity(a_player));
		Queue(splash);
	}

	void DrawPending(std::chrono::steady_clock::time_point a_now)
	{
		std::vector<PendingSplash> due;
		{
			std::scoped_lock lock(splashLock);
			if (pendingSplashes.empty()) {
				return;
			}
			due.swap(pendingSplashes);
		}

		auto* system = RE::TESWaterSystem::GetSingleton();
		if (!system) {
			return;
		}

		std::vector<PendingSplash> later;
		for (auto& splash : due) {
			const bool waiting = splash.stage > 0 ? a_now - splash.stepTime < kRippleStep : a_now < splash.stepTime;
			if (waiting) {
				later.push_back(splash);
				continue;
			}
			if (splash.stage == 1) {
				AddRing(system, splash, splash.radius);
			}
			AddDisc(system, splash, splash.radius * 0.5f * kDiscFade[splash.stage]);
			if (++splash.stage < kDiscFade.size()) {
				splash.stepTime = a_now;
				later.push_back(splash);
			}
		}

		if (!later.empty()) {
			std::scoped_lock lock(splashLock);
			for (auto& splash : later) {
				if (pendingSplashes.size() < kMaxPendingSplashes) {
					pendingSplashes.push_back(splash);
				}
			}
		}
	}

	struct Hook_PlayerUpdate
	{
		static void thunk(RE::PlayerCharacter* a_this, float a_delta)
		{
			func(a_this, a_delta);
			const auto now = std::chrono::steady_clock::now();
			TrackPlayer(a_this, now);
			DrawPending(now);
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	std::uintptr_t FindCallReturn(std::uintptr_t a_caller, std::uintptr_t a_callee)
	{
		DWORD64    imageBase = 0;
		const auto entry = RtlLookupFunctionEntry(a_caller, &imageBase, nullptr);
		if (!entry || imageBase + entry->BeginAddress != a_caller) {
			return 0;
		}
		auto end = static_cast<std::uintptr_t>(imageBase + entry->EndAddress);
		for (DWORD64 nextBase = 0;;) {
			const auto next = RtlLookupFunctionEntry(end, &nextBase, nullptr);
			if (!next || nextBase != imageBase || imageBase + next->BeginAddress != end || (next->UnwindData & 1)) {
				break;
			}
			const auto unwindFlags = *reinterpret_cast<const std::uint8_t*>(imageBase + next->UnwindData) >> 3;
			if (!(unwindFlags & UNW_FLAG_CHAININFO)) {
				break;
			}
			end = static_cast<std::uintptr_t>(imageBase + next->EndAddress);
		}

		std::uintptr_t found = 0;
		for (auto address = a_caller; address + 5 <= end; ++address) {
			if (*reinterpret_cast<const std::uint8_t*>(address) != 0xE8) {
				continue;
			}
			std::int32_t displacement;
			std::memcpy(&displacement, reinterpret_cast<const void*>(address + 1), sizeof(displacement));
			if (address + 5 + static_cast<std::intptr_t>(displacement) != a_callee) {
				continue;
			}
			if (found) {
				return 0;
			}
			found = address + 5;
		}
		return found;
	}
}

namespace Splashes
{
	void Install()
	{
		const auto splash = REL::RelocationID(31297, 32081).address();
		actorSplashReturn = FindCallReturn(REL::RelocationID(36357, 37348).address(), splash);
		if (!actorSplashReturn) {
			logger::error("Could not find the actor water-entry splash call in the game's code. Dynamic Splashes is off.");
			return;
		}
		if (!Detour::Function<Hook_WaterSplash>(splash)) {
			logger::error("Failed to hook the water splash (another mod may have patched it). Dynamic Splashes is off.");
			return;
		}
		REL::Relocation<std::uintptr_t> playerVtbl{ RE::VTABLE_PlayerCharacter[0] };
		Hook_PlayerUpdate::func = playerVtbl.write_vfunc(0xAD, Hook_PlayerUpdate::thunk);
		logger::info("Hooked the water splash (actor water-entry call returning to +0x{:X}) and PlayerCharacter::Update.",
			actorSplashReturn - REL::Module::get().base());
	}

	void OnGameLoaded()
	{
		{
			std::scoped_lock lock(splashLock);
			playerMotion.clear();
		}
		entry = {};
		jump = {};
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			player->RemoveAnimationGraphEventSink(&jumpSink);
			player->AddAnimationGraphEventSink(&jumpSink);
		}
	}
}
