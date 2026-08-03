#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>

// Custom binary level format. Reason this exists instead of using Tiled: Tiled's tile custom
// properties are global to a tile DEFINITION (the same everywhere that exact tile is painted on
// the map), not per painted INSTANCE, and this game needs per-instance metadata coupled directly
// to which sprite was used at that specific cell -- Tiled's object layer supports per-instance
// properties but isn't tied to a tile-grid sprite at all. See the format design discussion for
// the full reasoning.
//
// Every cell/object carries a short CAPPED list of (propertyNameID, value) pairs instead of a
// fixed predetermined set of fields -- new property NAMES can be introduced at any time (just
// add a string to the name table) without ever changing this file's binary layout. Property
// VALUES are always float; that's the one deliberate limitation (matches every other level format
// already in this codebase -- LevelData's META and TiledLoader's properties are both float-only
// too, since gameplay tuning values are overwhelmingly numeric).
//
// Layout (little-endian, native x86/x64 -- no byte-swapping, this format is not meant to be
// portable across architectures):
//   magic            char[4]            "TMAP"
//   version          uint16             currently 1, read-and-ignored today, reserved for
//                                        detecting a future incompatible layout change
//   width, height    uint16, uint16     map size in CELLS
//   tileWidth/Height uint16, uint16     cell size in PIXELS
//   sheetPathLen     uint16
//   sheetPath        char[sheetPathLen] UTF-8, NOT null-terminated -- which sprite sheet image
//                                        this level paints its spriteIndex values from
//   nameCount        uint16
//     nameLen        uint8
//     name           char[nameLen]      UTF-8 -- this entry's INDEX in the table is its ID,
//                                        referenced by every property below as a single byte
//   [cell records -- width*height of them, row-major, top-left to bottom-right]
//     spriteIndex    uint16             which cell in the sheet to draw; 0xFFFF = empty/no tile
//     behaviorType   uint8              see BinaryBehaviorType below (the canonical registry
//                                        the editor AND loader both reference -- never renumber)
//     propCount      uint8              0..kMaxPropertiesPerRecord
//     properties     (nameID: uint8, value: float)[propCount]
//   objectCount      uint16
//   [object records -- player/enemies/coins/platforms/emitters/anything not grid-locked]
//     x, y           float, float       world position
//     objectType     uint8              see BinaryObjectType below (same registry rule)
//     propCount      uint8
//     properties     (nameID: uint8, value: float)[propCount]
//
// Division of labor with the Stage-4 collision split in mind: CELLS feed the game's implicit
// tilemap (behaviorType -> the collision array + slope LUT; they must NOT become PhysicsWorld
// rows the way MapReader spawned them). OBJECTS are the things that DO become world rows --
// player, enemies, coins, moving platforms, plus emitter placements. The player object doubles
// as the spawn point; there is deliberately no separate header spawn field.

// ---- Canonical type values + standard property names ----
// The FORMAT doesn't interpret any of these -- they're bytes and floats on disk either way.
// But the editor and the loader are two separate consumers of the same file, and a typo'd
// property string between them fails SILENTLY to the default value, so both must reference
// these constants instead of retyping strings/magic numbers. Extend freely; never renumber
// existing values (saved levels reference them by value).

enum BinaryBehaviorType : uint8_t {
	BEHAVIOR_BACKGROUND      = 0,  // visual only, never enters collision
	BEHAVIOR_SOLID           = 1,
	BEHAVIOR_ONEWAY          = 2,  // stand on top, pass through from below
	BEHAVIOR_SLOPE_UP_RIGHT  = 3,
	BEHAVIOR_SLOPE_UP_LEFT   = 4,
	BEHAVIOR_SLOPE_DOWN_LEFT = 5,  // matches PlatformerPhysics2D::SlopeType's full set
	BEHAVIOR_SLOPE_DOWN_RIGHT= 6,
};

