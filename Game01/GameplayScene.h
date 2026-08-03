#pragma once
#include <vector>
#include <memory>
#include <DirectXMath.h>
#include <Renderer2D.h>
#include <Camera2D.h>
#include <InputManager.h>
#include <PhysicsWorld.h>
#include <SpatialGrid.h>
#include <PhysicsSystem.h>
#include <TileMap.h>
#include "BinaryMapFormat.h"
#include <SoundManager.h>
#include <GameTimer.h>
#include <Font.h>
#include "Scene.h"
#include "EmitterTable.h"
#include "EnemyAI.h"
#include "GridSpriteAnimation.h"
#include "GridSpriteSheet.h"
#include "GameOverScene.h"
// Ported from the source project's Level1 -- owns its own PhysicsWorld/SpatialGrid built from
// an inline map, and drives HandleInput/Update/Draw through SceneManager instead of being
// hardcoded into main()'s loop.
class GameplayScene : public Scene
{
public:
	// font/sound are raw, non-owning pointers -- owned once by main.cpp (outlives the whole
	// SceneManager stack: GameplayScene sits between StartMenuScene and GameOverScene on that
	// stack, never above main.cpp's own locals), not loaded/initialized separately per scene.
	// startingLives: defaults to 3 for a fresh game start (StartMenuScene's PushScene calls).
	// Respawning while lives remain doesn't reset THIS instance's world in place -- a fallen/
	// disappeared "canFall" platform, killed enemies, collected coins, etc. would stay broken --
	// it constructs a brand new GameplayScene via SceneManager::ReplaceScene, re-running the exact
	// same MapReader::BuildWorldFromMap construction path to get a fresh, guaranteed-correct level
	// state, passing the player's current lives count through here so it survives the reset (see
	// the respawnPending block in Update() for the ReplaceScene call itself).
	// levelNumber picks which map file loads: "levels\level_<N>.tmap". Threaded through both
	// scene-rebuild paths -- level completion passes level+1, the death-respawn rebuild passes
	// the CURRENT level (dying used to silently reset the run to level_1 otherwise).
	GameplayScene(JLib::Font* font, JLib::SoundManager* sound, JLib::Renderer2D& renderer, std::shared_ptr<JLib::InputManager> input, JLib::Camera2D& camera,
	              JLib::Mesh* quadMesh, JLib::Mesh* slopeUpRightMesh, JLib::Mesh* slopeUpLeftMesh,
	              JLib::TextureHandle tileTexture, uint32_t dustEffect, uint32_t width, uint32_t height, uint8_t startingLives = 3, int levelNumber = 1,
	              uint8_t currentHealth = 3, int currentCoinCount = 0);

	void Update(bool& isRunning, float dt = 0.0f) override;
	void Draw() override;
	void HandleInput(float dt) override;
	~GameplayScene() override = default;

private:
	enum class CMD { QUIT };
	DirectX::XMFLOAT2 lastScreenSize;
	// Runs exactly one PHYSICS_STEP-sized slice of simulation (grid rebuild, platforms, player
	// physics, enemy AI, world-parallel pass, ...). Called 0+ times per Update() by the fixed-
	// timestep accumulator below -- never called directly with a variable dt, which is the whole
	// point (see Update()'s comment for why that mattered).
	void RunPhysicsSubstep();
	// Real elapsed time not yet "spent" as a PHYSICS_STEP-sized substep. Carries the remainder
	// across frames so simulated time tracks real time on average regardless of framerate.a
	float physicsAccumulator = 0.0f;

