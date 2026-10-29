#pragma once

#include "PCH.h"

class Settings
{
public:
	static Settings& GetSingleton()
	{
		static Settings instance;
		return instance;
	}

	void Load()
	{
		const auto path = std::filesystem::path("Data/SKSE/Plugins/DynamicSplashes.ini");
		if (!std::filesystem::exists(path)) {
			Save();
			Log();
			return;
		}

		std::ifstream file(path);
		if (!file.is_open()) {
			Log();
			return;
		}

		std::string line;
		while (std::getline(file, line)) {
			auto start = line.find_first_not_of(" \t\r\n");
			if (start == std::string::npos || line[start] == ';' || line[start] == '#' || line[start] == '[') {
				continue;
			}

			auto eq = line.find('=', start);
			if (eq == std::string::npos) {
				continue;
			}

			auto keyEnd = line.find_last_not_of(" \t", eq - 1);
			if (keyEnd == std::string::npos || keyEnd < start) {
				continue;
			}
			std::string key = line.substr(start, keyEnd - start + 1);

			auto valStart = line.find_first_not_of(" \t", eq + 1);
			if (valStart == std::string::npos) {
				continue;
			}
			auto valEnd = line.find_first_of(" \t\r\n;#", valStart);
			std::string val = (valEnd == std::string::npos) ? line.substr(valStart) : line.substr(valStart, valEnd - valStart);
			const bool on = val == "true" || val == "1";

			if (key == "bRipples") {
				ripples = on;
			} else if (key == "bJumpOutSplashes") {
				jumpOutSplashes = on;
			} else if (key == "bMissedSplashes") {
				missedSplashes = on;
			}
		}

		Log();
	}

	void Save() const
	{
		const auto path = std::filesystem::path("Data/SKSE/Plugins/DynamicSplashes.ini");
		std::filesystem::create_directories(path.parent_path());

		std::ofstream file(path);
		if (!file.is_open()) {
			return;
		}

		bool afterValue = false;
		auto line = [&](std::string_view a_text) {
			if (afterValue && a_text.starts_with('#')) {
				file << '\n';
			}
			afterValue = false;
			file << a_text << '\n';
		};
		auto flag = [&](std::string_view a_key, bool a_value) {
			file << a_key << " = " << (a_value ? "true" : "false") << '\n';
			afterValue = true;
		};

		line("[Features]");
		line("# Ripples: jumping or falling into water leaves a fading inner ripple and an outer ring, sized by fall speed and stretched along your movement.");
		flag("bRipples", ripples);
		line("# Jump-Out Splashes: jumping out of water gives a splash with spray, sound and ripples where you push off.");
		flag("bJumpOutSplashes", jumpOutSplashes);
		line("# Missed Splashes: adds the splash the game skips on some of your landings, such as deep-water jumps.");
		flag("bMissedSplashes", missedSplashes);
	}

	bool ripples{ true };
	bool jumpOutSplashes{ true };
	bool missedSplashes{ true };

private:
	Settings() = default;
	Settings(const Settings&) = delete;
	Settings& operator=(const Settings&) = delete;

	void Log() const
	{
		logger::info("Settings loaded: Ripples={}, JumpOutSplashes={}, MissedSplashes={}", ripples, jumpOutSplashes, missedSplashes);
	}
};
