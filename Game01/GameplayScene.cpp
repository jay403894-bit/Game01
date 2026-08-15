#include "pch.h"
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "GameplayScene.h"
#include "LevelData.h"
#include "TiledLoader.h"
#include "MapReader.h"
#include "LevelInstantiator.h"
#include "SceneManager.h"
#include "TileColors.h"
#include "Emitter.h"
#include "AtlasAnimation.h"
#include "GridSpriteAnimation.h"
#include "EnemyAI.h"
#include "EnemyDefs.h"
#include <Helpers.h>
#include <TaskScheduler.h>
#include <thread>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>

using namespace PlatformerPhysics2D;
using namespace JLib;

namespace {
	// Game-defined particle type tags -- PlatformerPhysics2D's `particleType` column is a plain int it
	// never interprets; the meaning lives entirely here. Add new types as needed.
	enum GameParticleType { PT_DEFAULT = 0, PT_DUST = 1, PT_WEAPON_IMPACT = 2, PT_ENEMY_SHOT = 3 };

	// Emitter::tag values for this game -- lets a slot hold multiple emitters (e.g. the player's GPU
	// dust emitter AND a CPU weapon-impact emitter) and call sites pick the right one via
	// EmitterTable::FindByTag instead of depending on push_back order.
	enum GameEmitterTag { EMIT_DUST = 0, EMIT_WEAPON = 1, EMIT_COIN = 2,
	                       EMIT_POTION_RED = 3, EMIT_POTION_BLUE = 4,
	                       EMIT_POTION_GREEN = 5, EMIT_POTION_YELLOW = 6,
	                       EMIT_ENEMY_SHOT = 7, EMIT_EXPLOSION = 8 };

	// (ParticleNeedsBroadphase / ShouldInsertIntoGrid used to live here -- per-row grid-insert
	// filtering. Gone with the insert() path itself: the sorted commit bins every live row as a
	// free byproduct of the sort, and queries already filter by layer/mask/isGhosted, so there's
	// nothing left to skip.)

	// Loaded ONCE, ever, at static init -- not per-GameplayScene-construction (respawn now builds
	// a brand new GameplayScene via ReplaceScene, so re-reading the file on every death would mean
	// repeated disk I/O for no benefit; the level's own content never changes mid-run). Same
	// "next to the exe, not the working directory" placement as textures/sound/fonts -- see
	// ExeRelative's own comment elsewhere in this file for why a bare relative path isn't safe.
	// TiledLoader, not LevelFormat -- authored in the actual Tiled map editor instead of hand-
	// written text (see TiledLoader.h's comment for the tileset/object property conventions the
	// .tmj needs to follow). Both loaders produce the same LevelData either way -- MapReader.h
	// doesn't know or care which one built it -- so LevelFormat/level1.lvl are still here unused
	// rather than deleted, in case a hand-edited text level is ever useful again.

	// Was a flat "60 + 32" constant, sized for a much smaller test map -- world.spawn() returns -1
	// once activeCount hits capacity, and MapReader silently `continue`s past every -1, so growing
	// the map without growing this meant the tail end of the parse (often including the floor row,
	// since parsing is row-major and floor is last) just never spawned at all. Deriving this from the
	// map's actual size means it can never fall behind again as the map grows.
	size_t ComputeMaxCells(const LevelData& level)
	{
		size_t maxRowLen = 0;
		for (const auto& row : level.tiles) maxRowLen = std::max(maxRowLen, row.size());
		return level.tiles.size() * maxRowLen + 32; // +32 headroom for particles spawned during play
	}

	constexpr size_t kMaxParticles = 16; // landing dust moved to a GPU emitter -- no longer stresses this pool

	// Path for level N in the sequential-progression scheme ("levels\level_<N>.tmap").
	std::wstring LevelPath(int n)
	{
		return JLib::ExeRelative((L"levels\\level_" + std::to_wstring(n) + L".tmap").c_str());
	}

	// Loads the editor-authored level for the given level number. Empty BinaryLevel (width 0)
	// if the file is missing -- the world/grid still build at a safe minimum size and
	// InstantiateLevel returns playerIdx -1 (Update no-ops until a level actually exists).
	BinaryLevel LoadEditorLevel(int levelNumber)
	{
		return BinaryMapFormat::Load(LevelPath(levelNumber));
	}

	// EntityTable/PlatformTable capacities DERIVED from the level content, same
	// never-falls-behind reasoning as ComputeMaxCells above. These used to be flat constants
	// (kMaxActors = 1+4, kMaxPlatforms = 5+4) tuned to one specific test map --
	// EntityTable::Alloc() THROWS "EntityTable exhausted" on the first spawn past the cap, so
	// authoring a map with one more entity than the constant allowed (e.g. adding several
	// level-end triggers) crashed new-game inside LevelInstantiator's spawn loop.
	// Player/enemies/coins/level-ends all take entity slots; platforms take platform slots.
	// objects[] counted too -- the legacy path still spawns them.
	size_t CountSpawns(const BinaryLevel& lvl, bool wantPlatforms)
	{
		size_t n = 0;
		for (const auto& cell : lvl.cells) {
			int t = (int)lvl.GetProperty(cell.properties, BinaryProps::kEntityType, -1.0f);
			if (t < 0) continue;
			if ((t == OBJ_PLATFORM) == wantPlatforms) ++n;
		}
		for (const auto& obj : lvl.objects)
			if (((BinaryObjectType)obj.objectType == OBJ_PLATFORM) == wantPlatforms) ++n;
		return n;
	}
	// Floor + headroom: an empty/missing level still builds a valid world, and the count never
	// sits exactly at the cliff edge where one extra spawn throws.
	size_t MaxActors(const BinaryLevel& lvl)    { return std::max<size_t>(8, CountSpawns(lvl, false) + 4); }
	size_t MaxPlatforms(const BinaryLevel& lvl) { return std::max<size_t>(4, CountSpawns(lvl, true) + 2); }

	// Row capacity for the PhysicsWorld: every cell AND every actor/platform/particle spawns as a
	// row, so the pool must hold them all at once, with headroom. Floor of 256 so a missing/empty
	// level still yields a valid world.
	size_t RowCap(const BinaryLevel& lvl)
	{
		size_t cells = (size_t)lvl.width * lvl.height;
		return std::max<size_t>(256, cells + MaxActors(lvl) + MaxPlatforms(lvl) + kMaxParticles + 64);
	}
}

