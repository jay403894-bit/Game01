#include "pch.h"
#include "StartMenuScene.h"
#include <Helpers.h>
#include <cmath>
#include <SceneManager.h>
#include "GameplayScene.h"
#include "EditorScene.h"
#include <memory>
namespace {
const std::string kTitle = "Platformer Demo"; // shared by Update() (rerolls letterColors) and Draw()
}

StartMenuScene::StartMenuScene(JLib::Font* font, bool& imguiEnabled, JLib::SoundManager* sound, JLib::Renderer2D& renderer, std::shared_ptr<JLib::InputManager> input, JLib::Camera2D& camera,
	JLib::Mesh* quadMesh, JLib::Mesh* slopeUpRightMesh, JLib::Mesh* slopeUpLeftMesh,
	JLib::TextureHandle tileTexture, uint32_t dustEffect, uint32_t width, uint32_t height, HWND windowHandle)
	: font(font)
	, imguiEnabled(imguiEnabled)
	, sound(sound)
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
	, windowHandle(windowHandle)
{
	music = sound->PlayLoop(JLib::ExeRelativeA("sound\\bounce_light_3.flac").c_str());
	if (!music.IsValid()) {
		throw("PlayLoop(\"bounce_light_3.flac\") failed to load -- put a real file next to the exe to test.\n");
	}
}

void StartMenuScene::Update(bool& isRunning, float dt)
{
	// Self-healing restart: this scene never gets reconstructed when the player returns to it
	// (it just sits underneath GameplayScene/GameOverScene on SceneManager's stack the whole
	// time, alive but not updating), so its constructor-time PlayLoop() only ever runs once per
	// app launch. `music` gets explicitly Stop()'d before pushing GameplayScene (see HandleInput),
	// and GameplayScene's own level music is stopped on Game Over -- so by the time control comes
	// back here (via either PopScene() path), nothing is playing. Checking IsPlaying() every
	// Update() and restarting on the (rare -- once per return trip) frame it's found stopped
	// reloads/restarts the title music without needing GameOverScene or anything else upstream to
	// know this scene's music path or handle at all.
	if (!sound->IsPlaying(music)) {
		music = sound->PlayLoop(JLib::ExeRelativeA("sound\\bounce_light_3.flac").c_str());
	}

	if (scale > 2.0f) {
		scale -= 50 * dt;
	}
	else {
		// Swing between -90 and +90 degrees. rotationDir tracks the current swing direction
		// explicitly and flips it at each bound, clamping to the bound exactly (rather than
		// whatever value one last += step happened to land on) so it can't overshoot past +-90.
		rotation += rotationDir * 90.0f * dt;
		if (rotation <= -90.0f) {
			rotation = -90.0f;
			rotationDir = 1.0f;
		} else if (rotation >= 90.0f) {
			rotation = 90.0f;
			rotationDir = -1.0f;
		}
	}

	// Reroll the title's per-letter colors on a timer, not every Draw() call -- see letterColors'
	// comment in StartMenuScene.h. First call also sizes/fills the vector (starts empty).
	if (letterColors.size() != kTitle.size()) letterColors.resize(kTitle.size());
	colorTimer += dt;
	if (colorTimer >= colorChangeInterval) {
		colorTimer -= colorChangeInterval; // subtract, not reset to 0 -- keeps the average interval
		                                    // correct even if a frame stalls past one interval
		for (auto& c : letterColors) c = JLib::Colors::GetRandomColor();
	}
	if (!cmdQ.empty() && cmdQ.back() == CMD::QUIT) {
		cmdQ.pop_back();
		isRunning = false;
	}
}

