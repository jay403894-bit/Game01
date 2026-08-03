#pragma once
// SUPERSEDED 7-15 by LevelInstantiator.h -- do not use for new work. This version used a
// stale behaviorType numbering (2==Platform, but the format's BinaryBehaviorType has 2==ONEWAY),
// read entities only from objects[] (not the editor's cell kEntityType metadata), and never
// filled the PlatformerPhysics2D::TileMap the sensor migration needs. LevelInstantiator::InstantiateLevel
// is the canonical loader now; kept here only until every caller is confirmed off it.
#include <vector>
#include <string>
#include <PhysicsWorld.h>
#include <PhysicsSystem.h>
#include "EnemyAI.h"
#include "BinaryMapFormat.h"

struct BinaryMapReader
{
    static int BuildWorldFromBinaryMap(PlatformerPhysics2D::PhysicsContext& ctx, const BinaryLevel& level,
        std::vector<int>& platforms, std::vector<EnemyInstance>& enemies,
        std::vector<int>& collectibles)
    {
        using namespace PlatformerPhysics2D;
        int playerIdx = -1;
        const float tileSize = 16.0f;
        const float halfTile = tileSize * 0.5f;

        // 1. Load Grid-Based Cells (Solid Blocks, Slopes, and Platforms)
        for (int row = 0; row < level.height; ++row) {
            for (int col = 0; col < level.width; ++col) {
                const BinaryCell& cell = level.CellAt(col, row);
                if (cell.spriteIndex == 0xFFFF) continue; // Skip empty air tiles

                float x = (col * tileSize) + halfTile;
                float y = (row * tileSize) + halfTile;

                // Behavior Definitions: 
                // 0 = Background, 1 = Solid Block, 2 = Moving Platform, 3 = SlopeRight, 4 = SlopeLeft
                if (cell.behaviorType == 1) { // Solid Wall
                    int idx = ctx.world.spawn(ShapeType::RECT, Vec2{ tileSize, tileSize }, x, y, 0.0f, 0.0f, 1.0f,
                        0.0f, 1 << 1, 1 << 1, false, false, false);
                    if (idx == -1) continue;
                    ctx.world.isTile[idx] = 1;
                    ctx.world.tileID[idx] = 2; // Dark Gray color mapping
                    ctx.world.isSlope[idx] = 0;
                    ctx.world.slopeType[idx] = SlopeType::NONE;
                    ctx.world.friction[idx] = 0.15f;
                    ctx.world.restitution[idx] = 0.15f;
                }
                else if (cell.behaviorType == 2) { // Moving Platform
                    int idx = ctx.world.spawn(ShapeType::RECT, Vec2{ 16.0f, 8.0f }, x, y, 0.0f, 0.0f, 1.0f,
                        0.0f, 1 << 1, 1 << 1, false, true, false);
                    if (idx == -1) continue;
                    ctx.world.isTile[idx] = 1;
                    ctx.world.tileID[idx] = 3; // Platform orange color mapping
                    ctx.world.friction[idx] = 0.01f;
                    ctx.world.restitution[idx] = 0.01f;
                    ctx.world.size[idx] = { 16.0f, 16.0f };

                    int ps = ctx.world.platformSlot[idx];

                    // Pull custom metadata strings securely via float values
                    ctx.platforms.groupID[ps] = (int)level.GetProperty(cell.properties, "groupID", 101.0f);
                    ctx.platforms.platformTimer[ps] = level.GetProperty(cell.properties, "timer", 5.0f);
                    ctx.platforms.isTriggered[ps] = 1;
                    ctx.platforms.movingForward[ps] = 1;
                    ctx.platforms.moveSpeed[ps] = level.GetProperty(cell.properties, "speed", 64.0f);

                    // Rebuild target path vector offsets
                    ctx.platforms.targetA[ps] = Vec2{ x, y };
                    float targetOffsetX = level.GetProperty(cell.properties, "targetOffsetX", 0.0f);
                    float targetOffsetY = level.GetProperty(cell.properties, "targetOffsetY", -300.0f);
                    ctx.platforms.targetB[ps] = Vec2{ x + targetOffsetX, y + targetOffsetY };

                    ctx.platforms.canFall[ps] = (uint8_t)level.GetProperty(cell.properties, "canFall", 0.0f);
                    ctx.platforms.fallDelay[ps] = level.GetProperty(cell.properties, "fallDelay", 5.0f);

                    platforms.push_back(idx);
                }
                else if (cell.behaviorType == 3 || cell.behaviorType == 4) { // Slopes
                    int idx = ctx.world.spawn(ShapeType::RECT, Vec2{ tileSize, tileSize }, x, y, 0.0f, 0.0f, 1.0f,
                        0.0f, 1 << 1, 1 << 1, false, false, false);
                    if (idx == -1) continue;
                    ctx.world.isTile[idx] = 1;
                    ctx.world.tileID[idx] = (cell.behaviorType == 3) ? 4 : 5;
                    ctx.world.isSlope[idx] = 1;
                    ctx.world.slopeType[idx] = (cell.behaviorType == 3) ? SlopeType::UP_RIGHT : SlopeType::UP_LEFT;
                    ctx.world.friction[idx] = 0.15f;
                    ctx.world.restitution[idx] = 0.15f;
                }
                else if (cell.behaviorType == 0) { // Background Detail
                    int idx = ctx.world.spawn(ShapeType::RECT, Vec2{ tileSize, tileSize }, x, y, 0.0f, 0.0f, 1.0f,
                        0.0f, 0, 0, false, false, false);
                    if (idx == -1) continue;
                    ctx.world.isTile[idx] = 1;
                    ctx.world.tileID[idx] = 0; // Gray inert background color
                }
            }
        }

        // 2. Load Free-Floating Objects (Player Spawn, Enemies, Pickups)
        for (const auto& obj : level.objects) {
            // ObjectType definitions: 0 = Player, 1 = PatrolEnemy, 2 = ChaseEnemy, 3 = Coin
            if (obj.objectType == 0) { // Player Knight
                int idx = ctx.world.spawn(ShapeType::RECT, Vec2{ 16.0f, 16.0f }, obj.x, obj.y, 0.0f, 0.0f, 1.0f,
                    981.0f, 1 << 1, 1 << 1, true, false, false);
                if (idx == -1) continue;
                ctx.world.tileID[idx] = 1;
                int es = ctx.world.entitySlot[idx];
                ctx.entities.isPlayerControlled[es] = 1;
                ctx.entities.jumpForce[es] = level.GetProperty(obj.properties, "jumpForce", 420.0f);
                ctx.entities.health[es] = (uint8_t)level.GetProperty(obj.properties, "health", 3.0f);

                // Add explicit Grid Movement parameters!
                ctx.entities.gridMovement[es] = (uint8_t)level.GetProperty(obj.properties, "gridMovement", 0.0f);
                ctx.entities.moveSpeed[es] = level.GetProperty(obj.properties, "speed", 128.0f);

                playerIdx = idx;
            }
            else if (obj.objectType == 1 || obj.objectType == 2) { // Slime Enemies
                int idx = ctx.world.spawn(ShapeType::RECT, Vec2{ 16.0f, 16.0f }, obj.x, obj.y, 0.0f, 0.0f, 1.0f,
                    981.0f, 1 << 1, 1 << 1, true, false, false);
                if (idx == -1) continue;
                ctx.world.tileID[idx] = (obj.objectType == 1) ? 6 : 8;
                int es = ctx.world.entitySlot[idx];
                ctx.entities.health[es] = (uint8_t)level.GetProperty(obj.properties, "health", 3.0f);

                if (obj.objectType == 1) { // Patrol Type
                    float patrolLeft = level.GetProperty(obj.properties, "patrolLeft", -48.0f);
                    float patrolRight = level.GetProperty(obj.properties, "patrolRight", 48.0f);
                    ctx.entities.targetX[es] = obj.x + patrolLeft;
                    ctx.entities.targetY[es] = obj.x + patrolRight;
                    ctx.entities.moveSpeed[es] = level.GetProperty(obj.properties, "speed", 40.0f);
                    ctx.world.velX[idx] = ctx.entities.moveSpeed[es];
                    enemies.push_back({ idx, EnemyAIType::Patrol });
                }
                else { // Chase Type
                    ctx.entities.targetY[es] = level.GetProperty(obj.properties, "chaseRadius", 150.0f);
                    ctx.entities.moveSpeed[es] = level.GetProperty(obj.properties, "speed", 70.0f);
                    enemies.push_back({ idx, EnemyAIType::Chase });
                }
            }
            else if (obj.objectType == 3) { // Coins
                int idx = ctx.world.spawn(ShapeType::RECT, Vec2{ 16.0f, 16.0f }, obj.x, obj.y, 0.0f, 0.0f, 1.0f,
                    0.0f, 1 << 1, 1 << 1, true, false, false);
                if (idx == -1) continue;
                ctx.world.tileID[idx] = 7;
                collectibles.push_back(idx);
            }
        }

        return playerIdx;
    }
};