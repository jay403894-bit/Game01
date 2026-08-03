#pragma once
#include <cstdint>
#include <string>
#include <vector>

#pragma pack(push, 1)
struct Vec2 {
    float x;
    float y;
};

struct WorldFileHeader {
    char magic[4] = { 'J', 'M', 'A', 'P' }; // Custom format identifier
    uint32_t version = 1;
};

struct BinaryChunkHeader {
    char chunkID[4];     // "PLET" (Palette), "LAYR" (Tiles), "PHYS" (Physics), "DATB" (Data Tables)
    uint64_t chunkSize;  // 64-bit size allows files to grow without artificial limits
};
#pragma pack(pop)

// --- Runtime Structs used by your ImGui Editor and Serialization ---

struct TileTypeDescriptor {
    uint16_t id;
    std::string assetPath; // e.g., "assets/sprites/terrain/grass.png"
    bool isCollidable;
};

struct TileLayer {
    std::string layerName;  // "Background", "Foreground", "Hazards"
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint16_t> tileData; // Indigo-flexible 16-bit array = 65,535 possible unique tiles
};

struct PhysicsHullRaw {
    uint32_t shapeType;    // 0 = AABB Box, 1 = Circle, 2 = Capsule (Matching PlatformerPhysics2D)
    Vec2 position;
    Vec2 extents;          // radius, half-width, half-height
    uint32_t layerMask;    // Collision filtering masks
};

struct DataRowRaw {
    std::string tableType; // "PLATFORM", "ENTITY_SPAWN"
    std::string typeID;    // e.g., "ENEMY_GOBLIN", "PLATFORM_MOVING_ICE"
    Vec2 position;
    std::string propertiesJson; // Flexible, stringified schema properties for infinite custom options
};