#pragma once
#include <vector>
#include <memory>
#include <DirectXMath.h>
#include <Renderer2D.h>
#include <Camera2D.h>
#include <InputManager.h>
#include <SoundManager.h>
#include <Font.h>
#include <imgui/imgui.h>
#include <WRL.h>
#include "Scene.h"
#include "Rect.h"
#include "BinaryMapFormat.h"
#include <GridSpriteSheet.h>
// Milestone 1: a tile-behavior painter over a BinaryLevel document. The editor edits ONLY
// the BinaryLevel (never a PhysicsWorld -- that's the game's job via LevelInstantiator), so
// this scene deliberately does NOT include PhysicsWorld/PhysicsSystem. Paint a behavior into
// cells with the left mouse, erase with the right, Save writes the binary format the game
// loads. Sprite-sheet painting (real spriteIndex from an image), object placement, pan/zoom,
// and load are later milestones.
class EditorScene : public Scene
{
public:
	// font is a raw, non-owning pointer -- owned once by main.cpp (outlives the whole
	// SceneManager stack). imguiEnabled is main's shared flag: the editor flips it on in the
	// ctor (so main's loop runs ImGui NewFrame/Render) and off in the dtor.
	EditorScene(JLib::Font* font, bool& imguiEnabled, JLib::Renderer2D& renderer, std::shared_ptr<JLib::InputManager> input, JLib::Camera2D& camera,
		JLib::Mesh* quadMesh, JLib::Mesh* slopeUpRightMesh, JLib::Mesh* slopeUpLeftMesh,
		JLib::TextureHandle tileTexture, uint32_t width, uint32_t height, HWND windowHandle);

	void Update(bool& isRunning, float dt = 0.0f) override;
	void Draw() override;
	void HandleInput(float dt) override;
	~EditorScene() override;
private:
	enum class CMD { QUIT };
	// Paint = LMB stamps the brush/sprite (existing behavior). Select = LMB locks a cell as
	// the inspector target without painting it, so you can edit that cell's metadata.
	enum class Mode { Paint, Select };
	Mode m_mode = Mode::Paint;
	int m_selCol = -1, m_selRow = -1; // locked inspector selection (-1 = none)
	// Armed by the platform inspector's "Pick Target B" button: the NEXT grid click writes that
	// cell's center into the SELECTED cell's kTargetBX/Y (instead of moving the selection),
	// then disarms. Escape or right-click cancels. Only meaningful while a platform cell is
	// selected -- cleared whenever the selection changes.
	bool m_pickingTargetB = false;
	bool* imguiEnabled;
	uint16_t m_selectedSpriteIndex = 0;
	std::unique_ptr<JLib::GridSpriteSheet> tileSheet;
	std::unique_ptr<JLib::GridSpriteSheet> secondarySheet;  // Secondary (e.g., Platforms/Items)
	JLib::Font* font;
	std::vector<CMD> cmdQ;
	JLib::Renderer2D& renderer;
	std::shared_ptr<JLib::InputManager> input;
	JLib::Camera2D& camera;
	JLib::Mesh* quadMesh;
	JLib::Mesh* slopeUpRightMesh;
	JLib::Mesh* slopeUpLeftMesh;
	JLib::TextureHandle tileTexture; // solid-white texture; per-cell color tints it (same
	                                 // rendering approach GameplayScene uses for flat tiles)
	uint32_t width, height;          // window client size, for camera WorldToScreen
	// Converts InputManager::GetMousePos() (virtual-DESKTOP coords) into this window's CLIENT
	// coords via ScreenToClient before ScreenToWorld -- see the original skeleton's note.
	HWND windowHandle;

	// ---- editor document + state ----
	BinaryLevel m_level;
	uint8_t m_brush = BEHAVIOR_SOLID; // currently-painted behavior (BinaryBehaviorType)
	bool m_lastMouseInCell = false;   // for a readout; painting itself is per-frame while held
	// True until the first Draw completes. When this scene is pushed mid-run (e.g. from the
	// start menu), the push happens during Update -- AFTER main's loop-top ImGui::NewFrame
	// check already saw imGuiEnabled==false and skipped NewFrame. So on this ONE entry frame,
	// the flag is true (our ctor set it) but no frame was started; calling ImGui here AVs.
	// From the next frame on, loop-top sees the flag and NewFrame runs before Draw. See
	// DrawPanel's guard.
	bool m_firstDraw = true;
	uint8_t m_activeSheetTab = 0;
	char m_mapName[64] = "editor_level"; // Save/Load target -> levels\<name>.tmap
	int m_newW = 60, m_newH = 40;        // Resize inputs (kept in sync with the current level)
	void InitEmptyLevel(int cols, int rows, int tilePx);
	void ResizeLevel(int newW, int newH); // rebuild cells at a new size, preserving the overlap
	void PaintAt(int col, int row);   // stamp m_brush; erase (right-click) passes BEHAVIOR_BACKGROUND+0xFFFF
	void EraseAt(int col, int row);
	bool MouseToCell(int& outCol, int& outRow); // screen mouse -> cell; false if off-map
	DirectX::XMFLOAT4 ColorForBehavior(uint8_t behavior) const;
	void DrawLevel();                 // render background + painted cells
	void DrawSheetGrid(JLib::GridSpriteSheet* sheet, int sheetID);
	void DrawPanels();                 // the ImGui window
	void DrawInspector();             // per-cell metadata editor for the locked selection
	void LoadLevel();                 // BinaryMapFormat::Load into m_level (editor reopen)
};