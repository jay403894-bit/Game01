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
#include "Scene.h"
#include "Rect.h"
class StartMenuScene : public Scene
{
public:
	// font/sound are raw, non-owning pointers -- owned once by main.cpp (outlives the whole
	// SceneManager stack), not loaded/initialized separately per scene. See main.cpp's Font
	// comment for why a raw pointer is safe here.
	StartMenuScene(JLib::Font* font, bool& imguiEnabled, JLib::SoundManager* sound, JLib::Renderer2D& renderer, std::shared_ptr<JLib::InputManager> input, JLib::Camera2D& camera,
		JLib::Mesh* quadMesh, JLib::Mesh* slopeUpRightMesh, JLib::Mesh* slopeUpLeftMesh,
		JLib::TextureHandle tileTexture, uint32_t dustEffect, uint32_t width, uint32_t height, HWND windowHandle);

	void Update(bool& isRunning, float dt = 0.0f) override;
	void Draw() override;
	void HandleInput(float dt) override;
	~StartMenuScene() override = default;
private:
	enum class CMD { QUIT };
	JLib::SoundHandle music;
	float rotation = 0.0f;
	float rotationDir = -1.0f; // which way rotation is currently swinging -- flips at each bound in Update()
	float scale = 50.0f;
	// Per-letter title colors, rerolled on a timer instead of every Draw() call -- GetRandomColor()
	// called directly in Draw() rerolled 60+ times a second (once per rendered frame), which reads
	// as flicker/noise rather than a color effect. Rerolling here in Update() on colorChangeInterval
	// and just reading the cached result in Draw() decouples "how often colors change" from
	// framerate. Sized/filled lazily on first Update() call once the title string's length is known.
	std::vector<DirectX::XMFLOAT4> letterColors;
	float colorTimer = 0.0f;
	bool& imguiEnabled;
	const float colorChangeInterval = 0.3f;
	JLib::Rect newGameRect;
	JLib::Rect quitRect;
	JLib::Rect editorRect;
	JLib::SoundManager* sound;
	uint8_t selected = 0; // 0 = new game, 1 = quit
	// Analog sticks report a LEVEL, not an edge, so menu navigation latches: true while the stick is
	// pushed past the threshold, so one flick moves the cursor exactly one row.
	bool padStickHeld = false;
	JLib::Font* font;
	std::vector<CMD> cmdQ;
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
	// Needed only to convert InputManager::GetMousePos() (virtual-DESKTOP screen coordinates --
	// see its own comment in InputManager.h) into this window's CLIENT coordinates via
	// ScreenToClient before comparing against newGameRect/quitRect, which are built in the same
	// client-space convention Renderer2D::Submit()/GetScreenSize() use. Without this conversion,
	// the two Rects are being hit-tested against a coordinate space they were never built in --
	// works out by coincidence only if the window sits at the desktop origin.
	HWND windowHandle;
};