GameplayScene::GameplayScene(JLib::Font* font, JLib::SoundManager* sound, JLib::Renderer2D& renderer, std::shared_ptr<JLib::InputManager> input, JLib::Camera2D& camera,
	JLib::Mesh* quadMesh, JLib::Mesh* slopeUpRightMesh, JLib::Mesh* slopeUpLeftMesh,
	JLib::TextureHandle tileTexture, uint32_t dustEffect, uint32_t width, uint32_t height, uint8_t startingLives, int levelNumber,
	uint8_t currentHealth, int currentCoinCount)
	: m_binLevel(LoadEditorLevel(levelNumber))
	, coinCount(currentCoinCount)
	, world(RowCap(m_binLevel), MaxActors(m_binLevel), MaxPlatforms(m_binLevel), kMaxParticles)
	, level(levelNumber)
	, grid(std::max<int>((int)width,  m_binLevel.width  * (int)m_binLevel.tileWidth),
	       std::max<int>((int)height, m_binLevel.height * (int)m_binLevel.tileHeight),
	       16, (int)RowCap(m_binLevel))
	, ctx{ world, grid, world.entities, world.platforms }
	, sound(sound)
	, font(font)
	, lives(startingLives)
	, renderer(renderer)
	, input(input)
	, camera(camera)
	, quadMesh(quadMesh)
	, slopeUpRightMesh(slopeUpRightMesh)
	, slopeUpLeftMesh(slopeUpLeftMesh)
	, tileTexture(tileTexture)
	, dustEffect(dustEffect)
	, width(width)
	, height(height)
{
	// coin.png loaded directly (not via LoadAtlas) since it's a pre-made 12-frame sheet, not
	// something AtlasPacker produced -- see GameplayScene.h's coinAnim comment.
	auto* resourceManager = renderer.GetResourceManager();
	camera.zoom = 2.0f;



	music = sound->PlayLoop(JLib::ExeRelativeA("sound\\Frozen In Time (Chiptune).mp3").c_str());
	if (!music.IsValid()) {
		throw("PlayLoop(\"Frozen In Time (Chiptune).mp3\") failed to load -- put a real file next to the exe to test.\n");
	}

	sound->SetVolume(music, 0.25f);
	JLib::TextureHandle coinTex;
	renderer.ExecuteUploadCommand([&](ID3D12GraphicsCommandList* cmd) {
		// LoadFromWICFile (inside LoadTexture) resolves a relative path against the process's
		// CURRENT WORKING DIRECTORY, not the exe's directory -- when launching from the VS
		// debugger those aren't the same, so a bare "textures\\coin.png" fails to resolve even
		// though the file is really sitting next to the exe, and LoadTexture's ThrowIfFailed
		// throws on that failure (the exact unhandled exception this crashed with). ExeRelative
		// anchors the path to the exe's own directory instead -- same fix already used for
		// fonts/sound elsewhere in this codebase.
		coinTex = resourceManager->LoadTexture(JLib::ExeRelative(L"textures\\coin.png"), cmd);
		});

	coinAnim = JLib::GridSpriteAnimation(coinTex, 12, 1, 0, 12, 12.0f);
	JLib::TextureHandle platformTex;
	renderer.ExecuteUploadCommand([&](ID3D12GraphicsCommandList* cmd) {
		// LoadFromWICFile (inside LoadTexture) resolves a relative path against the process's
		// CURRENT WORKING DIRECTORY, not the exe's directory -- when launching from the VS
		// debugger those aren't the same, so a bare "textures\\coin.png" fails to resolve even
		// though the file is really sitting next to the exe, and LoadTexture's ThrowIfFailed
		// throws on that failure (the exact unhandled exception this crashed with). ExeRelative
		// anchors the path to the exe's own directory instead -- same fix already used for
		// fonts/sound elsewhere in this codebase.
		platformTex = resourceManager->LoadTexture(JLib::ExeRelative(L"textures\\platforms.png"), cmd);
		});
	// platforms.png is only 64x64 total -- (32, 32) meant "32 columns x 32 rows," which sliced it
	// into 2x2-pixel fragments instead of real tiles. At 64x64 with this pack's established 16px
	// tile convention (same as coin.png's cells), it's a 4x4 grid of 16x16 tiles.
	platformSheet = std::make_unique<JLib::GridSpriteSheet>(platformTex, 4, 4);

	// knight.png: 8x8 grid, confirmed against the tutorial's own layout -- see GameplayScene.h's
// knightIdle/etc. comment for the full row breakdown.
	JLib::TextureHandle knightTex;
	renderer.ExecuteUploadCommand([&](ID3D12GraphicsCommandList* cmd) {
		knightTex = resourceManager->LoadTexture(JLib::ExeRelative(L"textures\\knight.png"), cmd);
		});
	// Real row layout, confirmed by cropping and inspecting each row directly (see conversation --
	// two earlier guesses at this were both wrong): row0=idle(4)+"IDLE" label, row1="RUN" label
	// only, row2-3=run(8+8=16, NOT 18 -- the previous 18-frame range overran into row4's "ROLL"
	// label text, sampling it as if it were an 18th run frame), row4="ROLL" label only,
	// row5=roll(8, full row -- NOT 6, the earlier "trim to 6" was chasing the wrong row),
	// row6=hit(4)+"HIT" label, row7=death(4)+"DEATH" label.
	knightIdle = GridSpriteAnimation(knightTex, 8, 8, 0 * 8, 4, 6.0f);
	knightRun = GridSpriteAnimation(knightTex, 8, 8, 2 * 8, 16, 14.0f);
	knightRoll = GridSpriteAnimation(knightTex, 8, 8, 5 * 8, 8, 12.0f);
	knightHit = GridSpriteAnimation(knightTex, 8, 8, 6 * 8, 4, 10.0f);
	knightDeath = GridSpriteAnimation(knightTex, 8, 8, 7 * 8, 4, 6.0f, /*loop=*/false); // plays once, holds the dead pose
	currentKnightAnim = &knightIdle;

	JLib::TextureHandle tileTex;
	renderer.ExecuteUploadCommand([&](ID3D12GraphicsCommandList* cmd) {
		// LoadFromWICFile (inside LoadTexture) resolves a relative path against the process's
		// CURRENT WORKING DIRECTORY, not the exe's directory -- when launching from the VS
		// debugger those aren't the same, so a bare "textures\\coin.png" fails to resolve even
		// though the file is really sitting next to the exe, and LoadTexture's ThrowIfFailed
		// throws on that failure (the exact unhandled exception this crashed with). ExeRelative
		// anchors the path to the exe's own directory instead -- same fix already used for
		// fonts/sound elsewhere in this codebase.
		tileTex = resourceManager->LoadTexture(JLib::ExeRelative(L"textures\\world_tileset.png"), cmd);
		});
	// MUST match EditorScene exactly (same image, same grid) or authored spriteIndexes decode to
	// different cells in-game than in the editor. world_tileset.png: 256x256 = 16x16 grid of
	// 16px tiles. (four-seasons was briefly swapped in but reverted -- 11 columns re-maps every
	// authored spriteIndex; see EditorScene's matching comment.)
	tileSheet = std::make_unique<JLib::GridSpriteSheet>(tileTex, 16, 16);





	// Enemy sheets/anims from the kEnemyDefs table -- one loop instead of a hand-written block
	// per type. A new enemy sheet is loaded here automatically the moment its table row exists.
	enemyAnims.reserve(kEnemyDefCount);
	for (int e = 0; e < kEnemyDefCount; ++e) {
		const EnemyDef& def = kEnemyDefs[e];
		JLib::TextureHandle tex;
		renderer.ExecuteUploadCommand([&](ID3D12GraphicsCommandList* cmd) {
			tex = resourceManager->LoadTexture(JLib::ExeRelative(def.sheet), cmd);
			});
		EnemyAnimSet set;
		set.idle = GridSpriteAnimation(tex, def.gridX, def.gridY, def.idle.start, def.idle.count, def.idle.fps);
		set.walk = GridSpriteAnimation(tex, def.gridX, def.gridY, def.walk.start, def.walk.count, def.walk.fps);
		set.hit  = GridSpriteAnimation(tex, def.gridX, def.gridY, def.hit.start,  def.hit.count,  def.hit.fps);
		enemyAnims.push_back(set);
	}

	// Build the world from the editor-authored binary level (m_binLevel, loaded in the init
	// list). CELLS fill m_tiles (static collision) + transitional rows; cell-entities and any
	// objects[] spawn actors/platforms/coins. Same return contract as the old MapReader call.
	ctx.tiles = &m_tiles;
	playerIdx = LevelInstantiator::InstantiateLevel(m_binLevel, ctx, m_tiles, platforms, enemies, collectibles, levelEnd, mapBottomY, currentHealth);
	if (playerIdx != -1) {
		ctx.entities.jumpsRemaining[world.entitySlot[playerIdx]] = 1; // single jump, no double-jump -- see the matching respawn reset below
		respawnX = world.posX[playerIdx];
		respawnY = world.posY[playerIdx];

		// Opt the player into an emitter slot and give it BOTH backends up front, tagged so each
		// call site grabs the right one -- Update()'s landing check below just calls Spawn() on
		// whichever it looks up, neither backend ever allocates lazily.
		// GPU dust (EMIT_DUST): purely visual, never collides with anything, so it shouldn't cost
		// a PhysicsWorld row/ParticleTable slot or CPU time in UpdateWorldParallel -- dustEffect is
		// a real effectID (see main.cpp's RegisterParticleEffect call for it), running the same
		// straight-line-motion + real-gravity compute shader linearEffect uses.
		// CPU weapon-impact (EMIT_WEAPON): a bullet -- CPU particles are real PhysicsWorld rows
		// PhysicsSystem can resolve against tiles/entities, which a GPU emitter can never do. Was
		// layer=0/mask=0 (collides with NOTHING) and gravity=900 (a heavy lobbed arc, not a flat-
		// flying bullet) when this was still just reserved/unused -- layer/mask now 1<<1 to match
		// the bit tiles/enemies both spawn with elsewhere (MapReader.h), so it can actually hit
		// them; gravity 0 so it flies straight instead of arcing into the ground almost immediately.
		// Damage-on-hit is handled in the enemy stomp callback below (checks world.isParticle(other)
		// + PT_WEAPON_IMPACT BEFORE the player-only guard, since a bullet obviously isn't the player).
		world.emitterSlot[playerIdx] = emitters.Alloc();
		auto& playerEmitters = emitters.Get(world.emitterSlot[playerIdx]);
		playerEmitters.push_back(Emitter::MakeGPU(renderer, dustEffect, 4.0f, { 1.0f, 1.0f, 1.0f, 1.0f }, EMIT_DUST));
		playerEmitters.push_back(Emitter::MakeCPU(ctx, { 4.0f, 4.0f }, 0.0f, PT_WEAPON_IMPACT, 1 << 1, 1 << 1, EMIT_WEAPON));
		// Gold sparkle on pickup -- same dustEffect pool as EMIT_DUST (RegisterParticleEffect's
		// colorEnd is just the FADE-TO color; the color passed per Spawn() call is the start color,
		// so one GPU pool can serve visually distinct effects without registering a second one).
		playerEmitters.push_back(Emitter::MakeGPU(renderer, dustEffect, 3.0f, { 1.0f, 0.85f, 0.2f, 1.0f }, EMIT_COIN));
		// Potion pickup sparkles -- same shared dustEffect pool trick as EMIT_COIN above (per-
		// emitter start color, one registered effect). One emitter per potion color; the pickup
		// callback in SpawnPotion looks them up by tag.
		playerEmitters.push_back(Emitter::MakeGPU(renderer, dustEffect, 3.0f, { 1.0f, 0.25f, 0.2f, 1.0f }, EMIT_POTION_RED));
		playerEmitters.push_back(Emitter::MakeGPU(renderer, dustEffect, 3.0f, { 0.3f, 0.5f, 1.0f, 1.0f }, EMIT_POTION_BLUE));
		playerEmitters.push_back(Emitter::MakeGPU(renderer, dustEffect, 3.0f, { 0.3f, 1.0f, 0.4f, 1.0f }, EMIT_POTION_GREEN));
		playerEmitters.push_back(Emitter::MakeGPU(renderer, dustEffect, 3.0f, { 1.0f, 0.9f, 0.25f, 1.0f }, EMIT_POTION_YELLOW));
		// Shared ENEMY projectile emitter (cactus needles etc.) -- CPU particles so they're real
		// PhysicsWorld rows tiles/the player can collide with, same reasoning as EMIT_WEAPON.
		// Lives on the player's slot purely for lookup convenience (FindByTag); gravity 0 =
		// needles fly straight.
		playerEmitters.push_back(Emitter::MakeCPU(ctx, { 4.0f, 4.0f }, 0.0f, PT_ENEMY_SHOT, 1 << 1, 1 << 1, EMIT_ENEMY_SHOT));
		// Bomb detonation burst -- same shared dustEffect pool trick as EMIT_COIN (the per-Spawn
		// color is the START color, so one registered effect serves many looks): orange-red and
		// bigger than dust, so a detonation reads as fire instead of another landing puff.
		playerEmitters.push_back(Emitter::MakeGPU(renderer, dustEffect, 6.0f, { 1.0f, 0.45f, 0.1f, 1.0f }, EMIT_EXPLOSION));
	}

	// Stomp-to-kill's onCollision callback, registered HERE (not in MapReader) specifically so it
	// can reach `emitters`/`playerIdx` directly via [this] -- ResolveParticle (run for every enemy
	// every frame by UpdateWorldParallel, since enemies aren't isPlayerControlled) already finds the
	// player as a non-tile neighbor and fires this whenever they overlap.
	// Spawning the dust burst DIRECTLY in the callback, at the moment of the stomp, instead of
	// trying to set ctx.entities.isGrounded[...] and let the normal wasGrounded-edge-detection code
	// in Update() pick it up: that code (step 5) already ran for this frame using ApplyPhysics's own
	// CheckGrounding result BEFORE this callback ever fires (this fires from step 9's
	// UpdateWorldParallel, later in the same frame) -- and next frame, CheckGrounding overwrites
	// isGrounded again before the landing check ever sees the manually-set value. No timing
	// trick makes that path work; spawning here directly sidesteps it entirely.
	for (const EnemyInstance& enemy : enemies) {
		int e = enemy.worldIdx;
		world.hasCollisionEvent[e] = 1;
		// [this], not [this, &sound] -- `sound` here would resolve to the CONSTRUCTOR PARAMETER
		// (it shares the member's name, so it shadows it inside this scope), and &sound captures a
		// reference to that parameter's stack storage, which is gone the instant the constructor
		// returns. This lambda is stored into world.onCollision[e] and fires much later during
		// actual gameplay -- every call was dereferencing a dangling reference to freed stack
		// memory, which is why the crash surfaced inside PlaySound() (first real memory access:
		// locking m_VoicesMutex) despite this having nothing to do with an actual data race. Just
		// [this] is correct and safe: without the shadowing local capture, `sound` inside the
		// lambda body resolves to the MEMBER (this->sound), which is a raw pointer to the
		// SoundManager main.cpp owns for the entire program's lifetime -- always valid.
		world.onCollision[e] = [this](int self, int other, PlatformerPhysics2D::PhysicsWorld& world, float dx, float dy) {
			int enemySlot = world.entitySlot[self];

			// Bullet hit -- checked FIRST and returns early, since `other` here is a particle, not
			// the player, and would otherwise get filtered out by the "other != playerIdx" guard
			// right below. Same isGhosted-at-0-health pattern as a stomp kill; the bullet particle
			// itself is destroyed via life=0 (safe here specifically because compressArrays() only
			// ever compacts PARTICLE rows -- see the isGhosted-vs-life=0 comment elsewhere in this
			// file for why that distinction matters for entities but not particles).
			if (world.isParticle[other]) {
				int pSlot = world.particleSlot[other];
				// life > 0 gate: a bullet's life=0 doesn't physically remove its row until the
				// end-of-frame commit, so a frame with several substeps re-fires this callback
				// on the SAME dead bullet each remaining substep -- health-- per substep is why
				// one shot dealt 2-3 damage while a stomp (self-gating via the velY>0 bounce
				// check) always dealt exactly 1. Distinct live bullets still each land their hit.
				if (pSlot != -1 && world.life[other] > 0.0f && world.particles.particleType[pSlot] == PT_WEAPON_IMPACT) {
					if (enemySlot != -1) {
						ctx.entities.hitTimer[enemySlot] = kEnemyHitFlashDuration;
						if (ctx.entities.health[enemySlot] > 0) ctx.entities.health[enemySlot]--;
					}
					if (enemySlot == -1 || ctx.entities.health[enemySlot] == 0) {
						world.isGhosted[self] = 1;
						world.velX[self] = 0.0f;
						world.velY[self] = 0.0f;
						world.gravity[self] = 0.0f;
					}
					world.life[other] = 0.0f;
				}
				return;
			}

			if (other != playerIdx) return; // ignore enemy-vs-enemy and anything else non-tile/non-bullet

			int playerSlot = world.entitySlot[other];

			// Bombs trigger on stomp: explode immediately, damage player if in range
			if (dy > 0.0f && world.velY[other] > 0.0f) {
				auto it = std::find_if(this->enemies.begin(), this->enemies.end(),
					[self](const EnemyInstance& e) { return e.worldIdx == self; });
				if (it != this->enemies.end() && it->type == EnemyAIType::Bomb) {
					constexpr float kExplosionRadius = 80.0f;
					DirectX::XMFLOAT2 bombPos{ world.posX[self], world.posY[self] };

					// Omnidirectional orange burst -- EMIT_EXPLOSION, not the white landing dust
					if (Emitter* explosion = emitters.FindByTag(world.emitterSlot[playerIdx], EMIT_EXPLOSION)) {
						static std::mt19937 explosionRng(std::random_device{}());
						std::uniform_real_distribution<float> speed(-170.0f, 170.0f);
						for (int n = 0; n < 24; ++n) {
							explosion->Spawn(bombPos, { speed(explosionRng), speed(explosionRng) }, 0.5f, 6.0f);
						}
					}

					// Damage player if in range
					float dx = world.posX[other] - world.posX[self];
					float dy = world.posY[other] - world.posY[self];
					float distSq = dx * dx + dy * dy;
					if (distSq <= (kExplosionRadius * kExplosionRadius) && playerSlot != -1) {
						if (this->ctx.entities.health[playerSlot] > 0) this->ctx.entities.health[playerSlot]--;
						this->ctx.entities.hitTimer[playerSlot] = this->kPlayerInvulnDuration;
						float knockDir = (world.posX[other] >= world.posX[self]) ? 1.0f : -1.0f;
						world.velX[other] = knockDir * this->kKnockbackSpeedX;
						world.velY[other] = -this->kKnockbackSpeedY;
						if (this->ctx.entities.health[playerSlot] == 0) this->OnPlayerFell();

						this->sound->PlaySound(JLib::ExeRelativeA("sound\\qubodup-cfork-ccby3-jump.flac").c_str(), 0.5f);
					}

					// Destroy the bomb
					world.isGhosted[self] = 1;
					world.velX[self] = 0.0f;
					world.velY[self] = 0.0f;
					world.gravity[self] = 0.0f;
					return; // bomb destroyed, skip normal stomp
				}
			}

			// dy = thisEnemyY - otherY (see ResolveParticle's call site) -- dy > 0 means the
			// player's center is ABOVE this enemy's, and still falling (velY > 0): a stomp.
			// Any other contact direction/velocity is "colliding without bouncing" -- damages the
			// PLAYER with knockback instead (below).
			if (dy > 0.0f && world.velY[other] > 0.0f) {
				world.velY[other] = -400.0f; // fixed bounce impulse -- always bounces, kill or not
				// this->sound, not bare sound -- the constructor PARAMETER of the same name shadows
				// the member for this whole constructor body (including inside this lambda), so an
				// unqualified `sound` here would try to reach that parameter again, which is exactly
				// the dangling-capture problem this fix is for. this-> forces it to the member.
				this->sound->PlaySound(JLib::ExeRelativeA("sound\\qubodup-cfork-ccby3-jump.flac").c_str(), 0.5f);
				Emitter* dust = emitters.FindByTag(world.emitterSlot[playerIdx], EMIT_DUST);
				if (dust) {
					DirectX::XMFLOAT2 feetPos{ world.posX[playerIdx], world.posY[playerIdx] + world.size[playerIdx].y * 0.5f };
					static std::mt19937 stompRng(std::random_device{}());
					std::uniform_real_distribution<float> speedX(-70.0f, 70.0f);
					std::uniform_real_distribution<float> speedY(-110.0f, -40.0f);
					for (int n = 0; n < 10; ++n) {
						dust->Spawn(feetPos, { speedX(stompRng), speedY(stompRng) }, 0.5f, 4.0f);
					}
				}

				if (enemySlot != -1) {
					this->ctx.entities.hitTimer[enemySlot] = this->kEnemyHitFlashDuration; // drives slime*Hit in Draw()
					if (this->ctx.entities.health[enemySlot] > 0) this->ctx.entities.health[enemySlot]--;
				}
				// health == 0 (or no slot at all -- shouldn't happen for a spawned enemy, but don't
				// silently leave an unkillable row if it did) is what actually kills it. isGhosted,
				// NOT life=0 -- setting life<=0 makes compressArrays() (called every Update() for
				// particle reclaiming) physically shift this row out, which cascades into shifting
				// every row spawned AFTER it too, INCLUDING THE PLAYER (enemies are interspersed
				// among map cells, not guaranteed to come after the player in spawn order) --
				// silently invalidating playerIdx with nothing to catch it. isGhosted is already
				// skipped by ResolveParticle's neighbor loop and every sensor, so the row just sits
				// there permanently inert instead -- no index anywhere ever moves.
				if (enemySlot == -1 || this->ctx.entities.health[enemySlot] == 0) {
					world.isGhosted[self] = 1;
					world.velX[self] = 0.0f;
					world.velY[self] = 0.0f;
					world.gravity[self] = 0.0f;
				}
			}
			else if (playerSlot != -1 && this->ctx.entities.hitTimer[playerSlot] <= 0.0f) {
				// Colliding without bouncing -- damages the player and knocks them back, gated on
				// hitTimer so standing/overlapping an enemy for several frames during the knockback
				// itself doesn't re-trigger damage every one of those frames (a brief invulnerability
				// window, same field enemies use as a pure hit-flash timer -- see its comment).
				if (this->ctx.entities.health[playerSlot] > 0) this->ctx.entities.health[playerSlot]--;
				this->ctx.entities.hitTimer[playerSlot] = this->kPlayerInvulnDuration;

				float knockDir = (world.posX[other] >= world.posX[self]) ? 1.0f : -1.0f;
				world.velX[other] = knockDir * this->kKnockbackSpeedX;
				world.velY[other] = -this->kKnockbackSpeedY;

				if (this->ctx.entities.health[playerSlot] == 0) {
					// Same "player has died" entry point falling off the map uses -- see
					// OnPlayerFell()'s comment. Handles lives/Game-Over/respawn; health itself gets
					// reset back to 3 for free by MapReader on whichever fresh GameplayScene that
					// path constructs (full level rebuild, not just a position reset).
					this->OnPlayerFell();
				}
			}
			};
	}

	// Coin pickup -- same non-tile onCollision callback path as the enemy stomp above, just
	// unconditional on contact (no direction/velocity check -- touching a coin from any side
	// collects it) and no bounce.
	for (int c : collectibles) {
		world.hasCollisionEvent[c] = 1;
		world.onCollision[c] = [this](int self, int other, PlatformerPhysics2D::PhysicsWorld& world, float dx, float dy) {
			if (other != playerIdx) return;

			// isGhosted, NOT life=0 -- see the enemy stomp callback above for why: this coin is
			// spawned BEFORE the player in map order (row 1 vs row 2), so life=0 here would shift
			// the player's actual row out from under playerIdx the instant it's collected.
			world.isGhosted[self] = 1;
			world.velX[self] = 0.0f;
			world.velY[self] = 0.0f;
			world.gravity[self] = 0.0f;
			coinCount++;
			this->sound->PlaySound(JLib::ExeRelativeA("sound\\coin.flac").c_str(), 0.5f);
			Emitter* coin = emitters.FindByTag(world.emitterSlot[playerIdx], EMIT_COIN);
			if (coin) {
				DirectX::XMFLOAT2 pos{ world.posX[self], world.posY[self] };
				static std::mt19937 coinRng(std::random_device{}());
				std::uniform_real_distribution<float> speedX(-50.0f, 50.0f);
				std::uniform_real_distribution<float> speedY(-90.0f, -30.0f);
				for (int n = 0; n < 8; ++n) {
					coin->Spawn(pos, { speedX(coinRng), speedY(coinRng) }, 0.4f, 3.0f);
				}
			}
			};
	}
	
	for (int end : levelEnd) {
		world.hasCollisionEvent[end] = 1;
		// ONLY arms a flag -- same deferral discipline as OnPlayerFell()/respawnPending, but the
		// stakes are higher here: this callback fires inside ResolveParticle, on a WORKER thread,
		// in the middle of UpdateWorldParallel's ParallelFor over this very world. Calling
		// ReplaceScene from here (the first version did) destroys this GameplayScene -- including
		// the PhysicsWorld the other parallel chunks are still iterating -- mid-pass: instant
		// use-after-free on world.hasCollisionEvent/posX/everything. Update() performs the actual
		// level switch after the substep loop, at the same proven-safe point the respawn rebuild
		// uses. (Doing file I/O for the next map inside a physics substep was also wrong.)
		world.onCollision[end] = [this](int self, int other, PlatformerPhysics2D::PhysicsWorld& w, float dx, float dy) {
			if (other != playerIdx) return;
			levelCompletePending = true;
		};
	}

	
	PhysicsSystem::LinkGroups(ctx);

	// Everything above used spawn-time row indices, which are only valid until the first
	// commit. Capture Handles for everything held across frames, then run the initial
	// commit -- it builds the grid (there is no insert()/clear() path anymore; the sorted
	// commit IS the grid build) so the first substep's sensors have something to query --
	// and refresh the raw-index mirrors.
	if (playerIdx != -1) hPlayer = world.handleOf(playerIdx);
	hPlatforms.clear();
	for (int p : platforms) hPlatforms.push_back(world.handleOf(p));
	hEnemies.clear();
	for (const EnemyInstance& e : enemies) hEnemies.push_back(world.handleOf(e.worldIdx));
	hCollectibles.clear();
	for (int c : collectibles) hCollectibles.push_back(world.handleOf(c));
	hLevelEnd.clear();
	for (int l : levelEnd) hLevelEnd.push_back(world.handleOf(l));

	world.commit(&grid);
	RefreshIndices();
}

