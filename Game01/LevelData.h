#pragma once
#include <vector>
#include <string>
#include <unordered_map>
#include <fstream>
#include <sstream>

// A level = the same ASCII tile grid MapReader has always used, plus an optional per-cell
// METADATA section for anything beyond bare tile type (platform groupID/speed/target offset,
// enemy patrol bounds/chase radius/speed, ...). A cell with no metadata entry falls back to
// MapReader's own hardcoded defaults -- a TILES-only file (no META section at all) behaves
// identically to the old inline LEVEL_1 array, so metadata is purely opt-in tuning, never
// required just to place a tile.
struct TileMeta {
	std::unordered_map<std::string, float> values;
	bool Has(const std::string& key) const { return values.count(key) != 0; }
	float Get(const std::string& key, float defaultValue) const {
		auto it = values.find(key);
		return it != values.end() ? it->second : defaultValue;
	}
};

struct LevelData {
	std::vector<std::string> tiles;
	// Keyed by "col,row" -- matches the file format's own META line prefix exactly, so no
	// separate (col,row)<->string encode/decode step is needed at lookup time either.
	std::unordered_map<std::string, TileMeta> meta;

	const TileMeta* MetaAt(int col, int row) const {
		auto it = meta.find(std::to_string(col) + "," + std::to_string(row));
		return it != meta.end() ? &it->second : nullptr;
	}
};

// File shape (two sections, order doesn't matter, either can be omitted):
//   TILES
//   #......................d
//   #..$...................
//   ...
//
//   META
//   5,4 groupID=101 speed=64 targetOffsetY=-300
//   9,2 patrolLeft=-48 patrolRight=48 speed=40
//
// Only a line that's EXACTLY "TILES" or "META" starts/switches a section -- a blank line inside
// TILES is kept as a literal (empty) map row, not treated as a section terminator, so a level
// with genuinely blank rows round-trips correctly.
struct LevelFormat
{
	static LevelData Load(const std::wstring& path)
	{
		LevelData data;
		std::ifstream file(path);
		if (!file.is_open()) return data; // caller decides how to handle a missing/empty level

		enum class Section { None, Tiles, Meta } section = Section::None;
		std::string line;
		while (std::getline(file, line)) {
			// Strip a trailing \r -- whether a given file has \r\n or \n line endings depends on
			// how/where it was last saved, and a stray \r baked into a "tile row" string would
			// silently become part of that row's LAST character comparison in MapReader forever
			// (a real bug this exact pattern has hit before elsewhere in this codebase).
			if (!line.empty() && line.back() == '\r') line.pop_back();

			if (line == "TILES") { section = Section::Tiles; continue; }
			if (line == "META") { section = Section::Meta; continue; }

			if (section == Section::Tiles) {
				data.tiles.push_back(line);
			} else if (section == Section::Meta) {
				if (line.empty()) continue;
				std::istringstream iss(line);
				std::string key; // "col,row"
				iss >> key;
				TileMeta tm;
				std::string pair;
				while (iss >> pair) {
					size_t eq = pair.find('=');
					if (eq == std::string::npos) continue;
					tm.values[pair.substr(0, eq)] = std::stof(pair.substr(eq + 1));
				}
				data.meta[key] = tm;
			}
		}
		return data;
	}

	static void Save(const std::wstring& path, const LevelData& data)
	{
		std::ofstream file(path);
		if (!file.is_open()) return;

		file << "TILES\n";
		for (const auto& row : data.tiles) file << row << "\n";

		file << "\nMETA\n";
		for (const auto& entry : data.meta) {
			file << entry.first;
			for (const auto& kv : entry.second.values) file << " " << kv.first << "=" << kv.second;
			file << "\n";
		}
	}
};
