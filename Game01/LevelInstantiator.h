#pragma once
#include <vector>
#include <PhysicsWorld.h>
#include <PhysicsSystem.h>
#include <TileMap.h>
#include "EnemyAI.h"
#include "EnemyDefs.h"
#include "BinaryMapFormat.h"

// Canonical binary-level loader (SUPERSEDES BinaryMapReader.hpp -- that one used a stale
// behaviorType numbering where 2==Platform, but the format's BinaryBehaviorType has 2==ONEWAY,
// and it never filled the TileMap the sensor migration needs). Consumes a BinaryLevel (the
// TMAP format the editor writes) and builds the game world:
//
//   CELLS -> PlatformerPhysics2D::TileMap (implicit static collision) + transitional legacy tile rows.
//   A cell tagged with BinaryProps::kEntityType (set by the editor's inspector) spawns that
//     ENTITY (player/enemy/coin/platform) at the cell instead of being a static tile -- this
//     is the "everything is a cell with metadata" model. Works on AIR cells too (a player
//     spawn is usually on empty space: spriteIndex 0xFFFF but kEntityType set).
//   OBJECTS[] (if any) spawn the same entity types via the same spawners -- kept for backward
//     compat; the editor writes cell-entities, but object-based levels still load.
//
// Returns playerIdx; fills platforms/enemies/collectibles. Registers NO callbacks (the scene
// does that after -- callbacks need its EmitterTable). Uses the named BEHAVIOR_*/OBJ_*
// constants throughout, never magic numbers, so the editor and loader can't drift.
struct LevelInstantiator
{
	static int InstantiateLevel(const BinaryLevel& lvl, PlatformerPhysics2D::PhysicsContext& ctx,
	                             PlatformerPhysics2D::TileMap& tiles,
	                             std::vector<int>& platforms, std::vector<EnemyInstance>& enemies,
	                             std::vector<int>& collectibles, std::vector<int>& levelEnd, float& outMapBottomY,
	                             uint8_t startingHealth = 3)
	{
		using namespace PlatformerPhysics2D;
		int playerIdx = -1;
		if (lvl.width == 0 || lvl.height == 0) return playerIdx; // failed/empty load

		const float tileSize = (float)lvl.tileWidth;
		const float halfTile = tileSize * 0.5f;
		tiles.Init(lvl.width, lvl.height, tileSize, 0.0f, 0.0f, /*defaultFriction=*/0.15f);

		// Resolve the hot per-cell property IDs once (GetProperty re-scans the name table).
		const int idFriction    = lvl.FindPropertyID(BinaryProps::kFriction);
		const int idRestitution = lvl.FindPropertyID(BinaryProps::kRestitution);
		const int idEntityType  = lvl.FindPropertyID(BinaryProps::kEntityType);
		auto propOf = [](const std::vector<BinaryProperty>& props, int nameID, float def) {
			if (nameID < 0) return def;
			for (const auto& p : props) if (p.nameID == (uint8_t)nameID) return p.value;
			return def;
		};
		auto get = [&](const std::vector<BinaryProperty>& props, const char* n, float d) {
			return lvl.GetProperty(props, n, d);
		};

		// ---- per-type entity spawners (shared by the CELL path and the OBJECTS[] path) ----
		auto spawnPlayer = [&](float x, float y, const std::vector<BinaryProperty>& props) {
			// 16x16 hitbox, deliberately smaller than the drawn sprite (slope-clamp reasons).
			int idx = ctx.world.spawn(ShapeType::RECT, Vec2{ 16.0f, 16.0f }, x, y, 0.0f, 0.0f, 1.0f,
			                           981.0f, 1 << 1, 1 << 1, true, false, false);
			if (idx == -1) return;
			ctx.world.tileID[idx] = 1;
			int es = ctx.world.entitySlot[idx];
			ctx.entities.isPlayerControlled[es] = 1;
			ctx.entities.jumpForce[es] = get(props, BinaryProps::kJumpForce, 420.0f);
			ctx.entities.health[es] = startingHealth;
			playerIdx = idx;
		};
		// enemyDefIdx = index into kEnemyDefs (EnemyDefs.h). tileID = kEnemyTileIDBase + index is
		// what Draw's single table-driven enemy branch dispatches on; speed/health defaults come
		// from the table (per-cell props still override).
		auto spawnEnemy = [&](int enemyDefIdx, float x, float y, const std::vector<BinaryProperty>& props) {
			if (enemyDefIdx < 0 || enemyDefIdx >= kEnemyDefCount) return;
			const EnemyDef& def = kEnemyDefs[enemyDefIdx];
			int idx = ctx.world.spawn(ShapeType::RECT, Vec2{ def.hitW, def.hitH }, x, y, 0.0f, 0.0f, 1.0f,
			                           def.gravity, 1 << 1, 1 << 1, true, false, false);
			if (idx == -1) return;
			ctx.world.tileID[idx] = kEnemyTileIDBase + (uint32_t)enemyDefIdx;
			int es = ctx.world.entitySlot[idx];
			ctx.entities.health[es] = (uint8_t)get(props, BinaryProps::kHealth, def.health);
			ctx.entities.moveSpeed[es] = get(props, BinaryProps::kSpeed, def.speed);
			float aiParam = 150.0f;
			switch (def.ai) {
			case EnemyAIType::Patrol:
				ctx.entities.targetX[es] = x + get(props, BinaryProps::kPatrolLeft, -48.0f);  // OFFSET from spawn
				ctx.entities.targetY[es] = x + get(props, BinaryProps::kPatrolRight, 48.0f);
				ctx.world.velX[idx] = ctx.entities.moveSpeed[es]; // seed direction
				break;
			case EnemyAIType::Chase:
				ctx.entities.targetY[es] = get(props, BinaryProps::kChaseRadius, 150.0f);
				break;
			case EnemyAIType::Flyer:
				// targetX/Y = spawn ANCHOR (home position to hover at); activation radius rides
				// on the EnemyInstance since both target columns are taken.
				ctx.entities.targetX[es] = x;
				ctx.entities.targetY[es] = y;
				aiParam = get(props, BinaryProps::kChaseRadius, 150.0f);
				break;
			case EnemyAIType::Shooter:
				aiParam = get(props, BinaryProps::kChaseRadius, 180.0f); // fire-activation radius
				break;
			case EnemyAIType::Faller:
				// Anchor = spawn (hangs there, returns there); aiParam = horizontal HALF-WIDTH of
				// the under-me trigger band (Chase Radius field in the editor, repurposed).
				ctx.entities.targetX[es] = x;
				ctx.entities.targetY[es] = y;
				aiParam = get(props, BinaryProps::kChaseRadius, 24.0f);
				break;
			case EnemyAIType::Bomb:
				// Patrol bounds, same OFFSET-from-spawn convention as Patrol above; radius rides in
				// aiParam (Chase Radius field in the editor). Default trigger is deliberately small --
				// the blast radius is 80 game-side, and a trigger much larger than the blast means it
				// always detonates harmlessly before the player is in danger.
				ctx.entities.targetX[es] = x + get(props, BinaryProps::kPatrolLeft, -48.0f);
				ctx.entities.targetY[es] = x + get(props, BinaryProps::kPatrolRight, 48.0f);
				ctx.world.velX[idx] = ctx.entities.moveSpeed[es]; // seed direction
				aiParam = get(props, BinaryProps::kChaseRadius, 60.0f);
				break;
			case EnemyAIType::FlyerBomb:
				// Same anchor convention as Flyer (hover home in targetX/Y); aiParam = ACTIVATION
				// radius (start chasing). The detonate range is a small fixed constant in the AI.
				ctx.entities.targetX[es] = x;
				ctx.entities.targetY[es] = y;
				aiParam = get(props, BinaryProps::kChaseRadius, 150.0f);
				break;
			}
			enemies.push_back({ idx, def.ai, aiParam });
		};
		auto spawnCoin = [&](float x, float y, const std::vector<BinaryProperty>&) {
			int idx = ctx.world.spawn(ShapeType::RECT, Vec2{ 16.0f, 16.0f }, x, y, 0.0f, 0.0f, 1.0f,
			                           0.0f, 1 << 1, 1 << 1, true, false, false);
			if (idx == -1) return;
			ctx.world.tileID[idx] = 7;
			collectibles.push_back(idx);
		};
		auto spawnLevelEnd = [&](float x, float y, const std::vector<BinaryProperty>&) {
			// Level-end trigger: sensor box that player collides with to progress to next level.
			// No physics response needed (sensor=true), just visibility for collision detection.
			int idx = ctx.world.spawn(ShapeType::RECT, Vec2{ 16.0f, 16.0f }, x, y, 0.0f, 0.0f, 1.0f,
			                           0.0f, 1 << 1, 1 << 1, true, false, false);
			if (idx == -1) return;
			ctx.world.tileID[idx] = 9;  // tileID 9 = level-end trigger (unique to distinguish in Draw)
			levelEnd.push_back(idx);
		};
		auto spawnPlatform = [&](float x, float y, const std::vector<BinaryProperty>& props, uint16_t spriteIndex) {
			int idx = ctx.world.spawn(ShapeType::RECT, Vec2{ 16.0f, 8.0f }, x, y, 0.0f, 0.0f, 1.0f,
			                           0.0f, 1 << 1, 1 << 1, false, true, false);
			if (idx == -1) return;
			ctx.world.isTile[idx] = 1;
			// Store the authored SPRITE in tileID (game-owned render field; physics never reads it).
			// A platform MOVES, so its sprite can't be recovered by position like static tiles are --
			// GameplayScene::Draw detects platforms via world.isPlatform and reads the sprite here.
			ctx.world.tileID[idx] = spriteIndex;
			// Default matches ground tiles AND what the editor's Friction field displays (0.15).
			// The old 0.01 was MapReader's "slippery moving platform" legacy -- with the
			// friction-scaled player brake, it silently made every un-edited platform ice while
			// the editor showed 0.15. Author a LOWER value per-platform when ice is intended.
			ctx.world.friction[idx] = get(props, BinaryProps::kFriction, 0.15f);
			ctx.world.restitution[idx] = get(props, BinaryProps::kRestitution, 0.01f);
			ctx.world.size[idx] = { 16.0f, 16.0f };
			int ps = ctx.world.platformSlot[idx];
			// targetB ABSOLUTE; default = own position (stationary). targetA = placement pos.
			ctx.platforms.targetA[ps] = Vec2{ x, y };
			ctx.platforms.targetB[ps] = Vec2{ get(props, BinaryProps::kTargetBX, x),
			                                  get(props, BinaryProps::kTargetBY, y) };
			ctx.platforms.moveSpeed[ps] = get(props, BinaryProps::kMoveSpeed, 64.0f);
			ctx.platforms.groupID[ps] = (int)get(props, BinaryProps::kGroupID, 101.0f);
			ctx.platforms.platformTimer[ps] = get(props, BinaryProps::kPlatformTimer, 5.0f);
			ctx.platforms.isTriggered[ps] = (uint8_t)get(props, BinaryProps::kIsTriggered, 1.0f);
			ctx.platforms.movingForward[ps] = 1;
			ctx.platforms.canFall[ps] = (uint8_t)get(props, BinaryProps::kCanFall, 0.0f);
			ctx.platforms.fallDelay[ps] = get(props, BinaryProps::kFallDelay, 5.0f);
			platforms.push_back(idx);
		};
		auto spawnEntity = [&](int entType, float x, float y, const std::vector<BinaryProperty>& props, uint16_t spriteIndex) {
			switch ((BinaryObjectType)entType) {
			case OBJ_PLAYER:       spawnPlayer(x, y, props); break;
			// kEnemyDef property picks the kEnemyDefs row; the OBJ value only supplies the
			// legacy default (old levels saved before the property existed).
			case OBJ_ENEMY_PATROL: spawnEnemy((int)get(props, BinaryProps::kEnemyDef, 0.0f), x, y, props); break;
			case OBJ_ENEMY_CHASE:  spawnEnemy((int)get(props, BinaryProps::kEnemyDef, 1.0f), x, y, props); break;
			case OBJ_COIN:         spawnCoin(x, y, props); break;
			case OBJ_PLATFORM:     spawnPlatform(x, y, props, spriteIndex); break; // only platforms use the sprite
			case OBJ_LEVEL_END:    spawnLevelEnd(x, y, props); break;
			default: break; // emitter/checkpoint/portal: no gameplay yet
			}
		};

		// ---- CELLS ----
		for (int row = 0; row < (int)lvl.height; ++row) {
			for (int col = 0; col < (int)lvl.width; ++col) {
				const BinaryCell& cell = lvl.CellAt(col, row);
				int entType = (int)propOf(cell.properties, idEntityType, -1.0f);

				// Truly empty ONLY if no sprite AND no entity -- an air cell can still carry a
				// spawn (player/coin often sit on empty space).
				if (cell.spriteIndex == 0xFFFF && entType < 0) continue;

				const float x = (col * tileSize) + halfTile;
				const float y = (row * tileSize) + halfTile;

				// Entity cell -> spawn the dynamic entity here; it is NOT a static tile.
				// EXCEPTION: a level-end trigger is a pure sensor, so a sprite painted on the
				// same cell (door, flag, archway) still spawns as its normal static tile below --
				// the trigger and the decoration coexist. Other entity types keep the either/or
				// rule: their sprite IS the entity's look (platforms) or the entity visibly
				// replaces the cell (player/enemy/coin).
				if (entType >= 0) {
					spawnEntity(entType, x, y, cell.properties, cell.spriteIndex);
					if ((BinaryObjectType)entType != OBJ_LEVEL_END || cell.spriteIndex == 0xFFFF)
						continue;
				}

				// Static tile -> TileMap (permanent) + legacy row (transitional).
				const float friction = propOf(cell.properties, idFriction, 0.15f);
				const float restitution = propOf(cell.properties, idRestitution, 0.15f);
				size_t cellIdx = (size_t)row * lvl.width + col;
				tiles.behavior[cellIdx] = (TileBehavior)cell.behaviorType;
				tiles.friction[cellIdx] = friction;

				switch ((BinaryBehaviorType)cell.behaviorType) {
				case BEHAVIOR_BACKGROUND: {
					int idx = ctx.world.spawn(ShapeType::RECT, Vec2{ tileSize, tileSize }, x, y, 0.0f, 0.0f, 1.0f,
					                           0.0f, 0, 0, false, false, false);
					if (idx == -1) break;
					ctx.world.isTile[idx] = 1;
					ctx.world.tileID[idx] = 0;
					break;
				}
				case BEHAVIOR_SOLID:
				case BEHAVIOR_ONEWAY: { // transitional: one-way statics behave as solid until sensors read the TileMap
					int idx = ctx.world.spawn(ShapeType::RECT, Vec2{ tileSize, tileSize }, x, y, 0.0f, 0.0f, 1.0f,
					                           0.0f, 1 << 1, 1 << 1, false, false, false);
					if (idx == -1) break;
					ctx.world.isTile[idx] = 1;
					ctx.world.tileID[idx] = 2;
					ctx.world.friction[idx] = friction;
					ctx.world.restitution[idx] = restitution;
					tiles.rowHandles[cellIdx] = ctx.world.handleOf(idx);
					break;
				}
				case BEHAVIOR_SLOPE_UP_RIGHT:
				case BEHAVIOR_SLOPE_UP_LEFT:
				case BEHAVIOR_SLOPE_DOWN_LEFT:
				case BEHAVIOR_SLOPE_DOWN_RIGHT: {
					int idx = ctx.world.spawn(ShapeType::RECT, Vec2{ tileSize, tileSize }, x, y, 0.0f, 0.0f, 1.0f,
					                           0.0f, 1 << 1, 1 << 1, false, false, false);
					if (idx == -1) break;
					ctx.world.isTile[idx] = 1;
					ctx.world.isSlope[idx] = 1;
					ctx.world.slopeType[idx] = tiles.SlopeAt(x, y);
					ctx.world.tileID[idx] =
						(cell.behaviorType == BEHAVIOR_SLOPE_UP_RIGHT || cell.behaviorType == BEHAVIOR_SLOPE_DOWN_RIGHT) ? 4 : 5;
					ctx.world.friction[idx] = friction;
					ctx.world.restitution[idx] = restitution;
					tiles.rowHandles[cellIdx] = ctx.world.handleOf(idx);
					break;
				}
				default: break;
				}
			}
		}

		// ---- OBJECTS[] (backward compat; editor writes cell-entities instead) ----
		// Objects carry no sprite (0xFFFF) -- object-based platforms fall back to sheet cell 0.
		for (const BinaryObject& obj : lvl.objects)
			spawnEntity(obj.objectType, obj.x, obj.y, obj.properties, 0xFFFF);

		// Calculate map bottom edge for fall-death detection
		outMapBottomY = lvl.height * lvl.tileWidth;

		return playerIdx;
	}
};