void GameplayScene::RefreshIndices()
{
	playerIdx = world.resolve(hPlayer);
	for (size_t k = 0; k < hPlatforms.size(); ++k)
		platforms[k] = world.resolve(hPlatforms[k]);
	for (size_t k = 0; k < hEnemies.size(); ++k)
		enemies[k].worldIdx = world.resolve(hEnemies[k]);
	for (size_t k = 0; k < hCollectibles.size(); ++k)
		collectibles[k] = world.resolve(hCollectibles[k]);
	for (size_t k = 0; k < hLevelEnd.size(); ++k)
		levelEnd[k] = world.resolve(hLevelEnd[k]);
}

void GameplayScene::HandleInput(float dt)
{
	// Gamepad rides ALONGSIDE the keyboard (never instead of it) -- both are polled every frame and
	// whichever moved last wins, which is what PC players expect. Pad 0 only; JLib::InputManager
	// reads it over Raw Input HID, so gamepad support costs the process zero extra threads.
	using IM = JLib::InputManager;
	const bool padOn = input->IsGamepadConnected(0);

	if (input->IsKeyPressed(VK_ESCAPE) || (padOn && input->IsButtonPressed(IM::GamepadMenu))) {
		sound->Stop(music);
		cmdQ.push_back(CMD::QUIT);
	}
	if (respawnPending) return; // still falling/waiting out the death delay -- no player control
	// 5. Player input (replaces the source project's raylib InputSystem::HandlePlayerVelocityInput).
	int playerSlot = world.entitySlot[playerIdx];
	float moveDir = 0.0f;

	if (input->IsKeyDown('D')) moveDir += 1.0f;
	if (input->IsKeyDown('A')) moveDir -= 1.0f;
	if (padOn && moveDir == 0.0f) {
		// D-pad is digital (full tilt); the stick is analog but the movement model here is a
		// direction, not a speed, so anything past the deadzone counts as a full push. Keyboard
		// takes precedence when both are active -- no fighting over moveDir.
		if (input->IsButtonDown(IM::GamepadDPadRight))     moveDir += 1.0f;
		else if (input->IsButtonDown(IM::GamepadDPadLeft)) moveDir -= 1.0f;
		else {
			const float lx = input->GetLeftStickX();
			if (lx > 0.0f) moveDir = 1.0f;
			else if (lx < 0.0f) moveDir = -1.0f;
		}
	}
	ctx.entities.moveIntent[playerSlot] = moveDir;
	// isFacingRight is otherwise-unused for the player (EnemyAI.h reuses the same EntityTable
	// column for a completely different purpose -- patrol direction -- on enemy rows; same
	// "storage only, meaning depends on caller" pattern, no conflict since it's per-row). Nothing
	// set it for the player at all until now. Only updated on actual input (moveDir != 0) so
	// releasing both keys keeps facing whichever way the player was last actually moving, instead
	// of snapping back to a default every time input goes idle.
	if (moveDir > 0.0f) ctx.entities.isFacingRight[playerSlot] = 1;
	else if (moveDir < 0.0f) ctx.entities.isFacingRight[playerSlot] = 0;
	if (wallJumpLockTimer > 0.0f) {
		wallJumpLockTimer -= dt;
	}
	else if (moveDir != 0.0f) {
		// Accelerate toward the target speed instead of hard-setting velX -- lets
		// PhysicsSystem::ApplyPhysics's ground friction actually decelerate the player when
		// moveDir drops to 0, instead of friction never mattering because input overwrote velX
		// every frame regardless. moveDir == 0 below: velX is left alone entirely for friction
		// to act on (see the activelyDriven gate in ApplyPhysics).
		float target = moveDir * playerMoveSpeed;
		float accel = playerAccel * dt;
		float diff = target - world.velX[playerIdx];
		if (std::fabs(diff) <= accel) world.velX[playerIdx] = target;
		else world.velX[playerIdx] += accel * (diff > 0.0f ? 1.0f : -1.0f);
	}
	else if (ctx.entities.isGrounded[playerSlot]) {
		// Active braking: grounded + no input = plant your feet. The engine's passive decel
		// (world.friction * 2000 = ~300px/s^2 at .15) takes a FULL SECOND to stop a 300px/s
		// run -- ~9 tiles of slide, straight off any platform you land on. Feet multiply the
		// surface's grip, they don't replace it: the brake SCALES with the floor's authored
		// friction (baseline .15 = full kPlayerBrakeDecel, ~0.1s stop; an ice tile at .01
		// brakes at ~1/15th of that and genuinely slides). GAME-FEEL rule, so it lives here,
		// not in PlatformerPhysics2D -- engine friction alone still governs particles/AI. Airborne is
		// deliberately excluded: releasing input mid-jump keeps momentum (air control feel).
		constexpr float kPlayerBrakeDecel = 3000.0f;   // px/s^2 on baseline .15 friction
		constexpr float kBaselineFriction = 0.15f;
		int floorID = PhysicsSystem::TileBelowSensor(playerIdx, world, ctx.grid, ctx.tiles);
		if (floorID == -1) {
			// Same TileMap-fast-path blind spot ApplyPhysics hit: a moving platform is a world
			// row, not a TileMap cell -- reach it through the parent link instead.
			int par = ctx.entities.parentEntity[playerSlot];
			if (par != -1 && world.isPlatform[par]) floorID = par;
		}
		float ratio = (floorID != -1) ? (world.friction[floorID] / kBaselineFriction) : 1.0f;
		float brake = kPlayerBrakeDecel * ratio * dt;
		if (std::fabs(world.velX[playerIdx]) <= brake) world.velX[playerIdx] = 0.0f;
		else world.velX[playerIdx] -= brake * (world.velX[playerIdx] > 0.0f ? 1.0f : -1.0f);
	}

	// One jump edge for both devices -- computed ONCE so a wall-jump can't consume the keyboard
	// press and the pad press separately in the same frame.
	const bool jumpPressed = input->IsKeyPressed(VK_SPACE) ||
	                         (padOn && input->IsButtonPressed(IM::GamepadA));

	bool canJump = ctx.entities.isGrounded[playerSlot] && ctx.entities.jumpsRemaining[playerSlot] > 0;
	if (jumpPressed && canJump) {
		world.velY[playerIdx] = -ctx.entities.jumpForce[playerSlot];
		ctx.entities.isJumping[playerSlot] = true;
		sound->PlaySound(JLib::ExeRelativeA("sound\\jump.wav").c_str(), 0.5f);

		if (ctx.entities.jumpsRemaining[playerSlot] > 0) ctx.entities.jumpsRemaining[playerSlot]--;
	}
	bool isSliding = ctx.entities.isSliding[playerSlot];

	if (jumpPressed && isSliding) {
		float intent = ctx.entities.moveIntent[playerSlot];
		float kickDir = (intent > 0.0f) ? -1.0f : 1.0f;
		world.velY[playerIdx] = -ctx.entities.jumpForce[playerSlot];
		world.velX[playerIdx] = kickDir * ctx.entities.jumpForce[playerSlot];
		ctx.entities.isJumping[playerSlot] = true;
		if (ctx.entities.jumpsRemaining[playerSlot] > 0) ctx.entities.jumpsRemaining[playerSlot]--;
		wallJumpLockTimer = wallJumpLockDuration;
		sound->PlaySound(JLib::ExeRelativeA("sound\\jump.wav").c_str(), 0.5f);
	}

	// Fire a bullet -- IsKeyPressed (not IsKeyDown), so holding Shift doesn't spam a particle every
	// single frame; one shot per actual press. EMIT_WEAPON is the CPU emitter allocated back in the
	// constructor (see its comment) -- FindByTag, not assuming push_back order, same reasoning as
	// every other emitters.FindByTag call site in this file.
	// VK_LSHIFT/VK_RSHIFT both work here, and so does generic VK_SHIFT now: the Raw Input
	// InputManager disambiguates the specific key from scancode data AND mirrors it into the
	// generic VK slot (GameInput only ever reported the specific key, which is why older code in
	// this file standardized on VK_LSHIFT/VK_RSHIFT -- still fine, no need to churn call sites).
	if (input->IsKeyPressed('E') || (padOn && input->IsButtonPressed(IM::GamepadX))) {
		Emitter* weapon = emitters.FindByTag(world.emitterSlot[playerIdx], EMIT_WEAPON);
		if (weapon) {
			const float kBulletSpeed = 500.0f;
			bool facingRight = ctx.entities.isFacingRight[playerSlot] != 0;
			// Spawn a little outside the player's own hitbox. The +4 padding alone is NOT what
			// keeps the bullet alive (the callback below ignores the shooter for that) -- it just
			// stops the bullet visually clipping through the player sprite on its first frame.
			float spawnX = world.posX[playerIdx] + (facingRight ? 1.0f : -1.0f) * (world.size[playerIdx].x * 0.5f + 4.0f);
			DirectX::XMFLOAT2 pos{ spawnX, world.posY[playerIdx] };
			DirectX::XMFLOAT2 vel{ facingRight ? kBulletSpeed : -kBulletSpeed, 0.0f };
			int bulletIdx = weapon->Spawn(pos, vel, 2.0f); // 2s lifetime -- despawns on its own if it never hits anything
			// Self-destruct on a TILE hit -- without this the bullet just kept existing after a
			// wall/slope hit (ResolveParticle's tile branches physically stop it but never
			// destroyed it on their own), drifting at the wall until its 2s lifetime ran out
			// instead of disappearing on impact like a bullet should.
			// The shooter is EXEMPT: the bullet spawns half-overlapping the player's hitbox edge,
			// and while running in the shoot direction the player (300 px/s) closes on the bullet
			// (500 px/s) fast enough that the overlap survives into the same substep's collision
			// pass -- the bullet then killed ITSELF on the player, which is why shooting silently
			// did nothing whenever you were moving. [this] + a live playerIdx compare (not a
			// captured copy) so the check stays correct after commits reshuffle row indices.
			// ENTITIES (enemies) are also exempt HERE: the enemy's own callback both applies the
			// damage and kills the bullet, and it gates on the bullet's life > 0 -- if this
			// callback also killed it, whichever of the two happened to run first in a substep
			// would decide whether the enemy took damage at all. One owner, no race.
			if (bulletIdx != -1) {
				world.hasCollisionEvent[bulletIdx] = 1;
				world.onCollision[bulletIdx] = [this](int self, int other, PlatformerPhysics2D::PhysicsWorld& world, float dx, float dy) {
					if (other == this->playerIdx) return;         // never die on the shooter
					if (world.entitySlot[other] != -1) return;    // enemy hits: the enemy's callback owns those
					world.life[self] = 0.0f;
				};
			}
		}
	}
}

