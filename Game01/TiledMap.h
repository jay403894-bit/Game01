#pragma once
#include "json.hpp"
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

// Generic, un-opinionated Tiled JSON (.tmj) parser. Doesn't know or care what "solid" or
// "player" mean -- it just exposes exactly what's in the file: tile layers as raw GID grids,
// object layers as raw objects, and every custom property (tile or object) as-is, typed. No
// translation into a fixed small vocabulary, no collapsing 30 different-looking tiles into one
// char -- that decision belongs entirely to whatever game-side code reads this, not the loader.

// A single custom property's value, generic over Tiled's 4 property types (string/int/float/bool).
// AsX() coerces across numeric kinds (so a property authored as "int" still works through
// AsFloat()) but never silently coerces string<->number.
struct TiledValue {
	enum class Kind { None, Bool, Int, Float, String } kind = Kind::None;
	bool b = false; int i = 0; float f = 0.0f; std::string s;

	float AsFloat(float def = 0.0f) const {
		switch (kind) { case Kind::Int: return (float)i; case Kind::Float: return f; case Kind::Bool: return b ? 1.0f : 0.0f; default: return def; }
	}
	int AsInt(int def = 0) const {
		switch (kind) { case Kind::Int: return i; case Kind::Float: return (int)f; case Kind::Bool: return b ? 1 : 0; default: return def; }
	}
	bool AsBool(bool def = false) const {
		switch (kind) { case Kind::Bool: return b; case Kind::Int: return i != 0; case Kind::Float: return f != 0.0f; default: return def; }
	}
	std::string AsString(const std::string& def = "") const { return kind == Kind::String ? s : def; }

	static TiledValue Parse(const nlohmann::json& prop) {
		TiledValue v;
		std::string type = prop.value("type", "string");
		if (type == "bool") { v.kind = Kind::Bool; v.b = prop.value("value", false); }
		else if (type == "int") { v.kind = Kind::Int; v.i = prop.value("value", 0); }
		else if (type == "float") { v.kind = Kind::Float; v.f = prop.value("value", 0.0f); }
		else { v.kind = Kind::String; v.s = prop.value("value", ""); } // string, color, file, object, class -> stored raw
		return v;
	}
};

using TiledProperties = std::unordered_map<std::string, TiledValue>;

inline TiledProperties ParseProperties(const nlohmann::json& j) {
	TiledProperties props;
	if (!j.contains("properties")) return props;
	for (const auto& prop : j["properties"]) {
		std::string name = prop.value("name", "");
		if (!name.empty()) props[name] = TiledValue::Parse(prop);
	}
	return props;
}

struct TiledObject {
	int id = 0;
	std::string name;
	std::string objType; // Tiled's own built-in Class/Type field -- NOT a custom property, a
	                      // separate first-class field Tiled objects have (shows up as "type" or
	                      // "class" depending on Tiled version -- both checked when parsing).
	float x = 0, y = 0, width = 0, height = 0;
	bool point = false;
	TiledProperties properties;
};

struct TiledTileset {
	int firstGid = 1;
	std::wstring imagePath; // as written in the file -- resolve relative to the map file's own
	                         // directory yourself when actually loading the texture (Tiled paths
	                         // are relative to the FILE, not the exe/cwd).
	int tileWidth = 16, tileHeight = 16, imageWidth = 0, imageHeight = 0, columns = 0, tileCount = 0;
	// Per-LOCAL-tile-id (0-based within this tileset, i.e. gid - firstGid) custom properties, from
	// the embedded tileset's own "tiles" array. Only embedded tilesets are supported (defined
	// directly in the map file) -- an external .tsj referenced via "source" is not followed.
	std::unordered_map<int, TiledProperties> tileProperties;
};

struct TiledLayer {
	std::string name;
	std::string type; // "tilelayer" or "objectgroup" -- others (imagelayer, group) are skipped
	int width = 0, height = 0;
	std::vector<int> data;            // tilelayer: row-major GIDs, width*height entries
	std::vector<TiledObject> objects; // objectgroup
};

struct TiledMap {
	int width = 0, height = 0, tileWidth = 16, tileHeight = 16;
	std::vector<TiledTileset> tilesets;
	std::vector<TiledLayer> layers;

