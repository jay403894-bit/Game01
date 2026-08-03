#pragma once
#include "EnemyAI.h"

// Data-driven enemy catalogue. Adding an enemy type = ONE row in kEnemyDefs (plus one
// UpdateEnemyAI switch case only if it needs a genuinely new behavior). Everything that used
// to be hardcoded per-type in five places -- ctor texture/anim loading, LevelInstantiator's
// spawn stamps, Draw's sprite branch, the editor's Type list -- reads this table instead.
//
// An enemy ROW's tileID = kEnemyTileIDBase + its index here (retires the magic 6/8). Base is
// far above every other entity tileID in use (player 1, coin 7, level-end 9, bump potions
// 112/113/128/129) so the Draw dispatch ranges can never collide.
//
// Anim rows follow the packed-sheet convention: start = row * gridX, count = frames in that
// row (a manifest .txt line maps 1:1 onto one Anim entry).
inline constexpr uint32_t kEnemyTileIDBase = 1000;

struct EnemyDef {
	const char* name;       // shown in the editor's Type combo -- entries auto-generate from this table
	const wchar_t* sheet;   // ExeRelative texture path
	int gridX, gridY;       // sheet grid dims
	// Drawn sprite size and PHYSICS hitbox, per type -- a 48x48 Brickhead needs to actually
	// fill a corridor (a 16x16 hitbox under a big sprite meant you just ran underneath it).
	// Hitbox slightly smaller than the sprite is the norm (forgiving edges, same as the player).
	float spriteW, spriteH;
	float hitW, hitH;
	struct Anim { int start, count; float fps; };
	Anim idle, walk, hit;
	EnemyAIType ai;
	float speed;            // default; per-cell kSpeed prop overrides
	float health;           // default; per-cell kHealth prop overrides
	float gravity = 981.0f; // 0 for flyers -- spawnEnemy passes this to world.spawn
};

