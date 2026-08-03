#include "pch.h"
#include "GameOverScene.h"
#include <Helpers.h>

GameOverScene::GameOverScene(JLib::Renderer2D& renderer, std::shared_ptr<JLib::InputManager> input, JLib::Font* font, JLib::SoundManager* soundManager, JLib::Mesh* quadMesh, JLib::TextureHandle tileTexture)
	: renderer(renderer), input(input), font(font), soundManager(soundManager), quadMesh(quadMesh), tileTexture(tileTexture)
{
	// ExeRelativeA, not a bare relative path -- a bare "sound\\..." resolves against the CURRENT
	// WORKING DIRECTORY, which differs from the exe's own directory when launching via the VS
	// debugger. PlaySound() just returns an invalid handle and silently does nothing on a bad
	// path (no exception), which is why this never appeared to play at all.
	soundManager->PlaySound(JLib::ExeRelativeA("sound\\game_over_bad_chest.wav").c_str(), 0.5f);
}

void GameOverScene::Update(bool& isRunning, float dt)
{
	auto screenSize = renderer.GetScreenSize();
	autoReturnTimer += dt;

	if (autoReturnTimer >= autoReturnDelay) {
		// Pop this scene, then GameplayScene under it, landing back on StartMenuScene -- see
		// autoReturnTimer's comment in GameOverScene.h. Return immediately after: `this` (and
		// every member on it, including autoReturnTimer itself) is destroyed the moment the
		// second PopScene() drops the stack's unique_ptr to this instance.
		SceneManager::PopScene();
		SceneManager::PopScene();
		return;
	}
}

void GameOverScene::Draw()
{
	auto screenSize = renderer.GetScreenSize();
	JLib::BatchItem backgroundItem;
	backgroundItem.mesh = quadMesh;
	backgroundItem.tex = tileTexture;
	backgroundItem.color = JLib::Colors::Black;
	backgroundItem.position = { screenSize.x / 2.0f, screenSize.y / 2.0f};
	backgroundItem.size = { screenSize.x, screenSize.y };
	renderer.Submit(backgroundItem);
	renderer.SubmitText(*font, screenSize.x / 2.0f, screenSize.y / 2.0f - 50, "Game Over", 2.0f, JLib::Colors::Red, 0.0f, JLib::TextAlign::Center);
	// SubmitText's signature is (font, x, y, fmt, scale, color, rotation, align, zLayer, ...args)
	// -- the {} format arg comes AFTER zLayer, not right after the format string. Without zLayer
	// passed explicitly here, the countdown float below was landing in the zLayer INT slot instead
	// (silently truncated -- see the C4244 warning that was already flagging this), leaving zero
	// actual args for std::vformat to fill the "{}" placeholder with, which throws
	// _Throw_format_error (out-of-range format argument) the moment this line ever ran.
	renderer.SubmitText(*font, screenSize.x / 2.0f, screenSize.y / 2.0f + 50.0f, "Returning to Title Screen in {:.0f} seconds\nOr Press Any Key", 1.0f, JLib::Colors::Red, 0.0f, JLib::TextAlign::Center, 2, autoReturnDelay - autoReturnTimer);
}

void GameOverScene::HandleInput(float dt)
{
	// GetAnyKeyPressed leaves keyPressed UNTOUCHED (not zeroed) when it returns false -- checking
	// keyPressed's value instead of the function's actual bool return meant an uninitialized,
	// essentially-random stack value was being read on every frame nothing was pressed, which
	// just happened to be nonzero almost immediately -- the scene popped itself on its very first
	// HandleInput call, before a single frame of it ever rendered.
	uint8_t keyPressed = 0;
	if (input->GetAnyKeyPressed(keyPressed))
	{
		SceneManager::PopScene();
		SceneManager::PopScene();
	}
}