void GameplayScene::Update(bool& isRunning, float dt)
{
	if (playerIdx == -1) return;

	// Fixed-timestep accumulator. Update() is called once per RENDERED frame, but every physics
	// call inside RunPhysicsSubstep() used a hardcoded PHYSICS_STEP (~120Hz) amount of simulated
	// time regardless of dt -- so running it exactly once per Update() call meant total simulated
	// seconds-per-real-second scaled directly with framerate: ~0.5x speed at 60fps, ~2x speed at
	// 240fps, etc. Accumulating real dt and draining it in PHYSICS_STEP-sized slices makes the
	// substep COUNT vary with framerate instead of the SPEED -- more/fewer slow-motion-esque steps
	// per frame, but the same total simulated time per real second either way.
	constexpr float kMaxFrameDt = 0.25f; // caps a big stall (breakpoint, alt-tab) from spiraling into a huge catch-up burst
	physicsAccumulator += std::min(dt, kMaxFrameDt);
	while (physicsAccumulator >= PHYSICS_STEP) {
		RunPhysicsSubstep();
		physicsAccumulator -= PHYSICS_STEP;
	}

	// Deferred level switch -- armed by the level-end trigger's onCollision callback (which runs
	// on a worker thread mid-physics and must not touch the scene stack; see its comment).
	// Handled here FIRST, before death/game-over ticking: reaching the exit wins even if the
	// level timer would have expired the same frame. Same must-return-after-ReplaceScene rule
	// as the respawn rebuild below -- `this` is destroyed by the swap.
	if (levelCompletePending && !respawnPending && !gameOverTriggered) {
		levelCompletePending = false;
		BinaryLevel next = BinaryMapFormat::Load(LevelPath(level + 1));
		if (next.width > 0) {
			sound->Stop(music);
			// Lives, health, and coins carry forward across levels.
			int playerSlot = world.entitySlot[playerIdx];
			uint8_t survivingHealth = (playerSlot != -1) ? ctx.entities.health[playerSlot] : 3;
			SceneManager::ReplaceScene(std::make_unique<GameplayScene>(font, sound, renderer, input, camera,
				quadMesh, slopeUpRightMesh, slopeUpLeftMesh, tileTexture, dustEffect, width, height, lives, level + 1,
				survivingHealth, coinCount));
			return;
		}
		// No level_<N+1>.tmap: that was the last level. TODO: a real victory scene -- for now
		// pop back to the start menu instead of soft-locking in an empty world.
		sound->Stop(music);
		SceneManager::PopScene();
		return;
	}

	// Level timer -- see its comment in GameplayScene.h. Not ticked while already dying/waiting
	// (respawnPending/gameOverTriggered/isDead all imply OnPlayerFell() already fired once).
	if (!respawnPending && !gameOverTriggered && !isDead) {
		levelTimer -= dt;
		if (bumpNudgeTimer > 0.0f) bumpNudgeTimer -= dt; // bump-nudge visual clock (see Draw)
		if (levelTimer <= 0.0f) {
			levelTimer = 0.0f;
			OnPlayerFell(); // same "player has died, however caused" entry point as falling/enemy damage
		}
	}

	// Game-Over delay -- gives knightDeath time to actually play (see deathAnimDuration's comment
	// in GameplayScene.h) before GameOverScene takes over the stack and this scene stops being
	// drawn entirely. Ticked with real dt, same reasoning as respawnTimer below.
	if (gameOverTriggered) {
		deathAnimTimer += dt;
		if (deathAnimTimer >= deathAnimDuration) {
			// MUST return immediately after -- same reasoning as ReplaceScene below: nothing on
			// `this` is touched after the push, since PushScene doesn't destroy `this` scene, but
			// staying consistent with the rest of Update()'s control flow here is simpler than
			// reasoning about which specific fields would still be safe to read.
			SceneManager::PushScene(std::make_unique<GameOverScene>(renderer, input, font, sound, quadMesh, tileTexture));
			return;
		}
	}

	// Respawn delay -- ticked here (once per REAL rendered frame, via GameTimer's own wall clock)
	// rather than inside RunPhysicsSubstep, so it elapses in real seconds regardless of how many
	// physics substeps a given frame happens to run. The actual position/velocity reset was
	// previously done immediately inside OnPlayerFell() via a busy-wait loop that blocked Update()
	// (and therefore Draw()) entirely for 3 seconds -- freezing the whole game, not just the
	// camera, and making the player appear to stop falling instead of continuing to fall while
	// the camera held still. Now OnPlayerFell() only arms respawnPending; the reset happens here,
	// once the delay has actually elapsed in real time, without blocking anything in between.
	if (respawnPending) {
		respawnTimer.Tick();
		if (respawnTimer.TotalTime() >= 3.0f) {
			// Lives were already decremented (and the Game-Over-instead-of-wait check already
			// made) back in OnPlayerFell() at the moment of falling -- respawnPending is only
			// ever armed there when a respawn is actually going to happen, so this block just
			// performs the reset.
			//
			// Full level rebuild, not just a player position/velocity reset -- resetting only the
			// player left the rest of the world exactly as the fall left it, which breaks once
			// anything in the level can PERMANENTLY change: a "canFall" platform that already fell
			// away stays gone, so a respawn could drop the player into a level that's no longer
			// completable. Constructing a brand new GameplayScene via SceneManager::ReplaceScene
			// re-runs the same MapReader::BuildWorldFromMap construction path this scene itself
			// went through, guaranteeing a fresh, correct level state (platforms/enemies/coins all
			// back at their map-defined starting positions) instead of hand-patching whichever
			// specific pieces happen to be capable of going wrong. `lives` is threaded through so
			// it survives the reset; everything else (coinCount, killed enemies, etc.) intentionally
			// does NOT survive -- a respawn is a full retry of the level, not a checkpoint resume.
			//
			// MUST return immediately after -- ReplaceScene destroys the unique_ptr this GameplayScene
			// instance lives in as part of the swap, so `this` (and every member on it, including
			// respawnPending itself) is a dangling pointer for the rest of this function's execution
			// the instant this call returns. Touching anything else on `this` afterward (even just
			// falling through to code below this if-block) would be a use-after-free.
			sound->Stop(music);
			// `level` threaded through so dying retries the CURRENT level -- without it the
			// rebuild's default levelNumber silently restarted the whole run at level_1.
			SceneManager::ReplaceScene(std::make_unique<GameplayScene>(font, sound, renderer, input, camera,
				quadMesh, slopeUpRightMesh, slopeUpLeftMesh, tileTexture, dustEffect, width, height, lives, level));
			return;
		}
	}

	// 10. Quit -- GameplayScene is the only scene on the stack this pass (no menu underneath),
	// so just stop the app loop; don't PopScene() an otherwise-empty stack.
	if (!cmdQ.empty() && cmdQ.back() == CMD::QUIT) {
		cmdQ.pop_back();
		SceneManager::PopScene();
		return;
	}
}