inline constexpr EnemyDef kEnemyDefs[] = {
	// 0: green slime -- patrol walker. 4x3 sheet: row0 idle, row1 walk, row2 hit.
	{ "Slime (Patrol)",     L"textures\\slime_green.png",  4, 3, 24.0f, 24.0f, 16.0f, 16.0f, {0,4,6.0f},  {4,4,10.0f},  {8,4,10.0f},  EnemyAIType::Patrol,  40.0f, 3.0f },
	// 1: purple slime -- chase. Same sheet layout as green.
	{ "Slime (Chase)",      L"textures\\slime_purple.png", 4, 3, 24.0f, 24.0f, 16.0f, 16.0f, {0,4,6.0f},  {4,4,10.0f},  {8,4,10.0f},  EnemyAIType::Chase,   70.0f, 3.0f },
	// 2-5: mushrooms (Four Seasons pack). 4x5 sheet: row1 idle, row4 walk, row3 squash-as-hit.
	{ "Mushroom (Patrol)",  L"textures\\Mushroom_1.png",   4, 5, 24.0f, 24.0f, 16.0f, 16.0f, {4,4,6.0f},  {16,4,10.0f}, {12,4,10.0f}, EnemyAIType::Patrol,  30.0f, 3.0f },
	{ "Mushroom (Patrol 2)",L"textures\\Mushroom_2.png",   4, 5, 24.0f, 24.0f, 16.0f, 16.0f, {4,4,6.0f},  {16,4,10.0f}, {12,4,10.0f}, EnemyAIType::Patrol,  30.0f, 3.0f },
	{ "Mushroom (Chase)",   L"textures\\Mushroom_3.png",   4, 5, 24.0f, 24.0f, 16.0f, 16.0f, {4,4,6.0f},  {16,4,10.0f}, {12,4,10.0f}, EnemyAIType::Chase,   30.0f, 3.0f },
	{ "Mushroom (Chase 2)", L"textures\\Mushroom_4.png",   4, 5, 24.0f, 24.0f, 16.0f, 16.0f, {4,4,6.0f},  {16,4,10.0f}, {12,4,10.0f}, EnemyAIType::Chase,   30.0f, 3.0f },
	// 6: bee -- FLYER. 4x7 sheet (see Bee.txt): row2 Idle as idle AND walk (flying IS its
	// idle), row0 Attack_1 as hit flash. gravity 0; hovers at spawn anchor, attacks in radius.
	{ "Bee (Flyer)",        L"textures\\Bee.png",          4, 7, 24.0f, 24.0f, 16.0f, 16.0f, {8,4,8.0f},  {8,4,8.0f},   {0,4,10.0f},  EnemyAIType::Flyer,   70.0f, 2.0f, 0.0f },
	// 7: cactus -- SHOOTER. 8x6 sheet (see Cactus_1.txt): row3 Idle_1 as idle/walk (never
	// moves), row2 Attack_3 as hit flash. Grounded turret; needles inside Chase Radius.
	{ "Cactus (Shooter)",   L"textures\\Cactus_1.png",     8, 6, 24.0f, 24.0f, 16.0f, 20.0f, {24,4,6.0f}, {24,4,6.0f},  {16,4,10.0f}, EnemyAIType::Shooter,  0.0f, 3.0f },
	{ "Cactus 2 (Shooter)",   L"textures\\Cactus_2.png",     8, 6, 24.0f, 24.0f, 16.0f, 20.0f, {24,4,6.0f}, {24,4,6.0f},  {16,4,10.0f}, EnemyAIType::Shooter,  0.0f, 3.0f },
	// 8: Brickhead -- FALLER, the pack's actual Thwomp. 12x5 sheet of 48x48 cells (see
	// Brickhead_1.txt): row1 Idle_1 as idle/walk, row0 Attack (the slam grimace) as hit flash.
	// 40x44 hitbox under a 48x48 sprite: big enough that a corridor is genuinely BLOCKED while
	// it's down -- the whole reason per-def sizes exist (a 24px faller was just decoration you
	// walked under). gravity 0 -- the Faller AI owns velY. Sturdy: stomping a brick hurts pride.
	{ "Brickhead (Faller)", L"textures\\Brickhead_1.png", 12, 5, 48.0f, 48.0f, 40.0f, 44.0f, {12,9,8.0f}, {12,9,8.0f},  {0,12,14.0f}, EnemyAIType::Faller,   0.0f, 9.0f, 0.0f },
	// 9: Bomb -- BOMB, stationary proximity mine. 288x240 sheet = 12x10 grid of 24px cells
	// (VERIFIED with a gridline overlay -- an earlier 6-col guess made every frame sample
	// half-cells/empty space, which rendered as flickering): row0 = explosion (6 frames),
	// rows 1+ = 4-frame bomb loops. idle/walk = row1 (frames 12-15 -- there is no dedicated
	// walk row, patrolling reuses the idle loop); hit = row0's explosion, which doubles as the
	// lit-fuse anim while the detonation timer runs (see the Bomb block in RunPhysicsSubstep).
	// Patrols its bounds until the player enters trigger range, then stops and detonates.
	{ "Bomb",              L"textures\\Bomb.png",          12, 10, 24.0f, 24.0f, 16.0f, 16.0f, {12,4,6.0f}, {12,4,6.0f},  {0,6,10.0f},  EnemyAIType::Bomb,    30.0f, 1.0f },
	// 10: Balloon -- FLYERBOMB. 256x160 sheet = 8x5 grid of 32px cells (verified with a
	// gridline overlay, same as Bomb): row0 = 8-frame float loop as idle/walk, row1 = the
	// 4-frame POP as hit -- which doubles as the lit-fuse anim while the shared Bomb fuse
	// burns (user-confirmed: attack row is the SECOND row here, vs. Bomb's first).
	// Flyer-style hover at anchor, gravity 0; drifts at the player when activated and
	// detonates at close range instead of ramming. Fragile on purpose -- it's a balloon.
	{ "Balloon (FlyerBomb)", L"textures\\Balloon_1.png",    8, 5, 32.0f, 32.0f, 16.0f, 20.0f, {0,8,8.0f},  {0,8,8.0f},   {8,4,10.0f},  EnemyAIType::FlyerBomb, 70.0f, 1.0f, 0.0f },
	{ "Balloon 2 (FlyerBomb)", L"textures\\Balloon_2.png",    8, 5, 32.0f, 32.0f, 16.0f, 20.0f, {0,8,8.0f},  {0,8,8.0f},   {8,4,10.0f},  EnemyAIType::FlyerBomb, 70.0f, 1.0f, 0.0f },
	{ "Balloon 3 (FlyerBomb)", L"textures\\Balloon_3.png",    8, 5, 32.0f, 32.0f, 16.0f, 20.0f, {0,8,8.0f},  {0,8,8.0f},   {8,4,10.0f},  EnemyAIType::FlyerBomb, 70.0f, 1.0f, 0.0f },
	{ "Balloon 4 (FlyerBomb)", L"textures\\Balloon_4.png",    8, 5, 32.0f, 32.0f, 16.0f, 20.0f, {0,8,8.0f},  {0,8,8.0f},   {8,4,10.0f},  EnemyAIType::FlyerBomb, 70.0f, 1.0f, 0.0f },

};
inline constexpr int kEnemyDefCount = (int)(sizeof(kEnemyDefs) / sizeof(kEnemyDefs[0]));
