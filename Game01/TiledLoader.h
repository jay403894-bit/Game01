#pragma once
#include "LevelData.h"
#include "json.hpp"
#include <fstream>
#include <unordered_map>

// Parses a Tiled JSON map export (.tmj/.json -- Tiled's "Export As" JSON, NOT the .tmx/XML
// format) into the SAME LevelData MapReader.h already consumes -- MapReader itself needed no
// changes at all; this is just a second way to PRODUCE a LevelData, alongside LevelFormat's
// custom text format (LevelData.h), which still works and isn't going anywhere.
//
// Conventions this loader expects the Tiled project to follow (see level1.tmj for a working
// example generated from the original hardcoded test map):
//
//   Every tile/object is tagged with a custom STRING property named "type" -- a BEHAVIOR
//   category (solid/platform/background/slopeUpRight/slopeUpLeft/player/patrolEnemy/chaseEnemy/
//   coin, see TypeNameToChar below), NOT a specific tile's exact appearance. This matters because
//   most of these are pure flavor: a level might have 400 different-looking tiles (grass, brick,
//   dirt, ice, whatever a given tileset/biome has) that are all physically just "solid" -- tagging
//   each of them type="solid" collapses all of them to the same internal behavior regardless of
//   which one got painted where. Slopes are the one category that DOES need tagging per-tile
//   rather than bucketed, since they're actual geometry (not flavor) and not every tileset even
//   has slope art to begin with.
//
//   STRUCTURAL types (solid/platform/background/slopeUpRight/slopeUpLeft) go on a TILE LAYER --
//   tag each tile in the tileset that should behave that way. GID 0 (an empty cell -- Tiled's own
//   "no tile here" convention) becomes background automatically, and any tile whose gid has no
//   matching "type" property also falls back to background (fails toward "empty," not a crash, if
//   a tileset tile was never tagged).
//
//   ENTITY types (player/patrolEnemy/chaseEnemy/coin) go on an OBJECT LAYER instead -- drop a
//   point object, tag it with "type" the same way. The object's pixel position is converted to a
//   tile column/row (x/tilewidth, y/tileheight -- so point objects should stay grid-aligned in
//   Tiled for predictable placement). Every OTHER custom property on the object (patrolLeft,
//   chaseRadius, speed, health, jumpForce, ...) becomes a META override for that exact cell --
//   identical to hand-writing "col,row key=value" in the old custom text format, just authored
//   through Tiled's own property panel instead.
//
// NOT supported yet: external tilesets (a tileset referenced via "source" pointing at a separate
// .tsj file) -- only an EMBEDDED tileset (defined directly inside the map file, Tiled's default
// unless you explicitly save the tileset separately) is parsed. Multiple tile/object layers are
// also not merged -- only the FIRST tilelayer and FIRST objectgroup found are read.
struct TiledLoader
{
	// The only place the behavior-category vocabulary is spelled out -- shared by both the tile
	// layer and object layer passes below, so a tileset tile and an object can both use "type" and
	// mean the same thing. Unknown/unset type falls back to '.' (background) rather than crashing
	// or silently defaulting to something solid that could trap the player.
	static char TypeNameToChar(const std::string& type)
	{
		if (type == "solid") return '#';
		if (type == "platform") return '_';
		if (type == "slopeUpRight") return '/';
		if (type == "slopeUpLeft") return '\\';
		if (type == "background") return '.';
		if (type == "player") return '@';
		if (type == "patrolEnemy") return 'E';
		if (type == "chaseEnemy") return 'C';
		if (type == "coin") return '$';
		return '.';
	}

	static LevelData Load(const std::wstring& path)
	{
		LevelData data;

		std::ifstream file(path);
		if (!file.is_open()) return data; // caller decides how to handle a missing/empty level

		nlohmann::json j;
		try {
			file >> j;
		} catch (const nlohmann::json::parse_error&) {
			return data; // malformed file -- fail empty, same as a missing one, not a crash
		}

		int mapWidth = j.value("width", 0);
		int mapHeight = j.value("height", 0);
		int tileWidth = j.value("tilewidth", 16);
		int tileHeight = j.value("tileheight", 16);
		if (mapWidth <= 0 || mapHeight <= 0) return data;

		// gid -> char, built from every embedded tileset's per-tile "type" property. Many gids can
		// (and, for flavor tiles, SHOULD) map to the same char -- see the class comment.
		std::unordered_map<int, char> gidToChar;
		if (j.contains("tilesets")) {
			for (const auto& ts : j["tilesets"]) {
				int firstGid = ts.value("firstgid", 1);
				if (!ts.contains("tiles")) continue; // no embedded per-tile data -- external tileset, unsupported
				for (const auto& tile : ts["tiles"]) {
					int localId = tile.value("id", -1);
					if (localId < 0 || !tile.contains("properties")) continue;
					for (const auto& prop : tile["properties"]) {
						if (prop.value("name", "") == "type") {
							gidToChar[firstGid + localId] = TypeNameToChar(prop.value("value", ""));
							break;
						}
					}
				}
			}
		}

		// Tile grid, defaulted to all-background so a short/missing tilelayer still yields a
		// well-formed rectangular grid instead of ragged/empty rows.
		data.tiles.assign(mapHeight, std::string(mapWidth, '.'));

		if (j.contains("layers")) {
			for (const auto& layer : j["layers"]) {
				if (layer.value("type", "") == "tilelayer" && layer.contains("data")) {
					const auto& flat = layer["data"];
					for (int row = 0; row < mapHeight; ++row) {
						for (int col = 0; col < mapWidth; ++col) {
							size_t i = (size_t)row * mapWidth + col;
							if (i >= flat.size()) continue;
							int gid = flat[i].get<int>();
							if (gid == 0) continue; // already '.', nothing to look up
							auto it = gidToChar.find(gid);
							data.tiles[row][col] = (it != gidToChar.end()) ? it->second : '.';
						}
					}
					break; // only the first tilelayer
				}
			}

			for (const auto& layer : j["layers"]) {
				if (layer.value("type", "") == "objectgroup" && layer.contains("objects")) {
					for (const auto& obj : layer["objects"]) {
						float px = obj.value("x", 0.0f);
						float py = obj.value("y", 0.0f);
						int col = (int)(px / tileWidth);
						int row = (int)(py / tileHeight);
						if (row < 0 || row >= mapHeight || col < 0 || col >= mapWidth) continue;
						if (!obj.contains("properties")) continue;

						char entityChar = 0;
						TileMeta tm;
						for (const auto& prop : obj["properties"]) {
							std::string name = prop.value("name", "");
							std::string propType = prop.value("type", "");
							if (name == "type") {
								entityChar = TypeNameToChar(prop.value("value", ""));
							} else if (propType == "float" || propType == "int") {
								tm.values[name] = prop["value"].get<float>();
							}
							// bool/string properties other than "type" are ignored -- MapReader's
							// META lookups (TileMeta::Get) are float-only, same as the custom
							// text format's own "key=value" always being numeric.
						}
						if (entityChar == 0) continue; // object had no "type" property -- nothing to place

						data.tiles[row][col] = entityChar;
						if (!tm.values.empty()) {
							data.meta[std::to_string(col) + "," + std::to_string(row)] = tm;
						}
					}
					break; // only the first objectgroup
				}
			}
		}

		return data;
	}
};
