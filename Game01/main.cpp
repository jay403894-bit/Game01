#include "pch.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>   // CommandLineToArgvW
#include <cstdint>
#include <cstdio>
#include <cwchar>       // wcscmp, wcstol
#include <TaskDAG.h>
#include <Window.h>
#include <Renderer2D.h>
#include <Helpers.h>
#include <Font.h>
#include <Camera2D.h>
#include <InputManager.h>
#include <PhysicsWorld.h>
#include <SpatialGrid.h>
#include <PhysicsSystem.h>
#include <memory>
#include "SceneManager.h"
#include "GameplayScene.h"
#include "StartMenuScene.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx12.h"
//#include <InputManager.h>
// -w/--width, -h/--height, -warp/--warp. Parsed here (an app concern), then handed
// to the Window (size) and Renderer (warp adapter).
using namespace JLib;
using namespace PlatformerPhysics2D;

static void ParseCommandLine(uint32_t& width, uint32_t& height, bool& useWarp)
{
	int argc = 0;
	wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (!argv) return;

	for (int i = 0; i < argc; ++i)
	{
		if ((wcscmp(argv[i], L"-w") == 0 || wcscmp(argv[i], L"--width") == 0) && i + 1 < argc)
			width = wcstol(argv[++i], nullptr, 10);
		else if ((wcscmp(argv[i], L"-h") == 0 || wcscmp(argv[i], L"--height") == 0) && i + 1 < argc)
			height = wcstol(argv[++i], nullptr, 10);
		else if (wcscmp(argv[i], L"-warp") == 0 || wcscmp(argv[i], L"--warp") == 0)
			useWarp = true;
	}
	LocalFree(argv);
}