void GameplayScene::RunPhysicsSubstep()
{
	// Purely visual, but ticked here (not in Update()/Draw()) so it advances by PHYSICS_STEP on
	// the same fixed clock as everything else -- consistent with the accumulator fix, this stays
	// framerate-independent instead of ticking once per rendered frame with a variable dt.
	coinAnim.Update(PHYSICS_STEP);
	// Tick every enemy type's shared anims (same all-of-a-kind-in-sync simplification as
	// coinAnim -- see the old slime members' comment, now table-driven).
	for (auto& set : enemyAnims) {
		set.idle.Update(PHYSICS_STEP);
		set.walk.Update(PHYSICS_STEP);
		set.hit.Update(PHYSICS_STEP);
	}

	// Tick every hit-flash/invulnerability timer down. See kPlayerInvulnDuration/
	// kEnemyHitFlashDuration's comment in GameplayScene.h for what hitTimer means per row.
	{
		int playerSlot = world.entitySlot[playerIdx];
		if (playerSlot != -1 && ctx.entities.hitTimer[playerSlot] > 0.0f) {
			ctx.entities.hitTimer[playerSlot] = std::max(0.0f, ctx.entities.hitTimer[playerSlot] - PHYSICS_STEP);
		}
		for (const EnemyInstance& enemy : enemies) {
			if (enemy.worldIdx == -1) continue; // row died and its handle went stale
			int slot = world.entitySlot[enemy.worldIdx];
			if (slot != -1 && ctx.entities.hitTimer[slot] > 0.0f) {
				ctx.entities.hitTimer[slot] = std::max(0.0f, ctx.entities.hitTimer[slot] - PHYSICS_STEP);
			}
		}
	}

	// Idle/run/hit/death selection -- roll is loaded but still unused (no input bound to it).
	// isDead (set by OnPlayerFell()) takes priority over everything else and locks currentKnightAnim
	// to knightDeath: without this guard, OnPlayerFell() setting currentKnightAnim = &knightDeath
	// did nothing visible, because this block would just pick a different animation again the very
	// next substep based on moveIntent/hitTimer, before a single frame of the death pose was drawn.
	// hitTimer > 0 (set by the damage-collision callback) takes priority over movement while alive
	// -- shows knightHit regardless of moveIntent for the duration of the invuln window.
	{
		int moveSlot = world.entitySlot[playerIdx];
		GridSpriteAnimation* nextAnim;
		if (isDead) {
			nextAnim = &knightDeath;
		}
		else {
			bool moving = moveSlot != -1 && ctx.entities.moveIntent[moveSlot] != 0.0f;
			bool isHit = moveSlot != -1 && ctx.entities.hitTimer[moveSlot] > 0.0f;
			nextAnim = isHit ? &knightHit : (moving ? &knightRun : &knightIdle);
		}
		if (nextAnim != currentKnightAnim) {
			nextAnim->Reset();
			currentKnightAnim = nextAnim;
		}
		currentKnightAnim->Update(PHYSICS_STEP); // keep ticking even while dead, so knightDeath's frames actually play
	}

	// 1. (was: rebuild the grid -- TWICE, once here and once after platforms moved.) The
	// grid is now built by the commit() at the END of each substep (and by the constructor
	// for the very first one): the sorted-runs SpatialGrid has no insert()/clear() at all.
	// Platform movement below happens AFTER this substep's grid was built, so queries see
	// slightly stale platform bins -- covered by design: multi-cell platforms live in the
	// overflow bin (scanned by every query regardless of position), and single-cell movers
	// are absorbed by query()'s one-full-cell margin. See SpatialGrid.h.

	// 2-3. Snapshot positions, then move platforms using this frame's fixed physics step.
	PhysicsSystem::SyncPrevPositions(world);
	PhysicsSystem::UpdatePlatforms(ctx, PHYSICS_STEP);


	// Bump-block check -- BEFORE ApplyPhysics, because its ceiling constraint zeroes velY on the
	// bonk, and "moving upward into this tile" is exactly the signal we need. TileAboveSensor is
	// the same probe CheckCeilingCollision uses, so anything that registers here is the same hit
	// the physics resolves this substep; OnBlockBump no-ops unless that tile is an authored block.
	if (world.velY[playerIdx] < 0.0f) {
		int bumped = PhysicsSystem::TileAboveSensor(playerIdx, world, ctx.grid, ctx.tiles);
		if (bumped != -1) OnBlockBump(bumped);
	}

	// 6. Player physics step.
	PhysicsSystem::ApplyPhysics(playerIdx, ctx, PHYSICS_STEP);
	{
		FILE* f = nullptr;
		fopen_s(&f, "walljump.log", "a");
		if (f) { fprintf(f, "AFTER APPLYPHYSICS: velX=%.2f posX=%.2f\n", world.velX[playerIdx], world.posX[playerIdx]); fclose(f); }
	}
	// (Step 7, platform-carry, used to live here as a separate position-delta carry. It's now
	// entirely handled inside PhysicsSystem::PlatformPhysics, called from ApplyPhysics above --
	// both the horizontal carry and the falling-platform link-severing. Nothing to do here.)

	// Fall-death check -- right after ApplyPhysics settles this substep's posY, before anything
	// else (dust, camera, enemy AI) reads a position that's about to get reset out from under it.
	if (world.posY[playerIdx] > mapBottomY) {
		OnPlayerFell();
	}

	// Landing dust: isGrounded only tells you the CURRENT state, not when it changed, so the
	// edge (false -> true, this frame only) has to be tracked here rather than inside PlatformerPhysics2D.
	{
		int playerSlot = world.entitySlot[playerIdx];
		bool isGroundedNow = ctx.entities.isGrounded[playerSlot];
		if (isGroundedNow && !wasGrounded && world.emitterSlot[playerIdx] != -1) {
			// FindByTag, not "loop every emitter in the slot" -- the slot also holds the CPU
			// weapon-impact emitter now (see the constructor), and looping all of them here would
			// fire 10 CPU particles per landing on top of the GPU dust, needlessly burning through
			// ParticleTable's small pool for something that's supposed to be GPU-only.
			Emitter* dust = emitters.FindByTag(world.emitterSlot[playerIdx], EMIT_DUST);
			DirectX::XMFLOAT2 feetPos{ world.posX[playerIdx], world.posY[playerIdx] + world.size[playerIdx].y * 0.5f };

			// {0,0} velocity meant every particle just sat at feetPos and fell straight down under
			// gravity -- one visible dot, not a "dust kicked up" look. A real burst needs each
			// particle launched with its own upward + sideways kick so MakeCPU's gravity (300, set
			// at construction) arcs them back down along different paths instead of one shared line.
			static std::mt19937 dustRng(std::random_device{}());
			std::uniform_real_distribution<float> dustSpeedX(-70.0f, 70.0f);
			std::uniform_real_distribution<float> dustSpeedY(-110.0f, -40.0f); // negative = up

			constexpr int kDustBurstCount = 10;
			if (dust) {
				for (int n = 0; n < kDustBurstCount; ++n) {
					DirectX::XMFLOAT2 vel{ dustSpeedX(dustRng), dustSpeedY(dustRng) };
					dust->Spawn(feetPos, vel, 0.5f, 4.0f);
				}
			}
		}
		wasGrounded = isGroundedNow;
	}
	int playerSlot = world.entitySlot[playerIdx];

	if (ctx.entities.isSliding[playerSlot] && world.emitterSlot[playerIdx] != -1) {
		// Same FindByTag reasoning as the landing burst above -- this fires every physics frame
		// while sliding, so looping the whole slot would've meant a CPU weapon particle every
		// frame too, exhausting the 16-slot ParticleTable in under a second.
		Emitter* dust = emitters.FindByTag(world.emitterSlot[playerIdx], EMIT_DUST);
		DirectX::XMFLOAT2 feetPos{ world.posX[playerIdx], world.posY[playerIdx] + world.size[playerIdx].y * 0.5f };

		static std::mt19937 dustRng(std::random_device{}());
		std::uniform_real_distribution<float> dustSpeedX(-70.0f, 70.0f);
		std::uniform_real_distribution<float> dustSpeedY(-110.0f, -40.0f); // negative = up

		constexpr int kDustBurstCount = 1;
		if (dust) {
			for (int n = 0; n < kDustBurstCount; ++n) {
				DirectX::XMFLOAT2 vel{ dustSpeedX(dustRng), dustSpeedY(dustRng) };
				dust->Spawn(feetPos, vel, 0.5f, 4.0f);
			}
		}
	}

	// One dispatch call per enemy -- see EnemyAI.h's UpdateEnemyAI for the actual per-type intent
	// logic. It only sets velX; the real movement/collision (including stomp-kill, via the
	// onCollision callback MapReader registered on each enemy) happens in step 9's
	// UpdateWorldParallel below, same parallel path every other non-player cell already uses.
	for (EnemyInstance& enemy : enemies) { // non-const: Shooter cooldown (fireTimer) lives here
		if (enemy.worldIdx == -1) continue; // row died and its handle went stale
		bool wantsAttack = UpdateEnemyAI(enemy, ctx, world.posX[playerIdx], world.posY[playerIdx]);

		// Shooter firing: AI signaled intent, scene owns cooldown + the actual projectile
		// (needs emitters/sound the AI header deliberately can't reach). Serial section, so
		// direct Spawn + callback registration is safe -- same as the player's 'E' bullet.
		if (enemy.fireTimer > 0.0f) enemy.fireTimer -= PHYSICS_STEP;
		if (wantsAttack && enemy.type == EnemyAIType::Shooter && enemy.fireTimer <= 0.0f) {
			constexpr float kShotSpeed = 220.0f, kShotCooldown = 1.5f, kShotLifetime = 3.0f;
			enemy.fireTimer = kShotCooldown;
			if (Emitter* shot = emitters.FindByTag(world.emitterSlot[playerIdx], EMIT_ENEMY_SHOT)) {
				int e = enemy.worldIdx;
				float dx = world.posX[playerIdx] - world.posX[e];
				float dy = world.posY[playerIdx] - world.posY[e];
				float dist = std::sqrt(dx * dx + dy * dy);
				if (dist > 1.0f) {
					DirectX::XMFLOAT2 pos{ world.posX[e], world.posY[e] - world.size[e].y * 0.25f };
					int shotIdx = shot->Spawn(pos, { dx / dist * kShotSpeed, dy / dist * kShotSpeed }, kShotLifetime);
					if (shotIdx != -1) {
						world.hasCollisionEvent[shotIdx] = 1;
						world.onCollision[shotIdx] = [this](int self, int other, PlatformerPhysics2D::PhysicsWorld& world, float, float) {
							// Player hit: same damage/invuln/knockback rules as enemy contact
							// (gated on hitTimer so a shot during the invuln window is free).
							if (other == this->playerIdx) {
								int ps = world.entitySlot[other];
								if (ps != -1 && this->ctx.entities.hitTimer[ps] <= 0.0f) {
									if (this->ctx.entities.health[ps] > 0) this->ctx.entities.health[ps]--;
									this->ctx.entities.hitTimer[ps] = this->kPlayerInvulnDuration;
									float knockDir = (world.posX[other] >= world.posX[self]) ? 1.0f : -1.0f;
									world.velX[other] = knockDir * this->kKnockbackSpeedX;
									world.velY[other] = -this->kKnockbackSpeedY;
									if (this->ctx.entities.health[ps] == 0) this->OnPlayerFell();
								}
								world.life[self] = 0.0f; // needle spends itself on the player
								return;
							}
							if (world.entitySlot[other] != -1) return; // pass through enemies (incl. the shooter)
							world.life[self] = 0.0f;                   // tiles/platforms stop it
						};
					}
				}
			}
		}

		// Bomb/Balloon detonation -- SIBLING of the Shooter block above, never inside it (an
		// enemy is exactly one type, so a branch nested in the Shooter's `if` is dead code --
		// that was the bug that made bombs completely inert). The AI's attack signal (Bomb:
		// player inside trigger radius; FlyerBomb: drifted within detonate range) LIGHTS THE
		// FUSE rather than detonating instantly: aiState 0->1, fireTimer = fuse (ticked down by
		// the shared decrement at the top of this loop), and the enemy's hitTimer is set for the
		// same window so Draw shows the def's hit row (Bomb: ignite frames; Balloon: the pop) as
		// a visible wind-up. Both AIs hold position while aiState != 0. Detonation then bursts
		// the orange EMIT_EXPLOSION emitter and damages the player only if they're still inside
		// the BLAST radius -- triggering it and backing off is the intended counterplay.
		if ((enemy.type == EnemyAIType::Bomb || enemy.type == EnemyAIType::FlyerBomb)
			&& !world.isGhosted[enemy.worldIdx]) {
			constexpr float kBombFuse = 0.6f, kBlastRadius = 80.0f;
			int e = enemy.worldIdx;
			if (wantsAttack && enemy.aiState == 0) {
				enemy.aiState = 1;           // fuse lit
				enemy.fireTimer = kBombFuse;
				int slot = world.entitySlot[e];
				if (slot != -1) ctx.entities.hitTimer[slot] = kBombFuse; // ignite anim while the fuse burns
			}
			if (enemy.aiState == 1 && enemy.fireTimer <= 0.0f) {
				DirectX::XMFLOAT2 bombPos{ world.posX[e], world.posY[e] };

				// Omnidirectional orange burst (a blast goes every direction, unlike rising dust)
				if (Emitter* boom = emitters.FindByTag(world.emitterSlot[playerIdx], EMIT_EXPLOSION)) {
					static std::mt19937 boomRng(std::random_device{}());
					std::uniform_real_distribution<float> speed(-170.0f, 170.0f);
					for (int n = 0; n < 24; ++n) {
						boom->Spawn(bombPos, { speed(boomRng), speed(boomRng) }, 0.5f, 6.0f);
					}
				}

				int playerSlot = world.entitySlot[playerIdx];
				float dx = world.posX[playerIdx] - world.posX[e];
				float dy = world.posY[playerIdx] - world.posY[e];
				if ((dx * dx + dy * dy) <= (kBlastRadius * kBlastRadius) && playerSlot != -1) {
					if (ctx.entities.health[playerSlot] > 0) ctx.entities.health[playerSlot]--;
					ctx.entities.hitTimer[playerSlot] = kPlayerInvulnDuration;
					float knockDir = (world.posX[playerIdx] >= world.posX[e]) ? 1.0f : -1.0f;
					world.velX[playerIdx] = knockDir * kKnockbackSpeedX;
					world.velY[playerIdx] = -kKnockbackSpeedY;
					if (ctx.entities.health[playerSlot] == 0) OnPlayerFell();
				}
				// TODO: real detonation SFX -- jump.flac is a placeholder like the potion pickups.
				sound->PlaySound(JLib::ExeRelativeA("sound\\DeathFlash.flac").c_str(), 0.5f);

				// Spent: ghost the row, same inert-not-compacted discipline as stomped enemies.
				world.isGhosted[e] = 1;
				world.velX[e] = 0.0f;
				world.velY[e] = 0.0f;
				world.gravity[e] = 0.0f;
			}
		}
	}

	// Camera-follow moved to Draw() -- it now uses the same interpolated player position
	// everything else renders with (see Draw()'s comment), instead of snapping straight to
	// whatever this substep's raw posX/posY happen to be.

	// 8. Group/trigger bookkeeping for moving platforms. PHYSICS_STEP, not a real dt parameter --
	// this whole function only ever runs as one fixed-size slice of the accumulator loop in
	// Update(), so everything in here shares the same fixed clock.
	PhysicsSystem::UpdateGroups(ctx, PHYSICS_STEP);

	// 9. Everything else (particles, non-player-controlled cells) in parallel chunks.
	unsigned int workers = std::max(1u, std::thread::hardware_concurrency() - 1);
	int chunkSize = std::max(1, (int)(world.activeCount / workers));
	PhysicsSystem::UpdateWorldParallel(ctx, PHYSICS_STEP, JLib::TaskScheduler::Instance(), chunkSize);

	// UpdateWorldParallel just decremented every particle's life, but nothing actually reclaims
	// a dead row's slot (entitySlot/platformSlot/particleSlot) until the commit runs --
	// without this, ParticleTable's fixed pool fills up permanently after enough landings
	// (each dust particle's slot is allocated but never freed) and the NEXT spawn() throws
	// "ParticleTable exhausted", caught by main.cpp's updateTask try/catch, which sets
	// isRunning=false -- a clean-looking exit(0) that's actually this starvation bug.
	// commit(&grid) is the fused pass: kill-compaction + cell-sort + NEXT substep's grid
	// build in one O(n) sweep. It reorders every row, so RefreshIndices() must follow
	// immediately -- nothing between these two lines may touch a raw row index.
	world.commit(&grid);
	RefreshIndices();
}

