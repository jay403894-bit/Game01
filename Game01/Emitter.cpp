#include "pch.h"
#include "Emitter.h"

using namespace PlatformerPhysics2D;

Emitter Emitter::MakeCPU(PhysicsContext& ctx, Vec2 size, float gravity, int particleType, uint8_t layer, uint8_t mask, int tag)
{
	Emitter e;
	e.isGPU = false;
	e.ctx = &ctx;
	e.size = size;
	e.gravity = gravity;
	e.particleType = particleType;
	e.layer = layer;
	e.mask = mask;
	e.tag = tag;
	return e;
}

Emitter Emitter::MakeGPU(JLib::Renderer2D& renderer, uint32_t effectID, float size, DirectX::XMFLOAT4 color, int tag)
{
	Emitter e;
	e.isGPU = true;
	e.renderer = &renderer;
	e.effectID = effectID;
	e.gpuSize = size;
	e.color = color;
	e.tag = tag;
	return e;
}

int Emitter::Spawn(DirectX::XMFLOAT2 position, DirectX::XMFLOAT2 velocity, float lifetime, float offsetRadius)
{
	if (isGPU) {
		renderer->RequestSpawn(effectID, position, offsetRadius, velocity, lifetime, color, gpuSize);
		return -1;
	}

	int idx = ctx->world.spawn(ShapeType::CIRCLE, size, position.x, position.y, velocity.x, velocity.y,
	                            lifetime, gravity, layer, mask, false, false, true);
	if (idx == -1) return -1; // world full -- drop the spawn rather than throw, same as any other spawn() caller

	int slot = ctx->world.particleSlot[idx];
	if (slot != -1) ctx->world.particles.particleType[slot] = particleType;
	return idx;
}