enum BinaryObjectType : uint8_t {
	OBJ_PLAYER       = 0,  // exactly one per level -- doubles as the spawn point (no separate header field)
	OBJ_ENEMY_PATROL = 1,
	OBJ_ENEMY_CHASE  = 2,
	OBJ_COIN         = 3,
	OBJ_PLATFORM     = 4,  // moving/falling platform: an OBJECT, not a cell -- it isn't grid-locked
	                       // once it moves. Its position IS targetA; targetB comes from properties.
	OBJ_EMITTER      = 5,  // GPU particle emitter placement
	OBJ_CHECKPOINT   = 6,
	OBJ_PORTAL       = 7,  // NOTE: a portal's target level is a STRING, which property values
	                       // can't hold -- v1 convention: kPortalTarget is an index into the
	                       // game's own level list. A string-value table is a v2 concern.
	OBJ_LEVEL_END    = 8,  // level progression trigger: collide with player to advance to next level
};

// Standard property names. Values are always float (see the header comment); booleans are
// 0.0/1.0, IDs/indices are whole floats (exact up to 2^24 -- plenty).
namespace BinaryProps {
	// What a cell BECOMES at load, as a float holding a BinaryObjectType value (see below).
	// Absent / < 0 = a plain tile (no spawn). Set on a cell to make it spawn a platform/
	// player/enemy/coin at that cell's position -- the per-type params below configure it.
	// The game-side loader iterates cells and, when this is present, spawns accordingly
	// (that loader path is the pending reconciliation -- see roadmap).
	inline constexpr const char* kEntityType  = "entityType";
	// cells (per-instance overrides -- absent means "use the game's default for this behavior")
	inline constexpr const char* kFriction    = "friction";
	inline constexpr const char* kRestitution = "restitution";
	inline constexpr const char* kHealth      = "health";      // destructible tiles; also enemy HP
	inline constexpr const char* kGravity     = "gravity";     // per-object gravity override
	// player
	inline constexpr const char* kJumpForce   = "jumpForce";
	// enemies
	inline constexpr const char* kSpeed       = "speed";
	inline constexpr const char* kPatrolLeft  = "patrolLeft";  // world-x turnaround bounds (patrol)
	inline constexpr const char* kPatrolRight = "patrolRight";
	inline constexpr const char* kChaseRadius = "chaseRadius"; // activation radius (chase)
	// platforms (targetA is implicit = the object's own x,y)
	inline constexpr const char* kTargetBX    = "targetBX";
	inline constexpr const char* kTargetBY    = "targetBY";
	inline constexpr const char* kMoveSpeed   = "moveSpeed";
	inline constexpr const char* kFallDelay   = "fallDelay";
	inline constexpr const char* kCanFall     = "canFall";     // bool
	inline constexpr const char* kIsTriggered = "isTriggered"; // bool -- only moves when triggered
	inline constexpr const char* kGroupID     = "groupID";     // absent = 101 (legacy shared group -- see LevelInstantiator)
	inline constexpr const char* kPlatformTimer = "timer";     // back-and-forth reversal pause, NOT fallDelay
	// emitters
	inline constexpr const char* kEffectID    = "effectID";
	inline constexpr const char* kSpawnRate   = "spawnRate";
	inline constexpr const char* kLifetime    = "lifetime";
	inline constexpr const char* kZLayer      = "zLayer";
	// portals
	inline constexpr const char* kPortalTarget = "portalTarget"; // index into the game's level list
	// bump blocks (Mario-style: hit from below). Two orthogonal bools + a count compose every
	// variant -- brick / ?-coin / ?-item / multi-coin brick -- no block-type enum needed.
	inline constexpr const char* kBreakable    = "breakable";    // bool -- bump destroys it (once empty)
	inline constexpr const char* kContains     = "contains";     // 0=nothing 1=coin(auto-collect) 2=red potion(+HP) 3=blue(1-up) 4=green(+time) 5=yellow(+coins)
	inline constexpr const char* kContainCount = "containCount"; // bumps that yield contents (default 1 when kContains set)
	// Which kEnemyDefs row an enemy cell spawns (EnemyDefs.h). Absent = legacy mapping from the
	// OBJ value (PATROL->0, CHASE->1), so old levels load unchanged; the editor writes it for
	// every enemy cell it saves.
	inline constexpr const char* kEnemyDef     = "enemyDef";
}

struct BinaryProperty { uint8_t nameID = 0; float value = 0.0f; };

struct BinaryCell {
	uint16_t spriteIndex = 0xFFFF;
	uint8_t behaviorType = 0;
	uint8_t textureSheetID = 0; // reserved for future multi-sheet support (v1 only has one sheet, so this is always 0)
	std::vector<BinaryProperty> properties;
};

