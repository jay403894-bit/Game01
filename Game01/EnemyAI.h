#pragma once
#include <cmath>
#include <PhysicsWorld.h>
#include <PhysicsSystem.h>

// Add a new behavior by adding an enum case here and a matching case in UpdateEnemyAI's switch --
// nothing else (MapReader's spawn call, GameplayScene::Update's per-frame loop, the stomp-kill
// check) needs to know a new type exists. That's the actual "system" part: one dispatch point,
// callers just tag an enemy with a type and never touch behavior code again.
enum class EnemyAIType { Patrol, Chase, Flyer, Shooter, Faller, Bomb, FlyerBomb };

struct EnemyInstance {
	int worldIdx;
	EnemyAIType type;
	// Per-instance AI parameter; meaning depends on type (Flyer/Shooter: activation radius).
	// Chase keeps its radius in entities.targetY (legacy); Flyer can't -- it needs targetX/Y
	// for its ANCHOR.
	float aiParam = 150.0f;
	// Shooter-only: seconds until it may fire again. Ticked and consumed by GameplayScene's
	// enemy loop (the AI signals INTENT to fire via UpdateEnemyAI's return; actually spawning a
	// projectile needs the scene's emitter/sound, which this header deliberately can't reach).
	// Faller reuses it as its landed-pause countdown.
	float fireTimer = 0.0f;
	// Faller-only state machine: 0=armed at anchor, 1=slamming, 2=landed pause, 3=rising home.
	// lastPosY detects the landing (slamming but posY stopped moving = a tile blocked us --
	// non-player entities have no isGrounded, ResolveParticle just halts them).
	uint8_t aiState = 0;
	float lastPosY = 0.0f;
};

