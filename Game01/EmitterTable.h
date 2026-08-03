#pragma once
#include <vector>
#include <stdexcept>
#include "Emitter.h"

// Stable-slot pool of emitter GROUPS -- the Game01-side counterpart to PlatformerPhysics2D's
// EntityTable/PlatformTable/ParticleTable, but can't live in PlatformerPhysics2D since Emitter depends on
// JLib::Renderer2D (see PhysicsWorld::emitterSlot's comment in PhysicsWorld.h for why).
//
// One slot != one Emitter -- same as PlatformTable bundling a dozen fields behind one
// platformSlot, one emitterSlot here points to a small COLLECTION of emitters, since an entity
// commonly wants more than one at once (e.g. a CPU dust emitter AND a GPU flame emitter). An
// entity opts in by calling Alloc(), storing the returned slot in world.emitterSlot[i], then
// push_back-ing as many Emitters (via MakeCPU/MakeGPU) into Get(slot) as it needs.
//
// Same freelist mechanics as the PlatformerPhysics2D tables -- Free() clears the slot's emitter list so a
// future occupant of this slot never inherits a stale entity's emitters.
class EmitterTable
{
public:
	explicit EmitterTable(size_t capacity) : slots(capacity), freelist(capacity) {
		for (size_t i = 0; i < capacity; ++i) freelist[i] = static_cast<int>(capacity - 1 - i);
		freeTop = static_cast<int>(capacity);
	}

	int Alloc() {
		if (freeTop <= 0) throw std::runtime_error("Game01: EmitterTable exhausted");
		return freelist[--freeTop];
	}

	void Free(int slot) {
		slots[slot].clear();
		freelist[freeTop++] = slot;
	}

	std::vector<Emitter>& Get(int slot) { return slots[slot]; }
	size_t Capacity() const { return slots.size(); }

	// Picks out one emitter from a slot's list by its caller-defined tag (e.g. "the CPU weapon
	// emitter" vs "the GPU dust emitter") instead of assuming push_back order. Returns nullptr if
	// this slot has no emitter with that tag -- callers should check before dereferencing.
	Emitter* FindByTag(int slot, int tag) {
		for (Emitter& e : slots[slot]) {
			if (e.tag == tag) return &e;
		}
		return nullptr;
	}

private:
	std::vector<std::vector<Emitter>> slots;
	std::vector<int> freelist;
	int freeTop = 0;
};
