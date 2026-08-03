#pragma once
#include <DirectXMath.h>
#include <PhysicsWorld.h>
#include <PhysicsSystem.h>
#include <Renderer2D.h>

// One Spawn() call, two backends. CPU particles are rows in PhysicsWorld/ParticleTable -- they
// get simulated/collided by PhysicsSystem and drawn by GameplayScene::Draw like any other cell.
// GPU particles live entirely in RendererCore's compute-driven pool -- simulated and drawn on the
// GPU, never touching PhysicsWorld at all. Which backend a given Emitter hits is fixed at
// construction (MakeCPU/MakeGPU); callers that just want to spawn a particle don't need to care.
class Emitter
{
public:
	// tag is caller-defined and never interpreted here -- same "just an int, meaning lives at the
	// call site" convention as PlatformerPhysics2D's particleType. One emitter slot commonly holds several
	// emitters for different purposes (e.g. a CPU weapon-impact emitter AND a GPU dust emitter);
	// tag is how a caller picks which one it means without depending on push_back order. See
	// EmitterTable::FindByTag.
	static Emitter MakeCPU(PlatformerPhysics2D::PhysicsContext& ctx, PlatformerPhysics2D::Vec2 size, float gravity,
	                       int particleType, uint8_t layer = 0, uint8_t mask = 0, int tag = 0);
	static Emitter MakeGPU(JLib::Renderer2D& renderer, uint32_t effectID, float size, DirectX::XMFLOAT4 color, int tag = 0);

	// position/velocity are world units for a CPU emitter, screen units for a GPU emitter --
	// same as PhysicsWorld::spawn vs. Renderer2D::RequestSpawn already expect today.
	// Returns the spawned PhysicsWorld row index for a CPU emitter (-1 if the world was full and
	// the spawn was dropped, same as any other spawn() caller), or -1 always for a GPU emitter
	// (GPU particles aren't PhysicsWorld rows at all -- see the class comment). Lets a caller
	// attach a per-shot onCollision callback right after spawning (e.g. a bullet that should
	// destroy itself on its first hit) instead of every CPU particle type needing to be identical.
	int Spawn(DirectX::XMFLOAT2 position, DirectX::XMFLOAT2 velocity, float lifetime, float offsetRadius = 0.0f);

	int tag = 0;

private:
	bool isGPU = false;

	// CPU backend
	PlatformerPhysics2D::PhysicsContext* ctx = nullptr;
	PlatformerPhysics2D::Vec2 size{};
	float gravity = 0.0f;
	int particleType = 0;
	uint8_t layer = 0;
	uint8_t mask = 0;

	// GPU backend
	JLib::Renderer2D* renderer = nullptr;
	uint32_t effectID = 0;
	float gpuSize = 0.0f;
	DirectX::XMFLOAT4 color{};
};