void StartMenuScene::Draw()
{
	auto screenSize = renderer.GetScreenSize();

	// Per-letter rainbow title. Each SubmitText call before this used TextAlign::Center at the
	// SAME x -- Center re-centers whatever string it's given around that x, so every single-
	// character call centered itself right on top of the last one instead of advancing. The fix
	// isn't per-letter position DATA (there isn't any to look up) -- it's walking an X cursor
	// forward by each letter's own measured width (Font::TextWidth) and submitting with
	// TextAlign::Left, which draws starting AT x instead of centering on it. Kerning between
	// adjacent letters is lost this way (SubmitText only kerns pairs it's given together in one
	// call), but that's an acceptable tradeoff for being able to color each glyph independently.
	float totalWidth = font->TextWidth(kTitle, scale);
	float x = screenSize.x / 2.0f - totalWidth / 2.0f;

	// Colors come from letterColors, rerolled on a timer in Update() -- NOT GetRandomColor()
	// called here, which reran every Draw() call (60+ times/sec) and looked like flicker/noise
	// instead of a color effect.
	for (size_t i = 0; i < kTitle.size(); ++i) {
		std::string ch(1, kTitle[i]);
		DirectX::XMFLOAT4 color = (kTitle[i] == ' ') ? JLib::Colors::Black : letterColors[i];
		renderer.SubmitText(*font, x, 10.0f, ch, scale, color, rotation, JLib::TextAlign::Left, 2);
		x += font->TextWidth(ch, scale);
	}

	JLib::BatchItem newGameButton;
	JLib::BatchItem quitButton;
	JLib::BatchItem editorButton;
	JLib::BatchItem titleBG;

	titleBG.tex = tileTexture;
	titleBG.color = JLib::Colors::Black;
	titleBG.size = { screenSize.x, 100.0f };
	titleBG.mesh = quadMesh;
	titleBG.position = { screenSize.x / 2.0f, 50.0f };
	renderer.Submit(titleBG);
	newGameButton.tex = tileTexture;

	newGameButton.position = { screenSize.x / 2.0f, screenSize.y / 2.0f - 50.0f };
	newGameButton.size = { 300.0f, 50.0f };
	newGameButton.mesh = quadMesh;
	newGameRect = { newGameButton.position, newGameButton.size };
	renderer.SubmitText(*font, screenSize.x / 2.0f - 115, screenSize.y / 2.0f - 65.0f, "New Game");
	// GetCursorPos+ScreenToClient = cursor in this window's CLIENT space, matching the convention
	// newGameRect/Renderer2D::GetScreenSize() use. (Historical note: this replaced GameInput's
	// absolutePositionX/Y, which wasn't desktop pixels at all. The Raw Input InputManager's
	// GetMousePos() now performs this exact sequence, so it's equivalent to the explicit form.)
	POINT clientMouse;
	GetCursorPos(&clientMouse);
	ScreenToClient(windowHandle, &clientMouse);
	DirectX::XMFLOAT2 mousePos{ (float)clientMouse.x, (float)clientMouse.y };

	if (newGameRect.Contains(mousePos) || selected == 0) {
		newGameButton.color = JLib::Colors::DarkGray;
		selected = 0;
	}
	else
		newGameButton.color = JLib::Colors::Gray;

	quitButton.tex = tileTexture;
	quitButton.position = { screenSize.x / 2.0f, screenSize.y / 2.0f + 150.0f };
	quitButton.size = { 300.0f, 50.0f };
	quitButton.mesh = quadMesh;
	quitRect = { quitButton.position, quitButton.size };
	renderer.SubmitText(*font, screenSize.x / 2.0f-60.0f, screenSize.y / 2.0f + 135.0f, "Quit");

	if (quitRect.Contains(mousePos) || selected == 2) {
		quitButton.color = JLib::Colors::DarkGray;
		selected = 2;
	}
	else
		quitButton.color = JLib::Colors::Gray;
	editorButton.tex = tileTexture;
	editorButton.position = { screenSize.x / 2.0f, screenSize.y / 2.0f + 50.0f };
	editorButton.size = { 300.0f, 50.0f };
	editorButton.mesh = quadMesh;
	editorRect = { editorButton.position, editorButton.size };
	renderer.SubmitText(*font, screenSize.x / 2.0f - 80.0f, screenSize.y / 2.0f + 35.0f, "Editor");
	if (editorRect.Contains(mousePos) || selected == 1) {
		editorButton.color = JLib::Colors::DarkGray;
		selected = 1;
	}
	else
		editorButton.color = JLib::Colors::Gray;

	renderer.Submit(editorButton);
	renderer.Submit(newGameButton);
	renderer.Submit(quitButton);
}