struct BinaryObject {
	float x = 0.0f, y = 0.0f;
	uint8_t objectType = 0;
	std::vector<BinaryProperty> properties;
};

struct BinaryLevel {
	uint16_t width = 0, height = 0;
	uint16_t tileWidth = 16, tileHeight = 16;
	std::string sheetPath;
	std::vector<std::string> propertyNames; // index into this vector == that property's on-disk ID
	std::vector<BinaryCell> cells;          // width*height, row-major
	std::vector<BinaryObject> objects;

	BinaryCell& CellAt(int col, int row) { return cells[(size_t)row * width + col]; }
	const BinaryCell& CellAt(int col, int row) const { return cells[(size_t)row * width + col]; }

	// -1 if this name hasn't been used anywhere in the level yet.
	int FindPropertyID(const std::string& name) const {
		for (size_t i = 0; i < propertyNames.size(); ++i) if (propertyNames[i] == name) return (int)i;
		return -1;
	}
	// Returns the name's existing ID, or registers it as a new one and returns that. Capped at 256
	// distinct property names per level (ID is a uint8_t) -- plenty for tuning values, this isn't
	// meant to hold arbitrary per-cell strings/blobs.
	int GetOrAddPropertyID(const std::string& name) {
		int id = FindPropertyID(name);
		if (id != -1) return id;
		propertyNames.push_back(name);
		return (int)propertyNames.size() - 1;
	}

	// Reads a named property off a cell/object's own property list, resolving the name through
	// THIS level's table first. defaultValue if that record doesn't have this property set (same
	// "missing = fall back to a sane default" convention every level format in this codebase uses).
	float GetProperty(const std::vector<BinaryProperty>& props, const std::string& name, float defaultValue) const {
		int id = FindPropertyID(name);
		if (id == -1) return defaultValue;
		for (const auto& p : props) if (p.nameID == (uint8_t)id) return p.value;
		return defaultValue;
	}
	// Sets a named property on a cell/object's property list (adding the name to the table if it's
	// new), overwriting any existing value for that name. No-ops past kMaxPropertiesPerRecord --
	// caller should keep per-record property counts small by design, not rely on silent truncation.
	void SetProperty(std::vector<BinaryProperty>& props, const std::string& name, float value);
};

struct BinaryMapFormat
{
	// Sanity cap, not a layout constant (propCount is a uint8 on disk either way). 16, not 8:
	// a platform OBJECT alone carries 7 standard properties (targetB/speed/fall/trigger/group),
	// and 8 left zero headroom for anything per-instance on top.
	static constexpr uint8_t kMaxPropertiesPerRecord = 16;
	static constexpr uint16_t kFormatVersion = 1;

	static BinaryLevel Load(const std::wstring& path)
	{
		BinaryLevel level;
		std::ifstream file(path, std::ios::binary);
		if (!file.is_open()) return level; // caller decides how to handle a missing/empty level

		char magic[4] = {};
		file.read(magic, 4);
		if (file.gcount() != 4 || magic[0] != 'T' || magic[1] != 'M' || magic[2] != 'A' || magic[3] != 'P') {
			return BinaryLevel{}; // not one of our files -- fail empty, same as a missing one
		}

		uint16_t version = 0;
		ReadU16(file, version);
		if (version > kFormatVersion) {
			// A FUTURE layout read by this build wouldn't be garbage-detected by anything below
			// (the stream would just be misinterpreted field by field) -- refuse it outright.
			// Older versions (none exist yet) would be handled here with per-version branches.
			return BinaryLevel{};
		}

		ReadU16(file, level.width);
		ReadU16(file, level.height);
		ReadU16(file, level.tileWidth);
		ReadU16(file, level.tileHeight);

		uint16_t sheetPathLen = 0;
		ReadU16(file, sheetPathLen);
		level.sheetPath.resize(sheetPathLen);
		if (sheetPathLen > 0) file.read(level.sheetPath.data(), sheetPathLen);

		uint16_t nameCount = 0;
		ReadU16(file, nameCount);
		level.propertyNames.resize(nameCount);
		for (uint16_t i = 0; i < nameCount; ++i) {
			uint8_t nameLen = 0;
			file.read((char*)&nameLen, 1);
			std::string name(nameLen, '\0');
			if (nameLen > 0) file.read(name.data(), nameLen);
			level.propertyNames[i] = std::move(name);
		}

		size_t cellCount = (size_t)level.width * (size_t)level.height;
		level.cells.resize(cellCount);
		for (size_t i = 0; i < cellCount && file.good(); ++i) {
			ReadCellOrObjectProperties(file, level.cells[i].spriteIndex, level.cells[i].behaviorType, level.cells[i].properties);
		}

		uint16_t objectCount = 0;
		ReadU16(file, objectCount);
		level.objects.resize(objectCount);
		for (uint16_t i = 0; i < objectCount && file.good(); ++i) {
			BinaryObject& obj = level.objects[i];
			file.read((char*)&obj.x, sizeof(float));
			file.read((char*)&obj.y, sizeof(float));
			file.read((char*)&obj.objectType, 1);
			uint8_t propCount = 0;
			file.read((char*)&propCount, 1);
			obj.properties.resize(propCount);
			for (uint8_t p = 0; p < propCount; ++p) {
				file.read((char*)&obj.properties[p].nameID, 1);
				file.read((char*)&obj.properties[p].value, sizeof(float));
			}
		}

		return level;
	}

