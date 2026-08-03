#pragma once
#include <vector>
#include <memory>
#include <DirectXMath.h>
#include <Renderer2D.h>
#include <Camera2D.h>
#include <InputManager.h>
#include <PhysicsSystem.h>
#include <SoundManager.h>
#include <Font.h>
#include <SceneManager.h>
#include <Scene.h>
#include <Rect.h>
class GameOverScene : public Scene
{
public:
	// font/soundManager are raw, non-owning pointers, not shared_ptr -- GameOverScene only ever
	// exists on top of whatever pushed it (GameplayScene) on SceneManager's stack, and can never
	// outlive it (popping GameOverScene always happens before GameplayScene could be popped out
	// from under it), so there's no lifetime GameOverScene needs to co-own. GameplayScene's own
	// font/sound are std::unique_ptr<Font>/a by-value SoundManager respectively -- neither
	// converts to shared_ptr, which is what actually broke the PushScene call site.
	GameOverScene(JLib::Renderer2D& renderer, std::shared_ptr<JLib::InputManager> input, JLib::Font* font,
		JLib::SoundManager* soundManager, JLib::Mesh* quadMesh, JLib::TextureHandle tileTexture);
	void Update(bool& isRunning, float dt = 0.0f) override;
	void Draw() override;
	void HandleInput(float dt) override;
	~GameOverScene() override = default;
private:
	JLib::Renderer2D& renderer;
	std::shared_ptr<JLib::InputManager> input;
	JLib::Font* font;
	JLib::Mesh* quadMesh;
	JLib::SoundManager* soundManager;
	JLib::TextureHandle tileTexture; // FlushBatchTask/ResourceManager::Resolve requires a valid
	// Auto-return-to-title delay. Ticked in Update() with real dt (not a GameTimer -- Update()
	// already gets dt handed to it directly, no need for GameTimer's own separate wall clock).
	// Once it elapses, pops twice off SceneManager's stack (this scene, then GameplayScene under
	// it) to land back on StartMenuScene -- same two PopScene() calls HandleInput's "press any
	// key" path already does, just fired on a timer instead of/in addition to input. No re-entry
	// guard needed: once those pops fire, this GameOverScene instance is destroyed along with its
	// slot in the scene stack, so Update() can never be called on it again afterward.
	float autoReturnTimer = 0.0f;
	const float autoReturnDelay = 5.0f;
};