void StartMenuScene::HandleInput(float dt)
{
	// Gamepad navigation alongside keyboard/mouse. The analog stick needs an EDGE, not a level:
	// IsButtonPressed only exists for buttons, so the stick gets a manual "was it centered last
	// frame" latch -- otherwise one flick scrolls the whole menu at frame rate.
	using IM = JLib::InputManager;
	const bool padOn = input->IsGamepadConnected(0);
	const float ly = padOn ? input->GetLeftStickY() : 0.0f;
	const bool stickUp    = ly >  0.5f && !padStickHeld;
	const bool stickDown  = ly < -0.5f && !padStickHeld;
	padStickHeld = std::fabs(ly) > 0.5f;

	const bool navUp   = input->IsKeyPressed(VK_UP)   || input->IsKeyPressed('W') ||
	                     (padOn && (input->IsButtonPressed(IM::GamepadDPadUp)   || stickUp));
	const bool navDown = input->IsKeyPressed(VK_DOWN) || input->IsKeyPressed('S') ||
	                     (padOn && (input->IsButtonPressed(IM::GamepadDPadDown) || stickDown));
	const bool confirm = input->IsKeyPressed(VK_RETURN) || input->IsKeyPressed(VK_SPACE) ||
	                     (padOn && (input->IsButtonPressed(IM::GamepadA) ||
	                                input->IsButtonPressed(IM::GamepadMenu)));

	if (navUp) {
		if (selected == 0)
			selected = 2;
		else
			selected--;
	}
	else if (navDown) {
		if (selected == 2)
			selected = 0;
		else
			selected++;
	}
	if (confirm) {
		if (selected == 0) {
			// Start new game
			sound->Stop(music);
			SceneManager::PushScene(std::make_unique<GameplayScene>(font, sound, renderer, input, camera, quadMesh, slopeUpRightMesh, slopeUpLeftMesh, tileTexture, dustEffect, width, height));
		}
		else if (selected == 1) {
			// Open editor
			imguiEnabled = true;
			// Editor button clicked -- push EditorScene (not implemented in this snippet)
			// SceneManager::PushScene(std::make_unique<EditorScene>(...));
		} 
		else if (selected == 2) {
			// Quit game
			cmdQ.push_back(CMD::QUIT);
		}
	}
	if (input->IsMouseButtonPressed(JLib::InputManager::MouseLeftButton)) {
		// GetCursorPos+ScreenToClient predates the input rewrite (GameInput's "absolute position"
		// wasn't desktop pixels). The Raw Input InputManager's GetMousePos() now does EXACTLY this
		// internally, so either spelling is correct -- keeping the explicit form since it also
		// documents the client-space convention newGameRect/quitRect use.
		POINT clientMouse;
		GetCursorPos(&clientMouse);
		ScreenToClient(windowHandle, &clientMouse);
		DirectX::XMFLOAT2 mousePos{ (float)clientMouse.x, (float)clientMouse.y };

		if (newGameRect.Contains(mousePos)) {
			sound->Stop(music);
			SceneManager::PushScene(std::make_unique<GameplayScene>(font, sound, renderer, input, camera, quadMesh, slopeUpRightMesh, slopeUpLeftMesh, tileTexture, dustEffect, width, height));
		}
		else if (quitRect.Contains(mousePos)) {
			cmdQ.push_back(CMD::QUIT);
		}
		else if (editorRect.Contains(mousePos))	 {
			imguiEnabled = true;
			SceneManager::PushScene(std::make_unique<EditorScene>(font, imguiEnabled, renderer, input, camera, quadMesh, slopeUpRightMesh, slopeUpLeftMesh, tileTexture, width, height, this->windowHandle));

			// Editor button clicked -- push EditorScene (not implemented in this snippet)
			// SceneManager::PushScene(std::make_unique<EditorScene>(...));
		}
	}
}