namespace {
	// Spent-block sprite mapping, authored from world_tileset.png's four block color families:
	// each family is { '!' , '?' } -> blank/spent (same-palette solid). An index outside the
	// table keeps its own sprite (unknown blocks just stop yielding, they don't recolor).
	uint16_t SpentSpriteFor(uint16_t sprite) {
		switch (sprite) {
		case 33: case 32: return 16;
		case 50: case 35: return 18;
		case 37: case 36: return 20;
		case 40: case 39: return 22;
		default:          return sprite;
		}
	}
}

void GameplayScene::SpawnPotion(int containsType, DirectX::XMFLOAT2 pos)
{
	// kContains -> world_tileset sprite: 2=red(+HP) 3=blue(+life) 4=green(+time) 5=yellow(+coins).
	uint16_t sprite;
	switch (containsType) {
	case 2: sprite = 129; break;
	case 3: sprite = 128; break;
	case 4: sprite = 112; break;
	case 5: sprite = 113; break;
	default: return;
	}
	// Pops upward, gravity brings it back down to rest ON the block (Mario item reveal).
	// isEntity so its life never counts down (ResolveChunk only ages non-entity non-tile rows)
	// -- it stays until collected. Same layer/mask as coins so ResolveParticle pairs it with
	// the player.
	int idx = world.spawn(PlatformerPhysics2D::ShapeType::RECT, PlatformerPhysics2D::Vec2{ 12.0f, 12.0f },
	                      pos.x, pos.y - 4.0f, 0.0f, -140.0f, 1.0f,
	                      981.0f, 1 << 1, 1 << 1, true, false, false);
	if (idx == -1) return;
	world.tileID[idx] = sprite; // entity rows don't use tileID -- carry the sprite in it, platform-style
	world.hasCollisionEvent[idx] = 1;
	world.onCollision[idx] = [this, containsType](int self, int other, PlatformerPhysics2D::PhysicsWorld& world, float, float) {
		if (other != playerIdx) return;
		// isGhosted, not life=0 -- same row-shift reasoning as the coin callback above.
		world.isGhosted[self] = 1;
		world.velX[self] = 0.0f;
		world.velY[self] = 0.0f;
		world.gravity[self] = 0.0f;
		switch (containsType) {
		case 2: { // red: +1 health (cap 9, same as the old auto-collect)
			int es = world.entitySlot[playerIdx];
			if (ctx.entities.health[es] < 9) ctx.entities.health[es] += 1;
			break;
		}
		case 3: if (lives < 9) lives += 1; break;          // blue: 1-up
		case 4: levelTimer += 60.0f; break;                 // green: +time
		case 5: coinCount += 5; break;                      // yellow: coin cache
		}
		// TODO: per-potion pickup sounds; coin.flac placeholder for all four.
		this->sound->PlaySound(JLib::ExeRelativeA("sound\\coin.flac").c_str(), 0.5f);
		// Color-matched sparkle burst at the potion, same shape as the coin pickup's.
		int tag = (containsType == 2) ? EMIT_POTION_RED
		        : (containsType == 3) ? EMIT_POTION_BLUE
		        : (containsType == 4) ? EMIT_POTION_GREEN
		        : EMIT_POTION_YELLOW;
		if (Emitter* fx = emitters.FindByTag(world.emitterSlot[playerIdx], tag)) {
			DirectX::XMFLOAT2 pos{ world.posX[self], world.posY[self] };
			static std::mt19937 potionRng(std::random_device{}());
			std::uniform_real_distribution<float> speedX(-50.0f, 50.0f);
			std::uniform_real_distribution<float> speedY(-90.0f, -30.0f);
			for (int n = 0; n < 8; ++n)
				fx->Spawn(pos, { speedX(potionRng), speedY(potionRng) }, 0.4f, 3.0f);
		}
		};
}

void GameplayScene::OnBlockBump(int tileRow)
{
	if (m_binLevel.width == 0 || m_binLevel.tileWidth == 0) return;
	// Static tile rows never move: position IS the cell (same recovery Draw's setTileSprite uses).
	int col = (int)std::floor(world.posX[tileRow] / (float)m_binLevel.tileWidth);
	int row = (int)std::floor(world.posY[tileRow] / (float)m_binLevel.tileHeight);
	if (col < 0 || row < 0 || col >= (int)m_binLevel.width || row >= (int)m_binLevel.height) return;
	BinaryCell& cell = m_binLevel.CellAt(col, row);

	int  contains  = (int)m_binLevel.GetProperty(cell.properties, BinaryProps::kContains, 0.0f);
	bool breakable = m_binLevel.GetProperty(cell.properties, BinaryProps::kBreakable, 0.0f) != 0.0f;
	if (contains == 0 && !breakable) return; // plain tile -- just the physics bonk

	// Visual hop (see the member comment). Note: a SPENT unbreakable block clears kContains and
	// returns above, so it no longer nudges -- fine (it also uses a plain solid sprite shared
	// with normal ground, so there's no way to tell it apart from ground anymore anyway).
	bumpNudgeCol = col; bumpNudgeRow = row; bumpNudgeTimer = kBumpNudgeDuration;

	DirectX::XMFLOAT2 blockTop{ world.posX[tileRow], world.posY[tileRow] - m_binLevel.tileHeight * 0.5f };

	// ---- Contents first, then breakability (multi-coin-brick semantics): a bump that yields
	// never also breaks the block on the same hit. ----
	if (contains != 0) {
		float count = m_binLevel.GetProperty(cell.properties, BinaryProps::kContainCount, 1.0f);
		if (count > 0.0f) {
			if (contains == 1) { // coin: auto-collect (Mario bump-coin), same feedback as pickup
				coinCount++;
				sound->PlaySound(JLib::ExeRelativeA("sound\\coin.flac").c_str(), 0.5f);
				if (Emitter* coinFx = emitters.FindByTag(world.emitterSlot[playerIdx], EMIT_COIN)) {
					static std::mt19937 bumpRng(std::random_device{}());
					std::uniform_real_distribution<float> speedX(-50.0f, 50.0f);
					std::uniform_real_distribution<float> speedY(-90.0f, -30.0f);
					for (int n = 0; n < 8; ++n)
						coinFx->Spawn(blockTop, { speedX(bumpRng), speedY(bumpRng) }, 0.4f, 3.0f);
				}
			}
			else if (contains >= 2 && contains <= 5) {
				// Potion (2=red/HP 3=blue/1up 4=green/time 5=yellow/coins): spawns as a REAL
				// entity popping out the block's top and resting there until touched -- the
				// effect applies in its pickup onCollision, not here. Safe to direct-spawn:
				// OnBlockBump runs in RunPhysicsSubstep's serial section.
				SpawnPotion(contains, blockTop);
			}
			count -= 1.0f;
			m_binLevel.SetProperty(cell.properties, BinaryProps::kContainCount, count);
			if (count <= 0.0f) {
				m_binLevel.SetProperty(cell.properties, BinaryProps::kContains, 0.0f);
				// Out of contents: unbreakable ?-blocks go visually SPENT (Draw recovers static
				// tile sprites from m_binLevel by position, so writing the cell is all it takes);
				// breakable ones keep their sprite -- the NEXT bump breaks them.
				if (!breakable) cell.spriteIndex = SpentSpriteFor(cell.spriteIndex);
			}
			return;
		}
	}

	if (breakable) {
		// Kill the row (life<=0: skipped by Draw, both sensor paths, and the grid on rebuild),
		// silence the TileMap cell, and empty the authored cell so the sprite disappears.
		world.life[tileRow] = 0.0f;
		m_tiles.behavior[(size_t)row * m_binLevel.width + col] = PlatformerPhysics2D::TileBehavior::None;
		cell.spriteIndex = 0xFFFF;
		cell.behaviorType = BEHAVIOR_BACKGROUND;
		cell.properties.clear();
		if (Emitter* dust = emitters.FindByTag(world.emitterSlot[playerIdx], EMIT_DUST)) {
			static std::mt19937 breakRng(std::random_device{}());
			std::uniform_real_distribution<float> speedX(-70.0f, 70.0f);
			std::uniform_real_distribution<float> speedY(-120.0f, -20.0f);
			for (int n = 0; n < 10; ++n)
				dust->Spawn(blockTop, { speedX(breakRng), speedY(breakRng) }, 0.35f, 3.0f);
		}
		// TODO: pick a break sound (none of the current SFX quite fits).
	}
}