// One dispatch point for every enemy's behavior, called once per enemy per frame. Sets
// world.velX[e.worldIdx]'s INTENT ONLY according to `type` -- it does NOT move the enemy or
// resolve collision itself. That's on purpose: PhysicsSystem::ApplyPhysics (the sensor-based
// slope/wall-jump/ground-friction system) is reserved for the PLAYER only. Every other entity/tile/
// particle -- enemies included -- is meant to move and collide through the existing parallel path:
// UpdateWorldParallel -> UpdateChunk (which integrates gravity/velocity and skips only
// isPlayerControlled rows) -> ResolveParticle (simple AABB push-out + one-way top landing, and the
// onCollision callback dispatch for non-tile hits). Calling ApplyPhysics per-enemy here as well
// would double-integrate/double-resolve them against that same-frame parallel pass.
//
// Both cases below reuse EntityTable's targetX/targetY/moveSpeed columns (unused by anything else
// for non-gridMovement entities) but give them a DIFFERENT meaning per type -- Patrol treats
// targetX/targetY as fixed left/right world-space turnaround points; Chase treats targetY as an
// activation RADIUS around the enemy's own current position. This is exactly why the meaning is
// documented per-case here instead of being named generically -- the columns are just storage,
// the type is what says what they mean.
// Returns true when this enemy WANTS TO ATTACK this frame (currently only Shooter: player in
// range) -- the scene's enemy loop owns cooldown + projectile spawning. Every other type
// returns false.
inline bool UpdateEnemyAI(EnemyInstance& enemy, PlatformerPhysics2D::PhysicsContext& ctx, float playerX, float playerY)
{
	using namespace PlatformerPhysics2D;
	PhysicsWorld& world = ctx.world;
	int e = enemy.worldIdx;
	if (world.life[e] <= 0.0f) return false;
	if (world.isGhosted[e]) return false; // stomped -- see the stomp onCollision callback for why isGhosted, not life=0
	int slot = world.entitySlot[e];

	switch (enemy.type) {
	case EnemyAIType::Patrol: {
		float left = ctx.entities.targetX[slot];
		float right = ctx.entities.targetY[slot];
		float speed = ctx.entities.moveSpeed[slot];
		// isFacingRight (otherwise unused for non-player entities) is the PERSISTENT direction
		// state -- only flipped at a bound. velX itself is NOT persistent state: it gets
		// overwritten by ResolveParticle every frame this same step (restitution off a collision,
		// zeroed by the slope wall gate, etc.), so setting it only "at the bound" meant any outside
		// disturbance permanently derailed the patrol until it happened to drift back to a bound on
		// its own -- which, at a near-zero residual velocity, could take forever or never happen.
		// Re-asserting velX from isFacingRight EVERY frame, unconditionally, means nothing else in
		// the physics step can leave it stuck.
		if (world.posX[e] <= left) ctx.entities.isFacingRight[slot] = 1;
		else if (world.posX[e] >= right) ctx.entities.isFacingRight[slot] = 0;
		world.velX[e] = ctx.entities.isFacingRight[slot] ? speed : -speed;
		break;
	}
	case EnemyAIType::Chase: {
		float speed = ctx.entities.moveSpeed[slot];
		float chaseRadius = ctx.entities.targetY[slot];
		float dx = playerX - world.posX[e];
		// TRUE circular radius (dx AND dy, compared squared -- no sqrt needed). The old check
		// was fabs(dx) alone, which made the "radius" an infinite VERTICAL band: a player far
		// below/above on another floor but horizontally aligned still triggered the chase.
		float dy = playerY - world.posY[e];
		bool inRange = (dx * dx + dy * dy) <= (chaseRadius * chaseRadius);
		world.velX[e] = inRange ? (dx > 0.0f ? speed : -speed) : 0.0f;
		// Patrol already sets isFacingRight (see its case above); Chase never touched it at all,
		// so it stayed at its default (0) forever -- draw code reading
		// !ctx.entities.isFacingRight[slot] to flip the sprite was then permanently true for
		// every chase enemy, regardless of which way it was actually moving. Only update on
		// actual movement (same as the player's own HandleInput), so it keeps facing whichever
		// way it last moved once out of range/stopped, instead of snapping to a default.
		if (inRange) ctx.entities.isFacingRight[slot] = (dx > 0.0f) ? 1 : 0;
		break;
	}
	case EnemyAIType::Flyer: {
		// Airborne enemy (spawned with gravity 0 -- see EnemyDef::gravity): hover-bobs at its
		// spawn ANCHOR (targetX/targetY, stamped by spawnEnemy -- the columns Patrol/Chase use
		// for bounds/radius mean "home position" here); when the player enters e.aiParam radius,
		// flies STRAIGHT at them on both axes; when they leave, flies home and resumes bobbing.
		// Sets velX AND velY as intent every frame -- integration/collision stay in the shared
		// parallel path like every other enemy; tiles still wall it via ResolveParticle.
		float speed = ctx.entities.moveSpeed[slot];
		float dx = playerX - world.posX[e];
		float dy = playerY - world.posY[e];
		bool inRange = (dx * dx + dy * dy) <= (enemy.aiParam * enemy.aiParam);

		// Bob clock for free: age ticks once per substep, so age*PHYSICS_STEP-ish scaling gives
		// a stable phase without per-enemy timer state. Amplitude in px/s of velY, not position.
		constexpr float kBobFreq = 4.0f, kBobAmp = 18.0f, kHomeDeadzone = 3.0f;
		float bob = std::sin((float)world.age[e] * (1.0f / 120.0f) * kBobFreq * 6.2831853f) * kBobAmp;

		float tx, ty; // seek target this frame
		if (inRange) { tx = playerX; ty = playerY; }
		else         { tx = ctx.entities.targetX[slot]; ty = ctx.entities.targetY[slot]; }
		float sx = tx - world.posX[e], sy = ty - world.posY[e];
		float dist = std::sqrt(sx * sx + sy * sy);
		if (dist > kHomeDeadzone) {
			float s = (inRange ? speed : speed * 0.6f) / dist; // amble home, commit when attacking
			world.velX[e] = sx * s;
			world.velY[e] = sy * s + bob;
		} else {
			world.velX[e] = 0.0f;
			world.velY[e] = bob; // parked at anchor: pure hover-bob
		}
		if (world.velX[e] != 0.0f) ctx.entities.isFacingRight[slot] = (world.velX[e] > 0.0f) ? 1 : 0;
		break;
	}
	case EnemyAIType::Shooter: {
		// Stationary turret (a cactus doesn't walk): never moves itself, just faces the player
		// and signals attack intent while they're inside aiParam radius. The scene's enemy loop
		// turns intent + expired fireTimer into an actual projectile.
		world.velX[e] = 0.0f;
		float dx = playerX - world.posX[e];
		float dy = playerY - world.posY[e];
		bool inRange = (dx * dx + dy * dy) <= (enemy.aiParam * enemy.aiParam);
		if (inRange) ctx.entities.isFacingRight[slot] = (dx > 0.0f) ? 1 : 0;
		return inRange;
	}
	case EnemyAIType::Faller: {
		// Thwomp-style falling spike (gravity 0 -- AI owns velY entirely). Hangs at its spawn
		// ANCHOR (targetX/targetY); when the player passes UNDERNEATH within aiParam horizontal
		// half-width, slams down; a tile stopping it (posY stops advancing while slamming --
		// non-player rows have no isGrounded, ResolveParticle just halts them) starts a short
		// pause, then it rises slowly home and re-arms. Contact damage/stomp rules are the
		// generic enemy ones -- nothing else needs to know this type exists.
		constexpr float kSlamSpeed = 420.0f, kRiseSpeed = 55.0f, kLandPause = 0.7f;
		world.velX[e] = 0.0f;
		float anchorY = ctx.entities.targetY[slot];
		switch (enemy.aiState) {
		case 0: { // armed at anchor
			world.velY[e] = 0.0f;
			bool underMe = std::fabs(playerX - world.posX[e]) <= enemy.aiParam &&
			               playerY > world.posY[e]; // +Y is down: player below
			if (underMe) { enemy.aiState = 1; enemy.lastPosY = world.posY[e] - 1.0f; }
			break;
		}
		case 1: // slamming -- landing = posY stopped advancing despite the slam velocity
			if (world.posY[e] <= enemy.lastPosY + 0.01f) {
				enemy.aiState = 2; enemy.fireTimer = kLandPause;
				world.velY[e] = 0.0f;
			} else {
				enemy.lastPosY = world.posY[e];
				world.velY[e] = kSlamSpeed;
			}
			break;
		case 2: // landed pause (fireTimer ticked by the scene's enemy loop, same as Shooter)
			world.velY[e] = 0.0f;
			if (enemy.fireTimer <= 0.0f) enemy.aiState = 3;
			break;
		case 3: // rising home
			if (world.posY[e] <= anchorY) {
				world.posY[e] = anchorY;
				world.velY[e] = 0.0f;
				enemy.aiState = 0; // re-armed
			} else {
				world.velY[e] = -kRiseSpeed;
			}
			break;
		}
		break;
	}
	case EnemyAIType::Bomb: {
		// Walking mine: patrols between bounds (targetX/targetY, same storage as Patrol -- no
		// conflict with Chase's radius-in-targetY legacy, since Bomb's radius rides in aiParam)
		// until the player comes inside aiParam. Then it stops dead and returns true; the scene
		// owns the fuse/detonation (aiState > 0 = lit -- keep holding still while it burns).
		// There's no dedicated walk row in Bomb.png; the def points walk at the idle frames.
		float dx = playerX - world.posX[e];
		float dy = playerY - world.posY[e];
		bool inRange = (dx * dx + dy * dy) <= (enemy.aiParam * enemy.aiParam);
		if (enemy.aiState != 0 || inRange) {
			world.velX[e] = 0.0f; // lit (or about to be): stand still and blow
		} else {
			float left = ctx.entities.targetX[slot];
			float right = ctx.entities.targetY[slot];
			float speed = ctx.entities.moveSpeed[slot];
			// Same every-frame velX re-assert as Patrol (see its comment for why velX can't be
			// trusted as persistent state).
			if (world.posX[e] <= left) ctx.entities.isFacingRight[slot] = 1;
			else if (world.posX[e] >= right) ctx.entities.isFacingRight[slot] = 0;
			world.velX[e] = ctx.entities.isFacingRight[slot] ? speed : -speed;
		}
		return inRange;
	}
	case EnemyAIType::FlyerBomb: {
		// Balloon: Flyer's hover-bob/approach (anchor in targetX/targetY, activation radius in
		// aiParam -- same storage as Flyer), but the "attack" is DETONATION: instead of ramming,
		// it returns true once it drifts within the small detonate range of the player, and the
		// scene lights the same fuse/blast the Bomb uses. aiState > 0 = fuse lit (scene-owned):
		// hang in place bobbing while it burns.
		constexpr float kDetonateRange = 36.0f;
		constexpr float kBobFreq = 4.0f, kBobAmp = 18.0f, kHomeDeadzone = 3.0f;
		float bob = std::sin((float)world.age[e] * (1.0f / 120.0f) * kBobFreq * 6.2831853f) * kBobAmp;

		if (enemy.aiState != 0) { // lit: hold position, keep bobbing, nothing more to signal
			world.velX[e] = 0.0f;
			world.velY[e] = bob;
			return false;
		}

		float speed = ctx.entities.moveSpeed[slot];
		float dx = playerX - world.posX[e];
		float dy = playerY - world.posY[e];
		float distSq = dx * dx + dy * dy;
		bool inRange = distSq <= (enemy.aiParam * enemy.aiParam);

		float tx, ty; // seek target this frame -- player when activated, home anchor otherwise
		if (inRange) { tx = playerX; ty = playerY; }
		else         { tx = ctx.entities.targetX[slot]; ty = ctx.entities.targetY[slot]; }
		float sx = tx - world.posX[e], sy = ty - world.posY[e];
		float dist = std::sqrt(sx * sx + sy * sy);
		if (dist > kHomeDeadzone) {
			float s = (inRange ? speed : speed * 0.6f) / dist; // amble home, commit when closing in
			world.velX[e] = sx * s;
			world.velY[e] = sy * s + bob;
		} else {
			world.velX[e] = 0.0f;
			world.velY[e] = bob;
		}
		if (world.velX[e] != 0.0f) ctx.entities.isFacingRight[slot] = (world.velX[e] > 0.0f) ? 1 : 0;
		return distSq <= (kDetonateRange * kDetonateRange);
	}
	}
	return false;
}