	// m_binLevel is declared FIRST so the init list can read its dimensions to size world/grid
	// (it's loaded from the editor .tmap before either is constructed).
	// world/grid must be declared before ctx -- ctx holds references into world.entities/platforms,
	// and member init order follows declaration order regardless of the init-list's own order.
	BinaryLevel m_binLevel;
	PlatformerPhysics2D::PhysicsWorld world;
	PlatformerPhysics2D::SpatialGrid grid;
	PlatformerPhysics2D::TileMap m_tiles; // static collision geometry, filled by LevelInstantiator
	PlatformerPhysics2D::PhysicsContext ctx;
	JLib::SoundManager* sound;
	JLib::SoundHandle music;
	JLib::Font* font;
	// Raw row indices are only stable BETWEEN world.commit(&grid) calls -- the sorted
	// commit physically reorders every live row (see PhysicsWorld::commit). Anything held
	// across a commit lives as a Handle; RefreshIndices() (called immediately after every
	// commit) re-resolves the raw-index mirrors below, so all the existing per-frame code
	// keeps using plain ints and never notices.
	int playerIdx = -1;
	PlatformerPhysics2D::Handle hPlayer;
	std::vector<int> platforms;
	std::vector<int> levelEnd;
	std::vector<PlatformerPhysics2D::Handle> hPlatforms;
	std::vector<PlatformerPhysics2D::Handle> hLevelEnd;
	int level = 1;  // Current level number; used to load "level_N.tmap" and detect end conditions
	// Armed by the level-end trigger's onCollision callback; consumed by Update() AFTER the
	// substep loop. The callback must never switch scenes itself -- it runs on a worker thread
	// mid-ParallelFor over this scene's own PhysicsWorld (see the callback's comment).
	bool levelCompletePending = false;
	uint8_t lives = 3;
	JLib::GameTimer respawnTimer;
	// True from the moment the player falls to death until respawnTimer clears the 3s delay.
	// While true: HandleInput ignores movement, RunPhysicsSubstep's fall-death check won't
	// re-trigger, Draw() holds the camera at frozenCameraPos instead of following the player
	// (who keeps falling/simulating normally -- only the CAMERA stops, so the fall is still
	// visible instead of the screen just freezing solid).
	bool respawnPending = false;
	// Separate from respawnPending -- OnPlayerFell()'s Game-Over branch (lives == 0) needs its own
	// "stop re-triggering the fall-death check" latch, distinct from respawnPending, because
	// Update()'s respawn-rebuild block ALSO watches respawnPending: reusing that same flag meant
	// the instant GameOverScene got pushed, Update() kept executing (pushing a scene doesn't
	// interrupt the GameplayScene::Update() call already in progress) and, since respawnTimer was
	// never Reset() on the Game-Over path, immediately saw a stale TotalTime() >= 3s and fired the
	// respawn-rebuild's ReplaceScene() too -- tearing out the just-pushed GameOverScene before a
	// single frame of it ever rendered.
	bool gameOverTriggered = false;
	// True from the moment of death (fall OR HP reaching 0) until deathAnimTimer clears
	// deathAnimDuration. Guards RunPhysicsSubstep's idle/run/hit selection block so it can't
	// override knightDeath -- setting currentKnightAnim = &knightDeath in OnPlayerFell() alone did
	// nothing visible, because the very next substep's selection block (which only ever knew about
	// idle/run/hit) immediately picked a different animation based on moveIntent, before a single
	// frame of the death pose was ever drawn.
	bool isDead = false;
	float deathAnimTimer = 0.0f;
	// Gates ONLY the Game-Over path's PushScene(GameOverScene) call -- long enough to actually see
	// knightDeath play through once (4 frames @ 6fps = ~0.667s; a little headroom on top). The
	// lives>0 respawn path doesn't need its own separate timer for this: it already waits a full 3s
	// (respawnTimer, see Update()) before ReplaceScene fires, comfortably more than enough for the
	// death animation to finish and hold on its last frame well before that.
	const float deathAnimDuration = 0.8f;
	// Level timer -- counts down in real seconds (Update()'s own dt, same as respawnTimer/
	// deathAnimTimer); reaching 0 costs a life via OnPlayerFell(), same "player has died, however
	// caused" entry point falling/enemy damage already use. Not ticked while respawnPending/
	// gameOverTriggered/isDead -- already dying, no reason to also drain the clock during that
	// window. Resets to levelTimerStart for free on respawn, same as everything else that isn't
	// `lives` -- respawning constructs a brand new GameplayScene (see Update()'s ReplaceScene call).
	const float levelTimerStart = 500.0f;
	float levelTimer = levelTimerStart;
	DirectX::XMFLOAT2 frozenCameraPos{};
	// Smoothed horizontal look-ahead offset (world px). Eases toward +/-kCamLookAhead as the
	// player faces right/left, so turning around pans the view smoothly instead of snapping.
	float camLookX = 0.0f;
	// HISTORY: killed enemies/collected coins are marked isGhosted rather than life=0. That
	// discipline PREDATES the Handle system -- it originally existed because compressArrays()
	// physically shifting a dead row out would cascade into every later row's index, and a coin
	// spawned before the player once shifted the player's row out from under playerIdx (UB via
	// entitySlot[-1]). That danger is gone: indices are Handle-mirrored and refreshed after every
	// commit (see RefreshIndices), so life=0 death is SAFE for any row now. isGhosted is kept for
	// enemies/coins anyway -- it's cheap, and "inert but still drawn/faded" may be wanted later --
	// but it's a choice now, not load-bearing correctness.
	std::vector<EnemyInstance> enemies;
	std::vector<PlatformerPhysics2D::Handle> hEnemies;
	std::vector<int> collectibles;
	std::vector<PlatformerPhysics2D::Handle> hCollectibles;
	// Re-resolves playerIdx / platforms / enemies[].worldIdx / collectibles from their Handles.
	// MUST run immediately after every world.commit(&grid) -- that's the only point indices change.
	void RefreshIndices();
	int coinCount = 0;
	// coin.png: 12 frames, 16x16 each, laid out as a single row (192x16 total) -- loaded directly
	// via ResourceManager::LoadTexture in the constructor, NOT LoadAtlas -- it's a pre-made sheet
	// with no per-region sidecar, not something AtlasPacker produced. Shared by every collectible
	// (they're all identical coins), ticked once per RunPhysicsSubstep alongside everything else.
	JLib::GridSpriteAnimation coinAnim;
	std::unique_ptr<JLib::GridSpriteSheet> platformSheet;
	std::unique_ptr<JLib::GridSpriteSheet> tileSheet;