	// Which tileset a GID belongs to, and its LOCAL id within that tileset (gid - firstGid).
	// tilesetIndex is -1 for gid==0 (Tiled's "empty cell") or a gid no tileset's range covers.
	void ResolveGid(int gid, int& tilesetIndex, int& localId) const {
		tilesetIndex = -1; localId = -1;
		if (gid == 0) return;
		// Highest firstGid <= gid wins -- tilesets can be listed in any order.
		int best = -1;
		for (int t = 0; t < (int)tilesets.size(); ++t) {
			if (tilesets[t].firstGid <= gid && (best == -1 || tilesets[t].firstGid > tilesets[best].firstGid)) {
				best = t;
			}
		}
		if (best == -1) return;
		tilesetIndex = best;
		localId = gid - tilesets[best].firstGid;
	}

	// Custom properties for a tile CELL identified by its raw GID -- nullptr if gid==0 or that
	// tile was never given any properties in the tileset editor.
	const TiledProperties* TilePropertiesForGid(int gid) const {
		int tilesetIndex, localId;
		ResolveGid(gid, tilesetIndex, localId);
		if (tilesetIndex == -1) return nullptr;
		auto it = tilesets[tilesetIndex].tileProperties.find(localId);
		return (it != tilesets[tilesetIndex].tileProperties.end()) ? &it->second : nullptr;
	}

	const TiledLayer* FindLayer(const std::string& name) const {
		for (const auto& l : layers) if (l.name == name) return &l;
		return nullptr;
	}

	static TiledMap Load(const std::wstring& path)
	{
		TiledMap map;
		std::ifstream file(path);
		if (!file.is_open()) return map;

		nlohmann::json j;
		try { file >> j; } catch (const nlohmann::json::parse_error&) { return map; }

		map.width = j.value("width", 0);
		map.height = j.value("height", 0);
		map.tileWidth = j.value("tilewidth", 16);
		map.tileHeight = j.value("tileheight", 16);

		if (j.contains("tilesets")) {
			for (const auto& ts : j["tilesets"]) {
				TiledTileset tileset;
				tileset.firstGid = ts.value("firstgid", 1);
				tileset.tileWidth = ts.value("tilewidth", map.tileWidth);
				tileset.tileHeight = ts.value("tileheight", map.tileHeight);
				tileset.imageWidth = ts.value("imagewidth", 0);
				tileset.imageHeight = ts.value("imageheight", 0);
				tileset.columns = ts.value("columns", 0);
				tileset.tileCount = ts.value("tilecount", 0);
				if (ts.contains("image")) {
					std::string img = ts.value("image", "");
					tileset.imagePath = std::wstring(img.begin(), img.end()); // ASCII path assumption
				}
				if (ts.contains("tiles")) {
					for (const auto& tile : ts["tiles"]) {
						int localId = tile.value("id", -1);
						if (localId >= 0) tileset.tileProperties[localId] = ParseProperties(tile);
					}
				}
				map.tilesets.push_back(std::move(tileset));
			}
		}

		if (j.contains("layers")) {
			for (const auto& layerJson : j["layers"]) {
				TiledLayer layer;
				layer.name = layerJson.value("name", "");
				layer.type = layerJson.value("type", "");
				layer.width = layerJson.value("width", 0);
				layer.height = layerJson.value("height", 0);

				if (layer.type == "tilelayer" && layerJson.contains("data")) {
					layer.data.reserve(layerJson["data"].size());
					for (const auto& gid : layerJson["data"]) layer.data.push_back(gid.get<int>());
				} else if (layer.type == "objectgroup" && layerJson.contains("objects")) {
					for (const auto& objJson : layerJson["objects"]) {
						TiledObject obj;
						obj.id = objJson.value("id", 0);
						obj.name = objJson.value("name", "");
						// Tiled 1.9+ renamed the object's built-in Class field from "type" to
						// "class" in the JSON export; older exports (and "type" as a CUSTOM
						// property some users add anyway) still use "type". Check both.
						obj.objType = objJson.value("class", objJson.value("type", ""));
						obj.x = objJson.value("x", 0.0f);
						obj.y = objJson.value("y", 0.0f);
						obj.width = objJson.value("width", 0.0f);
						obj.height = objJson.value("height", 0.0f);
						obj.point = objJson.value("point", false);
						obj.properties = ParseProperties(objJson);
						layer.objects.push_back(std::move(obj));
					}
				}
				map.layers.push_back(std::move(layer));
			}
		}

		return map;
	}
};