void GameplayScene::OnPlayerFell()
{
	// Respawn at the map's '@' spawn point after a 3s delay, during which the camera holds still
	// (frozenCameraPos, set below) while the player keeps falling/simulating normally, instead of
	// the whole game freezing. The DETECTION (RunPhysicsSubstep's fallDeathY check) is the reusable
	// part; the actual position/velocity reset happens in Update() once respawnTimer clears 3s.
	if (respawnPending || gameOverTriggered) return; // already falling/waiting/over -- don't re-arm

	// isDead locks currentKnightAnim to knightDeath (see RunPhysicsSubstep's selection block) for
	// BOTH outcomes below -- setting currentKnightAnim directly here used to do nothing visible,
	// since that block would just override it again next substep with no way to know death had
	// started. deathAnimTimer only actually matters for the Game-Over branch (see its own comment
	// in GameplayScene.h); the respawn branch's existing 3s wait is already long enough on its own.
	isDead = true;
	deathAnimTimer = 0.0f;
	currentKnightAnim = &knightDeath;
	knightDeath.Reset();
	frozenCameraPos = camera.position;
	sound->PlaySound(JLib::ExeRelativeA("sound\\vgdeathsound.wav").c_str(), 0.5f);

	// Lives check happens HERE, at the moment of falling -- not after a wait already elapsed. If
	// this fall would be the one after the last life was already used (lives already at 0), there's
	// nothing to respawn to: Game Over. The fall that USES the last life (lives 1 -> 0) still
	// respawns normally below -- only a fall that happens with lives ALREADY at 0 is Game Over.
	if (lives == 0) {
		sound->Stop(music); // stop the level music right away -- StartMenuScene restarts its own
		// title music once it's back on top (see its Update()'s comment)
// gameOverTriggered, NOT respawnPending -- see gameOverTriggered's comment in
// GameplayScene.h for why reusing respawnPending here was the actual bug: Update()'s
// respawn-rebuild block also watches respawnPending, and (since respawnTimer is never
// Reset() on this path) would immediately fire its OWN branch this same frame. The actual
// PushScene(GameOverScene) now happens in Update(), gated on deathAnimTimer -- NOT here
// immediately -- so the death animation actually gets time to play before GameOverScene
// takes over the stack and this scene stops being drawn at all.
		gameOverTriggered = true;
		return;
	}
	lives--;
	sound->PlaySound(JLib::ExeRelativeA("sound\\vgdeathsound.wav").c_str(), 0.5f);
	respawnPending = true;
	respawnTimer.Reset();
}

DirectX::XMFLOAT4 GameplayScene::ColorForTileID(uint32_t tileID) const
{
	switch (tileID) {
	case 0: return TileColors::Gray;      // background ('.')
	case 1: return TileColors::SkyBlue;   // player ('@')
	case 2: return TileColors::DarkGray;  // solid ('#')
	case 3: return TileColors::Orange;    // moving platform ('_')
	case 4: return TileColors::DarkGray;  // slope up-right ('/') -- rendered as a plain rect, see plan
	case 5: return TileColors::DarkGray;  // slope up-left ('\')
	case 6: return DirectX::XMFLOAT4{ 0.8f, 0.1f, 0.1f, 1.0f }; // patrol enemy ('E') fallback -- Draw() uses slimeGreen* instead
	case 7: return DirectX::XMFLOAT4{ 1.0f, 0.9f, 0.1f, 1.0f }; // coin ('$')
	case 8: return DirectX::XMFLOAT4{ 0.6f, 0.1f, 0.8f, 1.0f }; // chase enemy ('C') fallback -- Draw() uses slimePurple* instead
	case 9: return DirectX::XMFLOAT4{ 0.0f, 1.0f, 1.0f, 1.0f }; // level-end trigger (cyan)
	default: return DirectX::XMFLOAT4{ 1.0f, 1.0f, 1.0f, 1.0f };
	}
}