	// knight.png: 8x8 grid (256x256, 32x32 cells). Rows: 0=idle(4 used), 1=blank separator,
	// 2-4=run(18 used across 3 rows), 5=blank separator, 6=roll(8, fills the row), 7=hit(4)+death(4)
	// sharing the last row -- confirmed against the tutorial's own row layout, and the numbers
	// account for exactly all 8 rows with nothing left over. currentKnightAnim points at whichever
	// of these is active; RunPhysicsSubstep's anim-selection block swaps it (and calls Reset() on
	// the new one) based on player state -- knightHit now fires while
	// ctx.entities.hitTimer[playerSlot] > 0 (see the damage-collision callback). knightDeath still
	// isn't wired to anything -- OnPlayerFell()'s existing respawn/Game-Over flow is what actually
	// handles death, not a death animation.
	JLib::GridSpriteAnimation knightIdle, knightRun, knightRoll, knightHit, knightDeath;
	JLib::GridSpriteAnimation* currentKnightAnim = nullptr;

	// slime_green.png/slime_purple.png: 96x72, confirmed 4x3 grid of 24x24 cells (row0=idle,
	// row1=walk, row2=hit -- now wired: RunPhysicsSubstep ticks ctx.entities.hitTimer down per
	// enemy, and Draw() shows *Hit instead of *Idle/*Walk while it's > 0, see the health/damage
	// system in the stomp onCollision callback for what actually sets hitTimer).
	// Green = patrol ('E', tileID 6), purple = chase ('C', tileID 8) -- picked in Draw() by tileID,
	// no per-enemy state needed since every enemy of a given type shares one animation instance
	// (same simplification coinAnim already uses -- all coins/slimes of a kind stay in visual sync).
	// One AnimSet per kEnemyDefs row, built in the ctor by looping the table (replaces the old
	// hand-written slimeGreen*/slimePurple* member sextet). Indexed by enemy-def index, which an
	// enemy ROW carries as tileID - kEnemyTileIDBase.
	struct EnemyAnimSet { JLib::GridSpriteAnimation idle, walk, hit; };
	std::vector<EnemyAnimSet> enemyAnims;

	std::vector<CMD> cmdQ;