// The whole program now reads as the lifecycle of two objects:
//   Window   -- owns the OS window + message pump (+ fullscreen/keys)
//   Renderer -- owns every GPU object + the render/resize/cleanup logic
int CALLBACK wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
	JLib::TaskScheduler::Init();
	JLib::TaskScheduler& scheduler = JLib::TaskScheduler::Instance();

	// 100% client-area scaling, DPI-aware non-client chrome.
	SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	// WIC (used by DirectXTex's LoadFromWICFile to load textures) is COM-based and
	// requires a COM apartment on this thread; without this the texture load fails with
	// CO_E_NOTINITIALIZED. Initialize once for the app, uninitialize on exit.
	if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
		return -1;

	uint32_t width = 1280, height = 720;
	bool useWarp = false;
	ParseCommandLine(width, height, useWarp);

	Window   window(hInstance, L"DirectX 12", width, height);
	// RendererCore: device/swap chain/fences/PRE-POST lists/particle system -- the low-level
	// GPU guts, reusable by a future 3D renderer. Renderer2D: sprite/text batching, driven BY
	// core's BeginFrame/PresentFrame via SetRenderer2D(). See both classes' header comments.
	RendererCore core;
	Renderer2D   renderer;
	core.SetRenderer2D(&renderer);
	window.SetRenderer(&core);                           // so WM_SIZE reaches Core (owns Resize/VSync)
	core.SetVSync(true); // disable vsync by
	core.Initialize(window.GetHandle(), width, height, useWarp);
	renderer.Initialize(core); // builds root sig/PSO/instance buffer/SRV heap/font against core's device

	core.InitImGui(window.GetHandle()); // ImGui_ImplDX12_Init needs a real window (window.GetHandle()) to exist first
	// Register particle effects AFTER both Initialize() calls (needs core's m_ComputeRootSignature/
	// m_CommandQueue), same pattern as RegisterEffect() for sprite shaders. Each gets its OWN
	// compute shader -- see UpdateParticles.hlsl (straight-line motion) and WaveParticles.hlsl
	// (sin-driven wave) -- and its own particle pool, so they can never step on each other's slots.
	// zLayer=1 here: draws over anything at zLayer 0 (e.g. background tiles) and under zLayer 2+
	// (e.g. the FPS/text overlay) -- see RendererCore::PresentFrame's layer-interleaving comment.
	uint32_t linearEffect = core.RegisterParticleEffect(L"shaders\\UpdateParticles.cso", 256000, false, 2,
		{}, 1, 1, { 1.0f, 0.1f, 0.1f, 0.0f });
	uint32_t waveEffect = core.RegisterParticleEffect(L"shaders\\WaveParticles.cso", 50000, false, 2,
		{}, 1, 1, { 0.2f, 0.6f, 1.0f, 0.0f });
	// Same straight-line-motion shader as linearEffect, but its own pool (own dead-list/spawn
	// buffer) and a real gravityScale -- landing dust is purely visual (never touches PhysicsWorld,
	// never costs a ParticleTable slot or CPU-side UpdateWorldParallel time), so GameplayScene
	// gets this ID passed down instead of allocating CPU particle rows for it.
	uint32_t dustEffect = core.RegisterParticleEffect(L"shaders\\UpdateParticles.cso", 4096, false, 2,
		{}, 1, 1, { 0.6f, 0.6f, 0.6f, 0.0f }, 300.0f);

	std::shared_ptr<InputManager> input;
	input = std::make_shared<InputManager>();
	// Raw Input registers against the real window (it must exist by now) and arrives as WM_INPUT;
	// the window procedure forwards those packets to the InputManager. Zero input threads.
	input->Initialize(window.GetHandle());
	Window::SetRawInputHandler(
		[](void* user, LPARAM lp) { static_cast<InputManager*>(user)->OnRawInput(lp); }, input.get());
	window.Show();
	Vertex quadVertices[] = {
		{ -0.5f,  0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.0f, 0.0f }, // Top-Left
		{  0.5f,  0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.0f, 0.0f }, // Top-Right
		{ -0.5f, -0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.0f, 1.0f }, // Bottom-Left
		{  0.5f, -0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.0f, 1.0f }  // Bottom-Right
	};
	uint32_t quadIndices[] = { 0, 1, 2, 1, 3, 2 }; // The two-triangle quad
	// Slope wedges -- a right triangle filling the tile's diagonal, high side matching
	// SlopeType (see PlatformerPhysics2D's GetSlopeSurfaceY: UP_RIGHT is low-left/high-right,
	// UP_LEFT is high-left/low-right). Bottom-left+bottom-right are shared; only the
	// included top corner differs, same as the original raylib project's DrawTriangle calls.
	Vertex slopeUpRightVertices[] = {
		{ -0.5f, -0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.0f, 1.0f }, // Bottom-Left
		{  0.5f, -0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.0f, 1.0f }, // Bottom-Right
		{  0.5f,  0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.0f, 0.0f }  // Top-Right
	};
	Vertex slopeUpLeftVertices[] = {
		{ -0.5f, -0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.0f, 1.0f }, // Bottom-Left
		{  0.5f, -0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.0f, 1.0f }, // Bottom-Right
		{ -0.5f,  0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.0f, 0.0f }  // Top-Left
	};
	uint32_t slopeIndices[] = { 0, 1, 2 };
	auto resourceManager = renderer.GetResourceManager();
	auto cmdList = renderer.GetCommandList();
	// Flat white 1x1 texture for GameplayScene's tiles -- Resolve() intentionally throws on an
	// invalid/default-constructed TextureHandle (see ResourceManager.h's "fail loudly" comment,
	// not a bug), so tiles need a REAL handle; a solid white texture lets item.color do 100% of
	// the visual work with no wood/stone pattern showing through the tint.
	TextureHandle tileTexture;
	renderer.ExecuteUploadCommand([&](ID3D12GraphicsCommandList* cmd) {
		tileTexture = resourceManager->CreateSolidColorTexture(cmd, 255, 255, 255, 255);
		});

	// Meshes MUST be their own persistent variables, not stored by value inside a SpriteBatchItem --
	// Submit() caches a pointer to whatever Mesh an item points at, permanently, across every
	// future frame (see SpriteBatchItem::mesh's comment), so the Mesh needs to outlive the item.
	Mesh quadMesh = resourceManager->CreateMesh(quadVertices, 4, quadIndices, 6);
	Mesh slopeUpRightMesh = resourceManager->CreateMesh(slopeUpRightVertices, 3, slopeIndices, 3);
	Mesh slopeUpLeftMesh = resourceManager->CreateMesh(slopeUpLeftVertices, 3, slopeIndices, 3);
	DirectX::XMFLOAT4 clearColor = Colors::SkyBlue; // Define your background -- same
	                                                            // values as before, just XMFLOAT4
	                                                            // now so it's consistent with
	                                                            // Colors.h/BatchItem::color instead
	                                                            // of a raw float[4].
	Camera2D camera;
	camera.position = { 1280 / 2,720 / 2 }; // recentered onto the player once GameplayScene::Update runs
	bool isRunning = true;
	SoundManager sound;
	if (!sound.Initialize()) {
		throw("failed to initialize sound on thread 1");
	}
	// Single Font instance for the whole app, owned here -- every scene (StartMenuScene,
	// GameplayScene, GameOverScene) takes it as a raw, non-owning Font* instead of each loading
	// its own copy. Safe for every scene to hold a raw pointer to: this outlives the entire
	// SceneManager stack (it's a wWinMain local, same lifetime as `sound`/`camera`/the meshes),
	// so no scene can ever end up with a dangling pointer to it.
	Font font;
	bool imGuiEnabled = false;
	font.Load(ExeRelative(L"fonts\\PixelOperator8.fnt"), ExeRelative(L"fonts\\PixelOperator8.png"), renderer);
	SceneManager::PushScene(std::make_unique<StartMenuScene>(&font, imGuiEnabled, &sound, renderer, input, camera, &quadMesh, &slopeUpRightMesh, &slopeUpLeftMesh, tileTexture, dustEffect, width, height, window.GetHandle()));
	while (window.ProcessMessages() && isRunning)
	{
		input->Update();
		// Horizontal move intent + jump are read here and consumed by the physics Update task
		// below (added to the per-frame DAG) -- gravity/collision/landing all come from
		// PhysicsSystem::ApplyPhysics now, not raw per-frame position deltas.
		if(imGuiEnabled) {
			ImGui_ImplDX12_NewFrame();
			ImGui_ImplWin32_NewFrame();
			ImGui::NewFrame();
		}
	//	ImGui::Begin("Test");
	//	ImGui::Text("If you see this, ImGui is working!");
	//	ImGui::End();
		// Finalizes this frame's draw data on the raw main-thread stack. PresentFrame picks it
		// up via ImGui::GetDrawData() and records it into the POST list -- no ImGui call ever
		// needs to run inside a task.
		//ImGui::Render();
		float dt = renderer.GetFrameTime();

		renderer.UpdateGlobalUniforms(renderer.GetScreenSize(), camera.position, camera.zoom);

		// Per-frame DAG: StartFrame -> {sprites, text} (main-affinity, parallel-in-principle
		// but both actually run on main, serially, via ProcessMainThread) -> PresentFrame.
		// EVERY Submit-producer node MUST be added as a dependency of presentNode below, or
		// PresentFrame's FlushBatchParallel merge can race with a producer still submitting.
		JLib::TaskDAG dag(scheduler);

		core.m_StartFrameCtx = { &core, clearColor };
		auto* startTask = scheduler.CreateTask(RendererCore::StartFrameTask, &core.m_StartFrameCtx,false);
		auto* startNode = dag.CreateMainNode(startTask);

		// Physics step: SceneManager delegates to the current scene (GameplayScene), which owns
		// its own PhysicsWorld/SpatialGrid/player entirely -- must run before spritesNode reads
		// anything Draw() submits.
		auto* updateTask = scheduler.CreateTask([&isRunning, dt] {
			try {
				SceneManager::HandleInput(dt);
				SceneManager::Update(isRunning, dt);
			} catch (const std::exception& e) {
				FILE* f = nullptr;
				fopen_s(&f, "crash.log", "a");
				if (f) { fprintf(f, "std::exception in Update: %s\n", e.what()); fclose(f); }
				isRunning = false;
			}
			});
		auto* updateNode = dag.CreateMainNode(updateTask);
		dag.AddDependency(updateNode, startNode);

		auto* spritesTask = scheduler.CreateTask([&renderer, linearEffect, waveEffect,imGuiEnabled] {
			// The map/player/physics world itself -- GameplayScene::Draw() submits one BatchItem
			// per active PhysicsWorld cell.
			SceneManager::Draw();
			if(imGuiEnabled)
				ImGui::Render();
			// effectID, basePosition, offsetRadius (jitter so multiple spawns scatter instead of
			// stacking), velocity, lifetime, color.
		//	renderer.RequestSpawn(linearEffect, { 300,300 }, 20.0f, { 0.0f, -50.0f }, 5.0f, { 1.0f, 0.6f, 0.1f, 1.0f });
			// velocity.y is repurposed as wave AMPLITUDE by WaveParticles.hlsl, not a real
			// vertical speed -- see that file's comment.
		//	renderer.RequestSpawn(waveEffect, { 0,0 }, 20.0f, { 30.0f, 60.0f }, 5.0f, { 0.2f, 0.6f, 1.0f, 1.0f });
			});
		auto* spritesNode = dag.CreateMainNode(spritesTask);
		dag.AddDependency(spritesNode, startNode);
		dag.AddDependency(spritesNode, updateNode);

		auto* textTask = scheduler.CreateTask([&renderer] {
			Font* f = renderer.GetSysFont();
		//	renderer.SubmitText(*f,  400.0f, 150.0f, "Left Aligned\ntest", 1.0f, {1,1,1,1}, 0.0f, TextAlign::Left, 0);
		//	renderer.SubmitText(*f,  400.0f, 250.0f, "Center Aligned", 1.0f, {1,1,1,1}, 0.0f, TextAlign::Center, 0);
		//	renderer.SubmitText(*f,  400.0f, 350.0f, "Right Aligned\ntest", 1.0f, {1,1,1,1}, 0.0f, TextAlign::Right, 0);
			renderer.UpdateFPS(); // its own SubmitText call -- same tier, same rules
			});
		auto* textNode = dag.CreateMainNode(textTask);
		dag.AddDependency(textNode, startNode);

		core.m_UpdateParticlesCtx = { &core };
		auto* particlesTask = scheduler.CreateTask(RendererCore::UpdateParticlesTask, &core.m_UpdateParticlesCtx);
		auto* particlesNode = dag.CreateMainNode(particlesTask);
		dag.AddDependency(particlesNode, startNode);

		core.m_PresentFrameCtx = { &core };
		auto* presentTask = scheduler.CreateTask(RendererCore::PresentFrameTask, &core.m_PresentFrameCtx);
		auto* presentNode = dag.CreateMainNode(presentTask);
		dag.AddDependency(presentNode, spritesNode);
		dag.AddDependency(presentNode, textNode);
		dag.AddDependency(presentNode, particlesNode);

		JLib::WaitGroup frameWg;
		frameWg.n.store(1, std::memory_order_relaxed); // 1 = the PresentFrame tail node
		presentTask->waitGroup = &frameWg;             // set BEFORE dag.Submit()

		dag.Submit();
		scheduler.WaitForMain(frameWg); // pumps ProcessMainThread; frame N+1 can't start until this returns

	}                               // continuous render loop
	// MUST be explicit, here, before renderer (Renderer2D) goes out of scope -- local
	// destruction order is REVERSE of declaration order, so renderer destructs BEFORE core
	// does. Relying on ~RendererCore() alone means Cleanup()'s GPU-idle Flush() would run
	// AFTER Renderer2D's command lists/allocators/instance buffer are already released,
	// letting the GPU still be mid-flight on resources that just got torn out from under it --
	// that's the shutdown freeze (driver doing extra recovery work), not a coincidence.
	core.Cleanup();
	// Detach the WM_INPUT forwarding before the InputManager shuts down (the window can outlive
	// it by a few lines). Raw Input has no COM to race CoUninitialize -- the old GameInput
	// shutdown-ordering hazard is gone -- but explicit teardown before the pump dies stays correct.
	Window::SetRawInputHandler(nullptr, nullptr);
	input->Shutdown();
	CoUninitialize();
	return 0;
}