	static void Save(const std::wstring& path, const BinaryLevel& level)
	{
		std::ofstream file(path, std::ios::binary);
		if (!file.is_open()) return;

		file.write("TMAP", 4);
		WriteU16(file, kFormatVersion);

		WriteU16(file, level.width);
		WriteU16(file, level.height);
		WriteU16(file, level.tileWidth);
		WriteU16(file, level.tileHeight);

		WriteU16(file, (uint16_t)level.sheetPath.size());
		file.write(level.sheetPath.data(), level.sheetPath.size());

		WriteU16(file, (uint16_t)level.propertyNames.size());
		for (const auto& name : level.propertyNames) {
			uint8_t len = (uint8_t)std::min<size_t>(name.size(), 255);
			file.write((const char*)&len, 1);
			file.write(name.data(), len);
		}

		for (const auto& cell : level.cells) {
			WriteU16(file, cell.spriteIndex);
			file.write((const char*)&cell.behaviorType, 1);
			WriteProperties(file, cell.properties);
		}

		WriteU16(file, (uint16_t)level.objects.size());
		for (const auto& obj : level.objects) {
			file.write((const char*)&obj.x, sizeof(float));
			file.write((const char*)&obj.y, sizeof(float));
			file.write((const char*)&obj.objectType, 1);
			WriteProperties(file, obj.properties);
		}
	}

private:
	static void ReadU16(std::ifstream& file, uint16_t& out) { file.read((char*)&out, sizeof(uint16_t)); }
	static void WriteU16(std::ofstream& file, uint16_t value) { file.write((const char*)&value, sizeof(uint16_t)); }

	static void ReadCellOrObjectProperties(std::ifstream& file, uint16_t& spriteIndex, uint8_t& behaviorType,
	                                        std::vector<BinaryProperty>& properties)
	{
		ReadU16(file, spriteIndex);
		file.read((char*)&behaviorType, 1);
		uint8_t propCount = 0;
		file.read((char*)&propCount, 1);
		properties.resize(propCount);
		for (uint8_t p = 0; p < propCount; ++p) {
			file.read((char*)&properties[p].nameID, 1);
			file.read((char*)&properties[p].value, sizeof(float));
		}
	}

	static void WriteProperties(std::ofstream& file, const std::vector<BinaryProperty>& properties)
	{
		uint8_t propCount = (uint8_t)std::min<size_t>(properties.size(), kMaxPropertiesPerRecord);
		file.write((const char*)&propCount, 1);
		for (uint8_t p = 0; p < propCount; ++p) {
			file.write((const char*)&properties[p].nameID, 1);
			file.write((const char*)&properties[p].value, sizeof(float));
		}
	}
};

inline void BinaryLevel::SetProperty(std::vector<BinaryProperty>& props, const std::string& name, float value)
{
	int id = GetOrAddPropertyID(name);
	for (auto& p : props) {
		if (p.nameID == (uint8_t)id) { p.value = value; return; }
	}
	if (props.size() >= BinaryMapFormat::kMaxPropertiesPerRecord) return; // silently capped -- see the declaration's comment
	props.push_back(BinaryProperty{ (uint8_t)id, value });
}
