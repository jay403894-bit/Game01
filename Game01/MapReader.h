#pragma once
#include <vector>
#include <string>
#include <PhysicsWorld.h>
#include <PhysicsSystem.h>
#include "EnemyAI.h"
#include "LevelData.h"

// Parses a LevelData (see LevelData.h) into PhysicsWorld cells:
//   '#' solid tile, '_' moving platform (all '_' pieces default to groupID 101 so
//   PhysicsSystem::LinkGroups treats them as one group -- override per-cell via META's groupID
//   to make independent platform groups), '/' slope rising left-to-right (SlopeType::UP_RIGHT),
//   '\' slope rising right-to-left (SlopeType::UP_LEFT), '.' inert background tile (layer/mask 0
//   -- never collides), '@' the player, 'E' a patrolling enemy, 'C' a chasing enemy (see
//   EnemyAI.h's UpdateEnemyAI for what each EnemyAIType actually does -- adding a third type only
//   needs a new case there + a new char here), '$' a collectible coin (static, no gravity -- just
//   sits there until the player touches it; see GameplayScene's constructor for the pickup
//   onCollision callback, same reason enemies' stomp callback lives there instead of here: it
//   needs GameplayScene's EmitterTable/coin counter).
//
// Every numeric detail that used to be a hardcoded constant here (platform speed/group/target,
// patrol bounds, chase radius/speed) is now a META override with that same old hardcoded value
// as its DEFAULT -- a level with no META section at all behaves identically to the original
// inline LEVEL_1 array. See LevelData.h's LevelFormat comment for the actual file syntax.
struct MapReader
{
	static int BuildWorldFromMap(PlatformerPhysics2D::PhysicsContext& ctx, const LevelData& level,
	                              std::vector<int>& platforms, std::vector<EnemyInstance>& enemies,
	                              std::vector<int>& collectibles)
	{
		using namespace PlatformerPhysics2D;
		int playerIdx = -1;
		const float tileSize = 16.0f;
		const float halfTile = tileSize * 0.5f;
		const std::vector<std::string>& mapData = level.tiles;

		for (int row = 0; row < (int)mapData.size(); ++row) {
			for (int col = 0; col < (int)mapData[row].size(); ++col) {
				char tileType = mapData[row][col];
				float x = (col * tileSize) + halfTile;
				float y = (row * tileSize) + halfTile;
				const TileMeta* meta = level.MetaAt(col, row);

				if (tileType == '#') {
					int idx = ctx.world.spawn(ShapeType::RECT, Vec2{ tileSize, tileSize }, x, y, 0.0f, 0.0f, 1.0f,
					                           0.0f, 1 << 1, 1 << 1, false, false, false);
					if (idx == -1) continue;
					ctx.world.isTile[idx] = 1;
					ctx.world.tileID[idx] = 2;
					ctx.world.isSlope[idx] = 0;
					ctx.world.slopeType[idx] = SlopeType::NONE;
					ctx.world.friction[idx] = 0.15f;
					ctx.world.restitution[idx] = 0.15f;
				}
				else if (tileType == '_') {
					int idx = ctx.world.spawn(ShapeType::RECT, Vec2{ 16.0f, 8.0f }, x, y, 0.0f, 0.0f, 1.0f,
					                           0.0f, 1 << 1, 1 << 1, false, true, false);
					if (idx == -1) continue;
					ctx.world.isTile[idx] = 1;
					ctx.world.tileID[idx] = 3;
					ctx.world.friction[idx] = 0.01f;
					ctx.world.restitution[idx] = 0.01f;
					ctx.world.size[idx] = { 16.0f, 16.0f };

					int ps = ctx.world.platformSlot[idx];
					// groupID: independent moving-platform groups -- give two separate platform
					// strips different groupIDs in META and they move independently instead of
					// being forced into one shared group just because both used '_'.
					ctx.platforms.groupID[ps] = meta ? (int)meta->Get("groupID", 101.0f) : 101;
					ctx.platforms.platformTimer[ps] = meta ? meta->Get("timer", 5.0f) : 5.0f;
					ctx.platforms.isTriggered[ps] = 1;
					ctx.platforms.movingForward[ps] = 1;
					ctx.platforms.moveSpeed[ps] = meta ? meta->Get("speed", 64.0f) : 64.0f;
					ctx.platforms.targetA[ps] = Vec2{ x, y };
					float targetOffsetX = meta ? meta->Get("targetOffsetX", 0.0f) : 0.0f;
					float targetOffsetY = meta ? meta->Get("targetOffsetY", -300.0f) : -300.0f;
					ctx.platforms.targetB[ps] = Vec2{ x + targetOffsetX, y + targetOffsetY };
					// canFall: 0 (default, matches the source map's commented-out line) = a normal
					// back-and-forth platform. Nonzero = it can DETACH and fall once triggered (see
					// PhysicsSystem::UpdatePlatforms/PlatformPhysics for the actual fall/detach
					// logic) -- PlatformerPhysics2D already fully implements this, MapReader just never had a
					// way to opt a platform into it until now.
					ctx.platforms.canFall[ps] = meta ? (uint8_t)meta->Get("canFall", 0.0f) : 0;
					// fallDelay: how long after being stepped on before it starts shaking/falling --
					// INDEPENDENT of `timer` (which still only controls the back-and-forth reversal
					// pause). Only matters if canFall is set; harmless to set on a non-falling
					// platform. Defaults to 5, not PlatformTable's own 0-default -- an unconfigured
					// canFall platform shaking/falling almost instantly the moment it's touched
					// would be a strange default for a level author who just wanted SOME delay.
					ctx.platforms.fallDelay[ps] = meta ? meta->Get("fallDelay", 5.0f) : 5.0f;

					platforms.push_back(idx);
				}
				else if (tileType == '/' || tileType == '\\') {
					int idx = ctx.world.spawn(ShapeType::RECT, Vec2{ tileSize, tileSize }, x, y, 0.0f, 0.0f, 1.0f,
					                           0.0f, 1 << 1, 1 << 1, false, false, false);
					if (idx == -1) continue;
					ctx.world.isTile[idx] = 1;
					ctx.world.tileID[idx] = (tileType == '/') ? 4 : 5;
					ctx.world.isSlope[idx] = 1;
					ctx.world.slopeType[idx] = (tileType == '/') ? SlopeType::UP_RIGHT : SlopeType::UP_LEFT;
					ctx.world.friction[idx] = 0.15f;
					ctx.world.restitution[idx] = 0.15f;
				}
				else if (tileType == '.') {
					int idx = ctx.world.spawn(ShapeType::RECT, Vec2{ tileSize, tileSize }, x, y, 0.0f, 0.0f, 1.0f,
					                           0.0f, 0, 0, false, false, false);
					if (idx == -1) continue;

					ctx.world.isTile[idx] = 1;
					ctx.world.tileID[idx] = 0;
				}
				else if (tileType == 'E' || tileType == 'C') {
					int idx = ctx.world.spawn(ShapeType::RECT, Vec2{ 16.0f, 16.0f }, x, y, 0.0f, 0.0f, 1.0f,
					                           981.0f, 1 << 1, 1 << 1, true, false, false);
					if (idx == -1) continue;
					// Distinct tileIDs (not just one shared "enemy" ID) so GameplayScene::Draw() can
					// pick the right slime sheet/color per row directly, no lookup into `enemies`
					// needed -- 6=patrol (green), 8=chase (purple). 7 is already taken by coin ('$').
					ctx.world.tileID[idx] = (tileType == 'E') ? 6 : 8;
					int es = ctx.world.entitySlot[idx];
					ctx.entities.health[es] = meta ? (uint8_t)meta->Get("health", 3.0f) : 3;
					// isPlayerControlled left false -- UpdateEnemyAI (see EnemyAI.h) drives velX
					// directly instead of reading input. EntityTable's targetX/targetY/moveSpeed are
					// storage only here; what they MEAN depends entirely on the type tag below.
					if (tileType == 'E') {
						float patrolLeft = meta ? meta->Get("patrolLeft", -48.0f) : -48.0f;
						float patrolRight = meta ? meta->Get("patrolRight", 48.0f) : 48.0f;
						ctx.entities.targetX[es] = x + patrolLeft;  // patrol left bound
						ctx.entities.targetY[es] = x + patrolRight; // patrol right bound
						ctx.entities.moveSpeed[es] = meta ? meta->Get("speed", 40.0f) : 40.0f;
						// UpdateEnemyAI's Patrol branch only flips velX once posX reaches a bound --
						// spawning exactly at the CENTER (neither bound) with the default velX=0 from
						// spawn() means it never had a reason to start moving at all. Seed an initial
						// direction here.
						ctx.world.velX[idx] = ctx.entities.moveSpeed[es];
						enemies.push_back({ idx, EnemyAIType::Patrol });
					} else {
						ctx.entities.targetY[es] = meta ? meta->Get("chaseRadius", 150.0f) : 150.0f;
						ctx.entities.moveSpeed[es] = meta ? meta->Get("speed", 70.0f) : 70.0f;
						enemies.push_back({ idx, EnemyAIType::Chase });
					}
					// Stomp-to-kill's onCollision callback is registered by GameplayScene's
					// constructor AFTER this returns, not here -- it needs to spawn a dust burst on
					// the stomp, which means reaching GameplayScene's own EmitterTable, and
					// MapReader has no access to that (nor should it -- stays render/effects-
					// agnostic, same reason it never touches Renderer2D directly).
				}
				else if (tileType == '$') {
					// gravity=0.0f -- unlike enemies, a coin has no reason to fall; it just sits at
					// its spawn point until picked up. isEntity=true (not isTile) so it's a
					// non-tile hit in ResolveParticle -- the pickup callback path, same branch
					// enemies' stomp callback uses, not a physical collision/landing.
					int idx = ctx.world.spawn(ShapeType::RECT, Vec2{ 16.0f, 16.0f }, x, y, 0.0f, 0.0f, 1.0f,
					                           0.0f, 1 << 1, 1 << 1, true, false, false);
					if (idx == -1) continue;
					ctx.world.tileID[idx] = 7;
					collectibles.push_back(idx);
				}
				else if (tileType == '@') {
					// Hitbox stays a tight 16x16 -- deliberately SMALLER than the drawn 32x32
					// knight sprite. A 32x32 hitbox is exactly one full tile wide (tileSize=32),
					// which broke slope sensor/clamping behavior tuned around a smaller entity
					// straddling tile boundaries differently (confirmed: 32x32 reproduced the
					// "falls into slope" bug, 16x16 doesn't). GameplayScene::Draw()'s
					// kKnightFeetBottomPx anchor already draws the bigger sprite on top of this
					// hitbox without needing the hitbox itself to match the sprite's size.
					int idx = ctx.world.spawn(ShapeType::RECT, Vec2{ 16.0f, 16.0f }, x, y, 0.0f, 0.0f, 1.0f,
					                           981.0f, 1 << 1, 1 << 1, true, false, false);
					if (idx == -1) continue;
					ctx.world.tileID[idx] = 1;
					int es = ctx.world.entitySlot[idx];
					ctx.entities.isPlayerControlled[es] = 1;
					ctx.entities.jumpForce[es] = meta ? meta->Get("jumpForce", 420.0f) : 420.0f;
					ctx.entities.health[es] = meta ? (uint8_t)meta->Get("health", 3.0f) : 3;
					playerIdx = idx;
				}
			}
		}

		return playerIdx;
	}
};