void GameplayScene::Draw()
{
	// Render-position interpolation. RunPhysicsSubstep() runs 0, 1, or several times per rendered
	// frame (the whole point of Update()'s fixed-timestep accumulator) -- Draw() used to just
	// render wherever the LAST substep left everything, which visibly holds still then jumps
	// whenever a frame gets zero substeps (common when PHYSICS_STEP doesn't divide evenly into the
	// actual frame rate), most noticeable vertically since gravity/jumps change posY faster than
	// walking changes posX. `alpha` is how far INTO the next (not-yet-run) substep the accumulator
	// already is; lerping between prevPos (snapshotted at the start of the last substep that DID
	// run, via SyncPrevPositions) and the current pos smooths that out. Requires prevPos to be
	// seeded at spawn time (now is, in PhysicsWorld::spawn()) -- otherwise a freshly-spawned row's
	// prevPos would still be {0,0} and every new dust particle/coin-pickup burst would visibly
	// flash in from the world origin for one frame.
	float alpha = PHYSICS_STEP > 0.0f ? std::clamp(physicsAccumulator / PHYSICS_STEP, 0.0f, 1.0f) : 0.0f;

	if (respawnPending) {
		// Hold the camera where it was the instant the player fell -- see OnPlayerFell()'s
		// comment. The player keeps falling/simulating normally below this camera position;
		// only the camera itself stops following.
		auto screensize = renderer.GetScreenSize();
		renderer.SubmitText(*font, screensize.x / 2, screensize.y / 2, "You died! Respawning in {} seconds...", 1.0f, JLib::Colors::Red, 0.0f, JLib::TextAlign::Center, 2, 3 - (int)respawnTimer.TotalTime());

		camera.position = frozenCameraPos;
	}
	else if (playerIdx != -1) {
		// Dead-zone camera (replaces hard player-centering, which moved the whole screen for
		// every one-tile shuffle and turnaround). The camera only moves when the player pushes
		// against a small window around it; inside the window it stays put. Plus: horizontal
		// look-ahead (see camLookX) so the view favors where the player is FACING, a vertical
		// rule that ignores jump arcs (grounded-follow) so hops don't bounce the screen, and a
		// clamp to the level's pixel bounds so the view never shows past the edges.
		float px = world.prevPos[playerIdx].x + (world.posX[playerIdx] - world.prevPos[playerIdx].x) * alpha;
		float py = world.prevPos[playerIdx].y + (world.posY[playerIdx] - world.prevPos[playerIdx].y) * alpha;
		float dt = renderer.GetLastDeltaTime();
		int slot = world.entitySlot[playerIdx];

		constexpr float kCamDeadHalfW   = 24.0f;  // 1.5 tiles: sustained walking moves the camera, fidgeting doesn't
		constexpr float kCamDeadHalfH   = 48.0f;  // taller than a max jump arc, so airborne frames stay inside it
		constexpr float kCamLookAhead   = 32.0f;  // how far ahead of the facing direction the view settles
		constexpr float kCamLookSpeed   = 4.0f;   // 1/s: ease rate for the look-ahead pan on turnaround
		constexpr float kCamGroundSnap  = 8.0f;   // 1/s: vertical ease toward the player while grounded

		// Horizontal look-ahead target eases toward the facing direction (exp smoothing is
		// framerate-independent enough at these rates).
		float lookTarget = ctx.entities.isFacingRight[slot] ? kCamLookAhead : -kCamLookAhead;
		camLookX += (lookTarget - camLookX) * std::min(1.0f, kCamLookSpeed * dt);

		DirectX::XMFLOAT2 cam = camera.position; // persistent across frames -- the camera IS the state

		// Horizontal: only move when the (look-ahead-shifted) player leaves the dead zone.
		float focusX = px + camLookX;
		if      (focusX > cam.x + kCamDeadHalfW) cam.x = focusX - kCamDeadHalfW;
		else if (focusX < cam.x - kCamDeadHalfW) cam.x = focusX + kCamDeadHalfW;

		// Vertical: while grounded, ease toward the player's height (lands on a new platform ->
		// camera settles there). While airborne, don't chase the jump arc -- but a hard dead-zone
		// still catches long falls/tall ascents so the player can never leave the screen.
		if (ctx.entities.isGrounded[slot])
			cam.y += (py - cam.y) * std::min(1.0f, kCamGroundSnap * dt);
		if      (py > cam.y + kCamDeadHalfH) cam.y = py - kCamDeadHalfH;
		else if (py < cam.y - kCamDeadHalfH) cam.y = py + kCamDeadHalfH;

		// Clamp to level bounds: never show past the edges. Half the view in WORLD units is
		// screen/(2*zoom) (WorldToScreen scales by zoom). A level smaller than the view just
		// centers on that axis.
		DirectX::XMFLOAT2 screen = renderer.GetScreenSize();
		float halfViewW = screen.x * 0.5f / camera.zoom;
		float halfViewH = screen.y * 0.5f / camera.zoom;
		float levelW = (float)m_binLevel.width  * m_binLevel.tileWidth;
		float levelH = (float)m_binLevel.height * m_binLevel.tileHeight;
		if (levelW > halfViewW * 2.0f) cam.x = std::clamp(cam.x, halfViewW, levelW - halfViewW);
		else                           cam.x = levelW * 0.5f;
		if (levelH > halfViewH * 2.0f) cam.y = std::clamp(cam.y, halfViewH, levelH - halfViewH);
		else                           cam.y = levelH * 0.5f;

		camera.position = cam;
	}

	// Live back-buffer size, NOT the constructor-passed width/height (those are frozen at the
	// launch resolution). WorldToScreen centers the camera at screenSize*0.5 -- feeding it the
	// stale size kept the world's "center" at the old resolution's midpoint after a resize,
	// while the GPU particles (which read the real size from the constant buffer) rendered at
	// the correct new center. Same live size the HUD below already uses.
	DirectX::XMFLOAT2 liveScreen = renderer.GetScreenSize();

	BatchItem item;
	item.tex = tileTexture;

	// Recover a STATIC tile's authored sprite from m_binLevel and decode it to the right sheet +
	// UV -- identical to the editor's DrawLevel, so in-game tiles match what you painted instead
	// of every solid tile drawing sheet cell (0,0). Static tile rows never move, so the cell is
	// just position/tileSize. Same >=256 => secondary-sheet convention as the editor, and the
	// row/col unpack now uses THE SHEET'S OWN column count (GetFramesX) -- the old hardcoded %16
	// was wrong for both the 11-wide four-seasons tileset and the 4-wide platform sheet.
	// 0xFFFF (empty/unset) falls back to sheet cell 0.
	auto decodeSprite = [&](int sprite) {
		int idx = (sprite == 0xFFFF) ? 0 : sprite;
		JLib::GridSpriteSheet* sheet = tileSheet.get();
		if (idx >= 256) { idx -= 256; sheet = platformSheet.get(); }
		item.tex = sheet->GetTexture();
		item.uvScale = sheet->GetUVScale();
		// 16-wide unpack, mirroring the editor exactly -- see DrawLevel's comment: authored
		// indexes in saved levels bake this convention in, so it stays until a deliberate
		// re-encode (GetFramesX() is available for that rework).
		item.uvOffset = sheet->GetUVOffset(idx % 16, idx / 16);
	};
	// STATIC tiles never move, so recover their sprite from the authored cell (position/tileSize).
	// Moving things (platforms) can't use this -- they carry their sprite on the row instead.
	auto setTileSprite = [&](size_t i) {
		int col = (m_binLevel.tileWidth  > 0) ? (int)std::floor(world.posX[i] / (float)m_binLevel.tileWidth)  : -1;
		int row = (m_binLevel.tileHeight > 0) ? (int)std::floor(world.posY[i] / (float)m_binLevel.tileHeight) : -1;
		uint16_t sprite = 0xFFFF;
		if (col >= 0 && row >= 0 && col < (int)m_binLevel.width && row < (int)m_binLevel.height)
			sprite = m_binLevel.CellAt(col, row).spriteIndex;
		decodeSprite(sprite);
	};

	for (size_t i = 0; i < world.activeCount; ++i) {
		if (world.life[i] <= 0.0f) continue;
		if (world.isGhosted[i]) continue; // collected coins / stomped enemies -- see their onCollision callbacks
		// Level-end triggers (tileID 9, see spawnLevelEnd) are gameplay-only sensors -- invisible
		// in game, drawn only in the editor (cyan indicator). Platforms excluded from the check:
		// their tileID holds an authored SPRITE index, which can legitimately be 9.
		if (world.tileID[i] == 9 && !world.isPlatform[i]) continue;
		// Binary-level model: every tileID==0 tile ROW is a painted "Not Solid" decoration cell
		// (LevelInstantiator only spawns a row for a non-empty cell), so it now RENDERS its sprite
		// (was skipped as the old invisible '.' background). Only a genuinely spriteless leftover
		// would draw nothing -- the setTileSprite lookup falls back to sheet cell 0 for those.
		// Particles keep tileID 0 too but are handled by their own isParticle branch below.

		if (world.slopeType[i] == SlopeType::UP_RIGHT) item.mesh = slopeUpRightMesh;
		else if (world.slopeType[i] == SlopeType::UP_LEFT) item.mesh = slopeUpLeftMesh;
		else item.mesh = quadMesh;

		float renderX = world.prevPos[i].x + (world.posX[i] - world.prevPos[i].x) * alpha;
		float renderY = world.prevPos[i].y + (world.posY[i] - world.prevPos[i].y) * alpha;
		item.position = camera.WorldToScreen({ renderX, renderY }, liveScreen);
		item.size = { world.size[i].x, world.size[i].y };
		item.rotation = world.rotation[i];
		// item is reused across every iteration -- tex/uvOffset/uvScale/flipX must be reset in
		// EVERY branch below (not just the player one), or a leftover flag from one row leaks into
		// every row submitted right after it. flipX in particular was only ever SET in the player
		// branch, never reset elsewhere -- so once the player (facing left) was processed, every
		// tile/entity with a higher array index than the player inherited flipX=true and flipped
		// too, for the rest of this Draw() call. Resetting it here, unconditionally, before any
		// branch runs, means only the player branch below ever actually flips anything.
		item.flipX = false;
		if ((int)i == playerIdx) { // player -- knight sheet via currentKnightAnim, not a flat color
			item.tex = currentKnightAnim->GetTexture();
			item.uvOffset = currentKnightAnim->GetUVOffset();
			item.uvScale = currentKnightAnim->GetUVScale();
			item.flipX = !ctx.entities.isFacingRight[world.entitySlot[playerIdx]];
			item.color = JLib::Colors::White;
			item.zLayer = 1;

			// The player's hitbox (world.size[playerIdx], 16x16) is deliberately tight for
			// collision, but knight.png's cells are 32x32 -- drawing at the hitbox's size squished
			// the sprite down, and CENTERING that smaller quad on the hitbox's center (the default
			// item.position/size set above, shared with every other row) left the character's
			// visual feet floating above where the hitbox's bottom edge -- and therefore the actual
			// ground collision -- resolves to. Draw at the sprite's real size instead, anchored by
			// its BOTTOM edge to the hitbox's bottom edge, so the feet always land exactly on the
			// hitbox's own ground contact point regardless of how tall the sprite is drawn.
			constexpr float kKnightSpriteSize = 32.0f;
			// Measured by scanning actual opaque pixels per cell (idle/run/roll frames all agree):
			// the character's feet sit at y=27 within the 32px-tall cell, not y=32 -- ~5px of
			// transparent padding is baked into every frame below the feet. Anchoring the QUAD's
			// raw bottom edge to the hitbox (the first attempt) still left that 5px as a visible
			// gap, since the drawn feet are 5px above the quad's actual bottom edge. Anchor the
			// FEET pixel row itself instead.
			constexpr float kKnightFeetBottomPx = 27.0f;
			float hitboxBottom = renderY + world.size[i].y * 0.5f;
			float spriteCenterY = hitboxBottom + kKnightSpriteSize * 0.5f - kKnightFeetBottomPx;
			item.size = { kKnightSpriteSize, kKnightSpriteSize };
			item.position = camera.WorldToScreen({ renderX, spriteCenterY }, liveScreen);
		}
		else if (world.isPlatform[i]) { // moving platform -- authored sprite carried on the row (tileID
			// holds the spriteIndex; detected via the physics flag, NOT a magic tileID, since a
			// platform's sprite value could equal any tileID -- and checked BEFORE the tileID==7
			// coin branch for exactly that reason).
			decodeSprite((int)world.tileID[i]);
			item.zLayer = (world.layer[i] == 0) ? 0 : 1;
			item.color = JLib::Colors::White;
		}
		else if (world.tileID[i] == 7) { // coin ('$') -- animated sheet, not a flat color
			item.tex = coinAnim.GetTexture();
			item.uvOffset = coinAnim.GetUVOffset();
			item.uvScale = coinAnim.GetUVScale();
			item.color = { 1.0f, 1.0f, 1.0f, 1.0f };
			item.zLayer = 1;
		}
		else if (world.isEntity[i] && (world.tileID[i] == 129 || world.tileID[i] == 128 ||
		                                world.tileID[i] == 112 || world.tileID[i] == 113)) {
			// Bump-block potion -- entity row carrying its tileset sprite in tileID (platform-
			// style; SpawnPotion stamps it). isEntity guard so a STATIC tile painted with a
			// potion sprite still takes the setTileSprite path below, not this one.
			decodeSprite((int)world.tileID[i]);
			item.color = JLib::Colors::White;
			item.zLayer = 1;
		}
		else if (world.isParticle[i]) {
			int pSlot = world.particleSlot[i];
			int pType = (pSlot != -1) ? world.particles.particleType[pSlot] : PT_DEFAULT;

			item.zLayer = 2;
			if (pType == PT_WEAPON_IMPACT) {
				item.tex = tileTexture;          // or a real bullet texture, once you have one
				item.uvOffset = { 0.0f, 0.0f };
				item.uvScale = { 1.0f, 1.0f };
				item.color = { 1.0f, 0.9f, 0.2f, 1.0f }; // yellow, distinct from dust
			}
			else if (pType == PT_ENEMY_SHOT) {
				item.tex = tileTexture;          // cactus needle -- flat color until a real sprite
				item.uvOffset = { 0.0f, 0.0f };
				item.uvScale = { 1.0f, 1.0f };
				//item.color = { 0.3f, 0.9f, 0.3f, 1.0f }; // green needle, distinct from the player's yellow
				item.color = JLib::Colors::Red;
			}
			else {
				item.tex = tileTexture;
				item.uvOffset = { 0.0f, 0.0f };
				item.uvScale = { 1.0f, 1.0f };
				item.color = { 1.0f, 1.0f, 1.0f, 1.0f };
			}
		}
		else if (world.tileID[i] == 2) { // solid tile -- real authored sprite (was always cell 0,0)
			setTileSprite(i);
			item.zLayer = 0; // STATIC tiles draw BEHIND entities (player/enemies at zLayer 1) --
			                 // was zLayer 1 (== player), so ground tiles drew OVER the player's
			                 // feet; invisible before when tiles were all light sheet-cell-0.
			item.color = JLib::Colors::White;
			// Bump-nudge: draw-time-only hop for the one recently-bumped block cell. The row's
			// physics position never moves -- only this quad. Offset is in world px, scaled by
			// zoom because item.position is already in screen space here.
			if (bumpNudgeTimer > 0.0f &&
			    bumpNudgeCol == (int)std::floor(world.posX[i] / (float)m_binLevel.tileWidth) &&
			    bumpNudgeRow == (int)std::floor(world.posY[i] / (float)m_binLevel.tileHeight)) {
				float progress = 1.0f - (bumpNudgeTimer / kBumpNudgeDuration); // 0 -> 1 over the hop
				item.position.y -= kBumpNudgeHeight * std::sin(3.14159265f * progress) * camera.zoom;
			}
		}
		else if (world.tileID[i] == 4 || world.tileID[i] == 5) { // slope -- authored sprite on the slope mesh
			setTileSprite(i); // mesh already picked above from slopeType
			item.zLayer = 0;  // behind entities, same reason as solid tiles above
			item.color = JLib::Colors::White;
		}
		else if (world.tileID[i] == 0) { // "Not Solid" decoration -- real sprite, VISIBLE (was transparent)
			setTileSprite(i);
			item.zLayer = 0;
			item.color = JLib::Colors::White;
		}
		else if (world.tileID[i] >= kEnemyTileIDBase &&
		         world.tileID[i] < kEnemyTileIDBase + (uint32_t)kEnemyDefCount) {
			// Enemy -- ONE table-driven branch for every type (replaces the per-type 6/8
			// hardcoding). tileID carries kEnemyTileIDBase + def index; anims come from the
			// matching EnemyAnimSet built in the ctor. Idle vs walk picked from THIS row's own
			// velX (the AI sets it, see EnemyAI.h); hitTimer > 0 (stomp callback) overrides with
			// hit. All-of-a-kind stays visually in sync, same simplification as coinAnim.
			int defIdx = (int)(world.tileID[i] - kEnemyTileIDBase);
			EnemyAnimSet& set = enemyAnims[defIdx];
			bool moving = world.velX[i] != 0.0f;
			bool isHit = ctx.entities.hitTimer[world.entitySlot[i]] > 0.0f;
			GridSpriteAnimation& anim = isHit ? set.hit : (moving ? set.walk : set.idle);
			item.tex = anim.GetTexture();
			item.flipX = !ctx.entities.isFacingRight[world.entitySlot[i]];
			item.uvOffset = anim.GetUVOffset();
			item.uvScale = anim.GetUVScale();
			item.color = { 1.0f, 1.0f, 1.0f, 1.0f };
			item.zLayer = 1;

			// Sprite drawn at the def's own size, anchored bottom-edge to the (per-def) hitbox
			// bottom: the pack's enemies sit flush at their cell bottom (pixel-scan confirmed on
			// the slimes). If a future sheet bakes in bottom padding like knight.png does, that
			// type will need a per-def feet offset -- add it to EnemyDef.
			const EnemyDef& def = kEnemyDefs[defIdx];
			float hitboxBottom = renderY + world.size[i].y * 0.5f;
			float spriteCenterY = hitboxBottom - def.spriteH * 0.5f;
			item.size = { def.spriteW, def.spriteH };
			item.position = camera.WorldToScreen({ renderX, spriteCenterY }, liveScreen);
		}
		else {
			item.tex = tileTexture;
			item.uvOffset = { 0.0f, 0.0f };
			item.uvScale = { 1.0f, 1.0f };
			item.zLayer = (world.layer[i] == 0) ? 0 : 1;
			item.color = ColorForTileID(world.tileID[i]);
		}
		// WorldToScreen (used for item.position above) scales POSITION by camera.zoom, but it has
		// no way to touch item.size -- that's a separate BatchItem field it never sees. Without
		// this, tile positions correctly spread apart/squeeze together at zoom != 1, but every
		// tile stayed the same pixel size regardless -- gaps between tiles at zoom > 1, overlapping
		// tiles at zoom < 1. Scaling size by the same zoom factor keeps tiles seamlessly adjacent
		// at any zoom level, matching how their positions already scale.
		item.size.x *= camera.zoom;
		item.size.y *= camera.zoom;
		renderer.Submit(item);
	}

	auto screenSize = renderer.GetScreenSize();
	BatchItem uiCoin;
	uiCoin.mesh = quadMesh;              // <- was missing
	uiCoin.tex = coinAnim.GetTexture();
	uiCoin.uvOffset = coinAnim.GetUVOffset();
	uiCoin.uvScale = coinAnim.GetUVScale();
	uiCoin.color = { 1.0f, 1.0f, 1.0f, 1.0f };
	uiCoin.zLayer = 2;
	uiCoin.position = { 32.0f, screenSize.y - 32.0f };
	uiCoin.size = { 32.0f, 32.0f };      // <- was missing, pick whatever HUD size you want
	renderer.Submit(uiCoin);
	renderer.SubmitText(*font, 50.0f, screenSize.y - 50.0f, "x " + std::to_string(coinCount), 1.0f, JLib::Colors::White, 2);
	BatchItem uiKnight;
	uiKnight.mesh = quadMesh;              // <- was missing
	uiKnight.tex = knightIdle.GetTexture();
	uiKnight.uvOffset = { 0.0f, 0.0f };
	uiKnight.uvScale = knightIdle.GetUVScale();
	uiKnight.color = JLib::Colors::White;
	uiKnight.zLayer = 2;
	uiKnight.position = { 200.0f, screenSize.y - 34.0f };
	uiKnight.size = { 32.0f, 32.0f };      // <- was missing, pick whatever HUD size you want
	renderer.Submit(uiKnight);
	renderer.SubmitText(*font, 220.0f, screenSize.y - 50.0f, "x " + std::to_string(lives), 1.0f, JLib::Colors::White, 2);
	renderer.SubmitText(*font, screenSize.x/2.0f - 200.0f, screenSize.y - 50.0f, "health: ", 1.0f, JLib::Colors::White, 2);
	if (playerIdx != -1) {
		for (int i = 0; i < ctx.entities.health[world.entitySlot[playerIdx]]; ++i) {
		BatchItem health;
		health.mesh = quadMesh;
		health.tex = tileTexture;
		health.uvOffset = { 0.0f, 0.0f };
		health.uvScale = { 1.0f, 1.0f };
		health.color = JLib::Colors::Red;
		health.zLayer = 2;
		health.position = { (float)(screenSize.x/2.0f + i * 34), screenSize.y - 34.0f };
		health.size = { 32.0f, 32.0f };
		renderer.Submit(health);
		}
	}

	// {:.0f} -- whole seconds, not a raw float with a long decimal tail (same reasoning as
	// GameOverScene's countdown text).
	renderer.SubmitText(*font, screenSize.x / 2.0f, 10.0f, "Time: {:.0f}", 1.0f, JLib::Colors::White, 0.0f,
		JLib::TextAlign::Center, 2, levelTimer);
}