	// Player's dust-on-landing emitter. Allocated once at construction (world.emitterSlot[playerIdx]
	// is set right after playerIdx is known) -- not lazily when grounding first flips, since the
	// emitter has to already exist before Update() can ever call Spawn() on it.
	EmitterTable emitters{ 4 };
	bool wasGrounded = false;
	// Fall-death trigger. respawnX/Y captured at construction (the player's '@' spawn point).
	// mapBottomY is the world Y coordinate of the map's bottom edge -- player falls below this
	// to trigger OnPlayerFell() in RunPhysicsSubstep. Set during level instantiation from map height.
	// Falling costs a life outright (bypasses HP entirely) -- OnPlayerFell() is the shared
	// "player has died, however caused" entry point (also called on enemy contact when health reaches 0).
	float respawnX = 0.0f, respawnY = 0.0f;
	float mapBottomY = 0.0f;  // Set by LevelInstantiator at level load
	void OnPlayerFell();
	// Bump-block response (Mario-style): called from RunPhysicsSubstep when the player's rising
	// head hits tile row `tileRow`. Reads the authored cell props (kContains/kContainCount/
	// kBreakable) from m_binLevel by position, pops contents / spends / breaks accordingly.
	// No-op for plain tiles, so it's safe to call on every ceiling bonk.
	void OnBlockBump(int tileRow);
	// Spawns a potion entity popping out the top of a bumped block (kContains 2..5) and
	// registers its pickup onCollision (apply effect + ghost + sound). Direct spawn -- only
	// call from the serial section of RunPhysicsSubstep (before UpdateWorldParallel).
	void SpawnPotion(int containsType, DirectX::XMFLOAT2 pos);
	// Bump-nudge: PURELY VISUAL block hop on bump. Physics/tile position never moves (tiles are
	// immovable by design -- and Draw recovers their sprite by position, which an actual move
	// would break); Draw just offsets the rendered quad for the bumped CELL while the timer
	// runs. One active nudge is plenty (bumps are one-at-a-time in practice).
	int   bumpNudgeCol = -1, bumpNudgeRow = -1;
	float bumpNudgeTimer = 0.0f;
	static constexpr float kBumpNudgeDuration = 0.15f; // s
	static constexpr float kBumpNudgeHeight   = 5.0f;  // world px at the hop's peak
	const float PHYSICS_STEP = 0.00833f;
	const float playerMoveSpeed = 300.0f;
	const float playerAccel = 2000.0f; // px/s^2 -- how fast velX approaches playerMoveSpeed
	// While > 0, step 5's moveDir->velX assignment is skipped so a wall-jump kick isn't
	// instantly overwritten by the movement key the player is still holding into the wall.
	float wallJumpLockTimer = 0.0f;
	const float wallJumpLockDuration = 0.05f;

	// Damage/knockback tuning -- see the enemy stomp onCollision callback (set up in the
	// constructor) for where these are actually used. hitTimer (EntityTable's own column, already
	// existed unused) does double duty per row: for the player it's an invulnerability window
	// (side-contact damage is ignored while > 0) AND drives showing knightHit in the anim-selection
	// block; for an enemy it's purely a hit-flash duration (no invulnerability concept for enemies
	// -- every stomp lands).
	const float kPlayerInvulnDuration = 0.8f;
	const float kEnemyHitFlashDuration = 0.3f;
	const float kKnockbackSpeedX = 220.0f;
	const float kKnockbackSpeedY = 280.0f; // negative applied -- upward pop, same sign convention as jumpForce

	JLib::Renderer2D& renderer;
	std::shared_ptr<JLib::InputManager> input;
	JLib::Camera2D& camera;
	JLib::Mesh* quadMesh;
	JLib::Mesh* slopeUpRightMesh;
	JLib::Mesh* slopeUpLeftMesh;
	JLib::TextureHandle tileTexture; // FlushBatchTask/ResourceManager::Resolve requires a valid
	                                 // handle -- a default-constructed "no texture" one throws
	                                 // instead of silently skipping, so every tile still needs
	                                 // a real texture; color still does the per-tileID tinting.
	uint32_t dustEffect; // GPU particle effectID from main.cpp's RegisterParticleEffect(), for the
	                     // landing-dust emitter -- see EmitterTable comment for why this can't be
	                     // resolved locally.
	uint32_t width, height;

	DirectX::XMFLOAT4 ColorForTileID(uint32_t tileID) const;
};
