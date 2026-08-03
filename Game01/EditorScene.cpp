#include "pch.h"
#include <Windows.h>
#include "EditorScene.h"
#include "SceneManager.h"
#include "EnemyDefs.h"
#include <Helpers.h>
#include <string>
#include <InputManager.h>
using namespace DirectX;

// levels\<name>.tmap, anchored to the exe dir (same rule as every other asset path here).
// ASCII map names only -- widen byte-for-byte.
static std::wstring MapPathFor(const char* name) {
	std::string s(name);
	std::wstring w(s.begin(), s.end());
	return JLib::ExeRelative(L"levels\\" + w + L".tmap");
}

EditorScene::EditorScene(JLib::Font* font, bool& imguiEnabled, 
	JLib::Renderer2D& renderer,
	std::shared_ptr<JLib::InputManager> input, JLib::Camera2D& camera, JLib::Mesh* quadMesh,
	JLib::Mesh* slopeUpRightMesh, JLib::Mesh* slopeUpLeftMesh, JLib::TextureHandle tileTexture,
	uint32_t width, uint32_t height, HWND windowHandle) :
	imguiEnabled(&imguiEnabled),
	font(font),
	renderer(renderer),
	input(input),
	camera(camera),
	quadMesh(quadMesh),
	slopeUpRightMesh(slopeUpRightMesh),
	slopeUpLeftMesh(slopeUpLeftMesh),
	tileTexture(tileTexture),
	width(width),
	height(height),
	windowHandle(windowHandle)
{
	imguiEnabled = true; // main's loop now runs ImGui NewFrame/Render (see main.cpp)
	InitEmptyLevel(60, 40, 16);
	JLib::TextureHandle tileTex;
	auto resourceManager = renderer.GetResourceManager();
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
	// world_tileset.png: 256x256 = a 16x16 grid of 16px tiles. (four-seasons-tileset.png was
	// briefly swapped in here but reverted: it's 176x256 = 11 columns, which changes the
	// meaning of every authored spriteIndex (row*cols+col) -- not worth re-authoring the block/
	// potion tables and repainting levels for a few extra sprites. If it comes back, it should
	// arrive as an ADDITIONAL bank via the planned sheet-registry work, not a swap.)
	tileSheet = std::make_unique<JLib::GridSpriteSheet>(tileTex, 16, 16);
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
	secondarySheet = std::make_unique<JLib::GridSpriteSheet>(platformTex, 4, 4);

	// Center the camera on the level so it's visible immediately. camera is shared with
	// main.cpp but nothing else reads it while this scene is on top of the stack.
	camera.position = { m_level.width * m_level.tileWidth * 0.5f,
	                    m_level.height * m_level.tileHeight * 0.5f };
	camera.zoom = 2.0f;
}

void EditorScene::InitEmptyLevel(int cols, int rows, int tilePx)
{
	m_level = BinaryLevel{};
	m_level.width = (uint16_t)cols;
	m_level.height = (uint16_t)rows;
	m_level.tileWidth = (uint16_t)tilePx;
	m_level.tileHeight = (uint16_t)tilePx;
	m_level.cells.resize((size_t)cols * rows); // default BinaryCell: spriteIndex 0xFFFF == empty
	m_newW = cols; m_newH = rows; // keep the Resize inputs reflecting the current size
}

// Rebuild the cell grid at a new size, preserving whatever overlaps the top-left corner.
// New area is empty (0xFFFF); trimmed area is discarded. Cheap -- it's just a copy.
void EditorScene::ResizeLevel(int newW, int newH)
{
	if (newW < 1) newW = 1; else if (newW > 1024) newW = 1024;
	if (newH < 1) newH = 1; else if (newH > 1024) newH = 1024;
	std::vector<BinaryCell> nc((size_t)newW * newH);
	int copyW = (newW < (int)m_level.width)  ? newW : (int)m_level.width;
	int copyH = (newH < (int)m_level.height) ? newH : (int)m_level.height;
	for (int r = 0; r < copyH; ++r)
		for (int c = 0; c < copyW; ++c)
			nc[(size_t)r * newW + c] = m_level.CellAt(c, r);
	m_level.cells = std::move(nc);
	m_level.width = (uint16_t)newW;
	m_level.height = (uint16_t)newH;
	m_newW = newW; m_newH = newH;
	m_selCol = m_selRow = -1; // selection indices are meaningless against the new grid
}

DirectX::XMFLOAT4 EditorScene::ColorForBehavior(uint8_t behavior) const
{
	// Collision tiles keep a debug tint so their shape is readable at a glance; a "Not Solid"
	// decorative tile (BEHAVIOR_BACKGROUND, e.g. a tree/foreground prop) renders at FULL color
	// so it looks like its actual art -- a dark tint there would make decoration nearly invisible.
	switch (behavior) {
	case BEHAVIOR_SOLID:            return { 0.55f, 0.55f, 0.60f, 1.0f }; // gray
	case BEHAVIOR_SLOPE_UP_RIGHT:  return { 0.90f, 0.60f, 0.20f, 1.0f }; // orange
	case BEHAVIOR_SLOPE_UP_LEFT:   return { 0.90f, 0.80f, 0.20f, 1.0f }; // yellow
	case BEHAVIOR_BACKGROUND:      return { 1.0f, 1.0f, 1.0f, 1.0f };     // Not Solid -- true color
	default:                       return { 1.0f, 1.0f, 1.0f, 1.0f };
	}
}

bool EditorScene::MouseToCell(int& outCol, int& outRow)
{
	// ImGui's IO mouse position is already in this window's CLIENT space (the Win32 backend
	// sets it from client-relative coords), so no GetMousePos()/ScreenToClient is needed --
	// and it's the exact same mouse ImGui uses for its own hit-testing, so painting and the
	// panel's cursor readout can never disagree. REQUIRES the window's WndProc to forward
	// messages to ImGui_ImplWin32_WndProcHandler (renderer side); without that, io.MousePos
	// is stale and nothing here (or any ImGui widget) receives input.
	ImVec2 m = ImGui::GetIO().MousePos;
	XMFLOAT2 world = camera.ScreenToWorld({ m.x, m.y }, { (float)width, (float)height });
	int col = (int)std::floor(world.x / m_level.tileWidth);
	int row = (int)std::floor(world.y / m_level.tileHeight);
	outCol = col; outRow = row;
	return col >= 0 && row >= 0 && col < m_level.width && row < m_level.height;
}
/*
void EditorScene::PaintAt(int col, int row)
{
	if (col < 0 || row < 0 || col >= m_level.width || row >= m_level.height) return;
	BinaryCell& c = m_level.CellAt(col, row);
	c.behaviorType = m_brush;
	// spriteIndex must be != 0xFFFF to count as "present" (both DrawLevel and
	// LevelInstantiator treat 0xFFFF as empty). No sheet yet, so the value is a placeholder;
	// stamp the behavior so a saved level still round-trips something meaningful.
	c.spriteIndex = m_brush;
}
*/
void EditorScene::PaintAt(int col, int row)
{
	if (col < 0 || row < 0 || col >= m_level.width || row >= m_level.height) return;
	BinaryCell& c = m_level.CellAt(col, row);

	c.behaviorType = m_brush;
	c.spriteIndex = m_selectedSpriteIndex;
	c.textureSheetID = m_activeSheetTab; // Record exactly which tab was active!
}
void EditorScene::EraseAt(int col, int row)
{
	if (col < 0 || row < 0 || col >= m_level.width || row >= m_level.height) return;
	BinaryCell& c = m_level.CellAt(col, row);
	c.spriteIndex = 0xFFFF;
	c.behaviorType = BEHAVIOR_BACKGROUND;
	c.properties.clear();
}

void EditorScene::HandleInput(float dt)
{
	if (input->IsKeyPressed(VK_ESCAPE)) {
		if (m_pickingTargetB) m_pickingTargetB = false; // Escape cancels an armed pick, not the editor
		else cmdQ.push_back(CMD::QUIT);
	}

	// Mouse painting goes entirely through ImGui's IO (see MouseToCell) -- the editor
	// requires ImGui anyway, and io mouse state is client-space and authoritative once the
	// WndProc forward is in place. HandleInput runs inside the ImGui frame (after main's
	// loop-top NewFrame, before Render), so io here is fresh this frame.
	ImGuiIO& io = ImGui::GetIO();
	if (io.WantCaptureMouse) return; // cursor is over the panel -- ImGui owns it, don't paint

	int col, row;
	m_lastMouseInCell = MouseToCell(col, row);
	if (!m_lastMouseInCell) return;

	// Armed target-B pick: the next grid click is CONSUMED here -- it writes the clicked cell's
	// center (world coords, same convention the loader reads) into the SELECTED platform cell's
	// kTargetBX/Y. It must not fall through to Paint/Select below, or picking would repaint or
	// move the selection out from under the platform being edited. Right-click cancels.
	if (m_pickingTargetB) {
		if (io.MouseClicked[ImGuiMouseButton_Right]) { m_pickingTargetB = false; return; }
		if (io.MouseClicked[ImGuiMouseButton_Left]) {
			if (m_selCol >= 0 && m_selRow >= 0) {
				BinaryCell& sel = m_level.CellAt(m_selCol, m_selRow);
				m_level.SetProperty(sel.properties, BinaryProps::kTargetBX,
					col * (float)m_level.tileWidth + m_level.tileWidth * 0.5f);
				m_level.SetProperty(sel.properties, BinaryProps::kTargetBY,
					row * (float)m_level.tileHeight + m_level.tileHeight * 0.5f);
			}
			m_pickingTargetB = false;
		}
		return;
	}

	if (m_mode == Mode::Paint) {
		// Paint while held (not just on press) so dragging strokes a line of cells.
		if (io.MouseDown[ImGuiMouseButton_Left])  PaintAt(col, row);
		if (io.MouseDown[ImGuiMouseButton_Right]) EraseAt(col, row);
	}
	else { // Select: a single click locks this cell as the inspector target (no painting).
		if (io.MouseClicked[ImGuiMouseButton_Left]) { m_selCol = col; m_selRow = row; }
	}
}

void EditorScene::Update(bool& isRunning, float dt)
{
	if (!cmdQ.empty() && cmdQ.back() == CMD::QUIT) {
		cmdQ.pop_back();
		SceneManager::PopScene();
		return;
	}
	static DirectX::XMFLOAT2 lastMousePos = { 0.0f, 0.0f };
	ImVec2 imguiMousePos = ImGui::GetMousePos();

	DirectX::XMFLOAT2 currentMousePos = { imguiMousePos.x, imguiMousePos.y };

	// Detect when the panning button is held down (Middle Mouse Button is standard for editors)
	if (input->IsMouseButtonDown(JLib::InputManager::MouseMiddleButton))
	{
		// Calculate how many screen pixels the mouse moved since last frame
		float deltaX = currentMousePos.x - lastMousePos.x;
		float deltaY = currentMousePos.y - lastMousePos.y;

		// Convert pixel delta to world units by dividing by the current zoom factor
		// (If zoomed in, mouse drag moves the camera slower; if zoomed out, it moves faster)
		camera.position.x -= deltaX / camera.zoom;
		camera.position.y += deltaY / camera.zoom; // Change to += or -= depending on whether your Y-axis goes up or down
	}

	// Keep track of the mouse position for the next frame's delta check
	lastMousePos = currentMousePos;
	// --- CAMERA ZOOM SYSTEM ---
	if (!ImGui::GetIO().WantCaptureMouse)
	{
		// Simply query your library!
		float scrollDelta = input->GetMouseWheelDelta();

		if (scrollDelta != 0.0f)
		{
			float zoomSpeed = 0.15f * camera.zoom;
			camera.zoom += scrollDelta * zoomSpeed;

			// Clamp boundaries
			if (camera.zoom < 0.1f) camera.zoom = 0.1f;
			if (camera.zoom > 6.0f) camera.zoom = 6.0f;
		}
	}
}

void EditorScene::DrawLevel()
{
	const float tw = (float)m_level.tileWidth;
	const float th = (float)m_level.tileHeight;
	const XMFLOAT2 screen{ (float)width, (float)height };

	// 1. Level-bounds background (Uses fallback tileTexture as a dark backing board)
	{
		JLib::BatchItem bg;
		bg.tex = tileTexture;
		bg.mesh = quadMesh;
		float w = m_level.width * tw, h = m_level.height * th;
		bg.position = camera.WorldToScreen({ w * 0.5f, h * 0.5f }, screen);
		bg.size = { w * camera.zoom, h * camera.zoom };
		bg.color = { 0.12f, 0.12f, 0.15f, 1.0f };
		bg.zLayer = 0;
		renderer.Submit(bg);
	}

	// Selection + hover highlight (translucent quads on zLayer 2, above the tiles). Kept
	// dependency-free on purpose -- a proper gridOutline-effect outline can replace these
	// later by setting item.effectID/effectParams once that effect is registered in Game01.
	auto highlight = [&](int c, int r, XMFLOAT4 color) {
		if (c < 0 || r < 0 || c >= m_level.width || r >= m_level.height) return;
		JLib::BatchItem hi;
		hi.tex = tileTexture;
		hi.mesh = quadMesh;
		hi.position = camera.WorldToScreen({ c * tw + tw * 0.5f, r * th + th * 0.5f }, screen);
		hi.size = { tw * camera.zoom, th * camera.zoom };
		hi.color = color;
		hi.zLayer = 2;
		renderer.Submit(hi);
	};
	if (m_mode == Mode::Select || m_mode == Mode::Paint) {
		int hc, hr;
		if (MouseToCell(hc, hr)) highlight(hc, hr, { 1.0f, 1.0f, 1.0f, 0.15f }); // hovered cell
	}
	highlight(m_selCol, m_selRow, { 0.2f, 0.7f, 1.0f, 0.40f }); // locked selection

	// 2. Painted cells pulling from your actual world_tileset asset
	if (!tileSheet) return;

	JLib::BatchItem item;
	item.zLayer = 1;

	for (int row = 0; row < m_level.height; ++row) {
		for (int col = 0; col < m_level.width; ++col) {
			const BinaryCell& c = m_level.CellAt(col, row);

			// Empty cells with metadata (spawns, coins, player start) get a red indicator
			// Empty cells with metadata (spawns, coins, player start) get a colored indicator
			if (c.spriteIndex == 0xFFFF && !c.properties.empty()) {
				JLib::BatchItem meta;
				meta.tex = tileTexture;
				meta.mesh = quadMesh;
				float cx = col * tw + tw * 0.5f;
				float cy = row * th + th * 0.5f;
				meta.position = camera.WorldToScreen({ cx, cy }, screen);
				meta.size = { 8.0f * camera.zoom, 8.0f * camera.zoom };
				meta.zLayer = 3;

				// Read entity type from properties to color-code the indicator
				float entTypeFloat = m_level.GetProperty(c.properties, BinaryProps::kEntityType, -1.0f);
				int entType = (int)entTypeFloat;

				switch (entType) {
				case OBJ_PLAYER:
					meta.color = { 0.0f, 1.0f, 0.0f, 1.0f }; // green
					break;
				case OBJ_ENEMY_PATROL:
				case OBJ_ENEMY_CHASE:
					meta.color = { 1.0f, 0.0f, 0.0f, 1.0f }; // red
					break;
				case OBJ_COIN:
					meta.color = { 1.0f, 1.0f, 0.0f, 1.0f }; // yellow
					break;
				case OBJ_LEVEL_END:
					meta.color = { 0.0f, 1.0f, 1.0f, 1.0f }; // cyan
					break;
				default:
					meta.color = { 1.0f, 0.0f, 0.0f, 1.0f }; // red (fallback)
					break;
				}

				renderer.Submit(meta);
				continue; // skip rendering the tile itself
			}

			if (c.spriteIndex == 0xFFFF) continue; // empty with no metadata

			switch (c.behaviorType) {
			case BEHAVIOR_SLOPE_UP_RIGHT:  item.mesh = slopeUpRightMesh; break;
			case BEHAVIOR_SLOPE_UP_LEFT:   item.mesh = slopeUpLeftMesh;  break;
			default:                       item.mesh = quadMesh;         break;
			}

			// --- THE MULTI-SHEET DECODER FIX ---
			int spriteIndex = c.spriteIndex;
			JLib::GridSpriteSheet* currentSheet = tileSheet.get();

			// If the index is 256 or higher, it belongs to the secondary sheet!
			if (spriteIndex >= 256) {
				spriteIndex -= 256; // Normalize back down to 0-255 for UV slicing
				if (secondarySheet) {
					currentSheet = secondarySheet.get();
				}
			}

			// Apply the texture and UV scales dynamically for whichever sheet is active
			item.tex = currentSheet->GetTexture();
			item.uvScale = currentSheet->GetUVScale();

			// Unpack 1D tile ID into a 2D sheet row/col (16-wide convention -- correct for
			// world_tileset; the 4-wide platform sheet's %16 is a KNOWN shared quirk, but every
			// authored spriteIndex in every saved level was picked under it, so it must stay
			// until the sheet-registry rework re-encodes deliberately. GetFramesX() exists on
			// GridSpriteSheet when that day comes.)
			int sheetCol = spriteIndex % 16;
			int sheetRow = spriteIndex / 16;
			item.uvOffset = currentSheet->GetUVOffset(sheetCol, sheetRow);
			// ------------------------------------

			float cx = col * tw + tw * 0.5f;
			float cy = row * th + th * 0.5f;
			item.position = camera.WorldToScreen({ cx, cy }, screen);
			item.size = { tw * camera.zoom, th * camera.zoom };

			// Retain behavior color tinting for debugging map collision easily
			item.color = ColorForBehavior(c.behaviorType);
			renderer.Submit(item);
		}
	}
}
void EditorScene::DrawSheetGrid(JLib::GridSpriteSheet* sheet, int sheetID)
{
	ImTextureID imguiTexID = renderer.GetImGuiTextureID(sheet->GetTexture());
	DirectX::XMFLOAT2 scale = sheet->GetUVScale();

	const int totalTiles = 16 * 16;
	const int columnsInGrid = 4;

	if (ImGui::BeginChild("TileScrollRegion", ImVec2(0, -1.0f), true, ImGuiWindowFlags_AlwaysVerticalScrollbar))
	{
		for (int i = 0; i < totalTiles; ++i) {
			int sCol = i % 16;
			int sRow = i / 16;

			DirectX::XMFLOAT2 uv0 = sheet->GetUVOffset(sCol, sRow);
			DirectX::XMFLOAT2 uv1 = { uv0.x + scale.x, uv0.y + scale.y };

			if (i % columnsInGrid != 0) {
				ImGui::SameLine();
			}

			// Combine sheetID and tile index to prevent any internal tracking collisions
			ImGui::PushID((sheetID << 16) | i);

			// Only highlight the button if it matches BOTH the index AND the active tab
			bool isSelected = (m_selectedSpriteIndex == i && m_activeSheetTab == sheetID);
			if (isSelected) {
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.6f, 1.0f, 1.0f));
			}

			if (ImGui::ImageButton("##tile_choice", imguiTexID, ImVec2(32.0f, 32.0f), ImVec2(uv0.x, uv0.y), ImVec2(uv1.x, uv1.y))) {
				m_selectedSpriteIndex = (sheetID == 1) ? (256 + i) : i;
				m_activeSheetTab = sheetID; // Mark which texture sheet is currently active for painting
			}

			if (isSelected) {
				ImGui::PopStyleColor();
			}
			ImGui::PopID();
		}
	}
	// OUTSIDE the if, unconditionally: unlike BeginTabBar/BeginTabItem, EndChild must be called
	// even when BeginChild returned false (clipped/collapsed) -- skipping it unbalances ImGui's
	// window/ID stacks and asserts at frame end ("must call EndChild", "missing End/PopID").
	ImGui::EndChild();
}
void EditorScene::DrawPanels()
{
	// =================================================================
	// PANEL 1: MAIN TOOLS & SETTINGS
	// =================================================================
	ImGui::SetNextWindowSize(ImVec2(260.0f, 340.0f), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Editor Tools"))
	{
		ImGui::Text("Level: %dx%d  cell %dpx", m_level.width, m_level.height, m_level.tileWidth);

		int col, row;
		if (MouseToCell(col, row)) ImGui::Text("Cursor cell: %d, %d", col, row);
		else                       ImGui::Text("Cursor cell: (off map)");

		ImGui::Separator();
		ImGui::Text("Collision (paint = LMB, erase = RMB)");
		int brush = m_brush;
		ImGui::RadioButton("Solid", &brush, BEHAVIOR_SOLID);
		// "Not Solid" = a painted sprite that renders but never collides (trees, foreground
		// props, background decoration). It's BEHAVIOR_BACKGROUND under the hood -- the same
		// value an empty cell would have -- but WITH a real spriteIndex, so it draws. Erasing
		// (RMB) is different: that clears the cell to 0xFFFF (nothing drawn at all).
		ImGui::RadioButton("Not Solid", &brush, BEHAVIOR_BACKGROUND);
		ImGui::RadioButton("Slope UR", &brush, BEHAVIOR_SLOPE_UP_RIGHT);
		ImGui::RadioButton("Slope UL", &brush, BEHAVIOR_SLOPE_UP_LEFT);
		// DOWN slopes intentionally omitted -- not needed for this game (see roadmap); the
		// enum values still exist in the format if they're ever wanted.
		m_brush = (uint8_t)brush;

		ImGui::Separator();
		ImGui::Text("Tool");
		int mode = (int)m_mode;
		ImGui::RadioButton("Paint", &mode, (int)Mode::Paint);  ImGui::SameLine();
		ImGui::RadioButton("Select", &mode, (int)Mode::Select);
		m_mode = (Mode)mode; // Select = click a cell to inspect/edit its metadata (see Inspector)

		ImGui::Separator();
		ImGui::InputText("Map name", m_mapName, sizeof(m_mapName)); // -> levels\<name>.tmap
		if (ImGui::Button("Save")) {
			BinaryMapFormat::Save(MapPathFor(m_mapName), m_level);
		}
		ImGui::SameLine();
		if (ImGui::Button("Load")) {
			LoadLevel();
		}
		ImGui::SameLine();
		if (ImGui::Button("Clear")) {
			InitEmptyLevel(m_level.width, m_level.height, m_level.tileWidth);
			m_selCol = m_selRow = -1;
		}
		ImGui::Separator();
		ImGui::Text("Map size (cells)");
		ImGui::InputInt("W", &m_newW); ImGui::SameLine();
		ImGui::InputInt("H", &m_newH);
		if (ImGui::Button("Resize (keeps top-left)")) ResizeLevel(m_newW, m_newH);

		ImGui::Separator();
		if (ImGui::Button("Exit editor")) cmdQ.push_back(CMD::QUIT);
	}
	ImGui::End(); // Closes "Editor Tools" cleanly

	// =================================================================
	// PANEL 2: MULTI-SHEET TILE PALETTE
	// =================================================================
	ImGui::SetNextWindowSize(ImVec2(220.0f, 400.0f), ImGuiCond_FirstUseEver);

	if (ImGui::Begin("Tile Palette"))
	{
		// Create a tab bar to flip between sheets
		if (ImGui::BeginTabBar("PaletteSheets"))
		{
			// ---- TAB 1: PRIMARY WORLD SHEET ----
			if (ImGui::BeginTabItem("World Tiles"))
			{
				m_activeSheetTab = 0;

				if (tileSheet) {
					DrawSheetGrid(tileSheet.get(), 0); // 0 = Sheet Type Identifier
				}
				else {
					ImGui::Text("No World Sheet Loaded");
				}
				ImGui::EndTabItem();
			}

			// ---- TAB 2: SECONDARY SHEET ----
			if (ImGui::BeginTabItem("Objects / Platforms"))
			{
				m_activeSheetTab = 1;

				if (secondarySheet) {
					DrawSheetGrid(secondarySheet.get(), 1); // 1 = Sheet Type Identifier
				}
				else {
					ImGui::Text("No Secondary Sheet Loaded");
				}
				ImGui::EndTabItem();
			}

			ImGui::EndTabBar();
		}
	}
	ImGui::End();
}

// Per-cell metadata editor for the locked selection. Every field is the same GetProperty/
// SetProperty pattern -- add health, moveSpeed, an entityType combo, etc. by copying the
// friction block. SetProperty registers the name in the level's table, so save/load just works.
void EditorScene::DrawInspector()
{
	ImGui::SetNextWindowSize(ImVec2(240.0f, 240.0f), ImGuiCond_FirstUseEver);
	if (ImGui::Begin("Inspector"))
	{
		bool valid = (m_selCol >= 0 && m_selRow >= 0 &&
			m_selCol < m_level.width && m_selRow < m_level.height);
		if (!valid) {
			ImGui::TextWrapped("No cell selected. Switch to Select mode and click a cell.");
		}
		else {
			BinaryCell& c = m_level.CellAt(m_selCol, m_selRow);
			ImGui::Text("Cell %d, %d", m_selCol, m_selRow);
			ImGui::Text("spriteIndex: %d", c.spriteIndex);
			static const char* behNames[] = {
				"Not Solid", "Solid", "One-Way", "Slope UR", "Slope UL", "Slope DL", "Slope DR"
			};
			ImGui::Text("Behavior: %s", (c.behaviorType <= 6) ? behNames[c.behaviorType] : "?");
			ImGui::Separator();

			// Reusable field helpers -- every metadata field is one of these two lines.
			auto floatField = [&](const char* label, const char* prop, float def) {
				float v = m_level.GetProperty(c.properties, prop, def);
				if (ImGui::InputFloat(label, &v)) m_level.SetProperty(c.properties, prop, v);
			};
			auto boolField = [&](const char* label, const char* prop, float def = 0.0f) {
				bool b = m_level.GetProperty(c.properties, prop, def) != 0.0f;
				if (ImGui::Checkbox(label, &b)) m_level.SetProperty(c.properties, prop, b ? 1.0f : 0.0f);
			};

			floatField("Friction", BinaryProps::kFriction, 0.15f);
			floatField("Restitution", BinaryProps::kRestitution, 0.15f);

			// ---- Bump block (Mario-style, hit from below). Meaningful on SOLID cells. ----
			ImGui::SeparatorText("Bump Block");
			boolField("Breakable", BinaryProps::kBreakable);
			{
				static const char* kContainNames[] = { "Nothing", "Coin", "Potion Red (+HP)",
					"Potion Blue (1-Up)", "Potion Green (+Time)", "Potion Yellow (+Coins)" };
				int contains = (int)m_level.GetProperty(c.properties, BinaryProps::kContains, 0.0f);
				if (contains < 0 || contains > 5) contains = 0;
				if (ImGui::Combo("Contains", &contains, kContainNames, 6))
					m_level.SetProperty(c.properties, BinaryProps::kContains, (float)contains);
				if (contains != 0)
					floatField("Count", BinaryProps::kContainCount, 1.0f);
			}

			// ---- Entity/spawn type: what this cell BECOMES at load ----
			// A single combo (not one brush per type) -- these are rare + parameterized, so the
			// form lives here, not in the paint toolbar. Values are BinaryObjectType; -1 = plain
			// tile. Picking a type reveals that type's fields, all via the same GetProperty pair.
			ImGui::SeparatorText("Entity");
			int entType = (int)m_level.GetProperty(c.properties, BinaryProps::kEntityType, -1.0f);
			// Combo entries: fixed types + ONE ENTRY PER kEnemyDefs ROW (a new enemy in the
			// table shows up here automatically -- no editor edit needed). Enemy entries store
			// kEntityType = OBJ_ENEMY_PATROL/CHASE by the def's AI (legacy loader default stays
			// aligned) PLUS kEnemyDef = the table index, which is what the loader actually uses.
			static std::vector<const char*> names;
			if (names.empty()) {
				names = { "None (plain tile)", "Player" };
				for (int e = 0; e < kEnemyDefCount; ++e) names.push_back(kEnemyDefs[e].name);
				names.push_back("Coin"); names.push_back("Platform"); names.push_back("Level End");
			}
			const int kEnemyFirst = 2;                       // combo slots [kEnemyFirst, +count) = enemies
			const int kCoinSlot = kEnemyFirst + kEnemyDefCount;
			int curDef = (int)m_level.GetProperty(c.properties, BinaryProps::kEnemyDef,
				(entType == OBJ_ENEMY_CHASE) ? 1.0f : 0.0f); // legacy cells: OBJ value implies the def
			int cur = 0;
			if      (entType == OBJ_PLAYER)    cur = 1;
			else if (entType == OBJ_ENEMY_PATROL || entType == OBJ_ENEMY_CHASE)
				cur = kEnemyFirst + ((curDef >= 0 && curDef < kEnemyDefCount) ? curDef : 0);
			else if (entType == OBJ_COIN)      cur = kCoinSlot;
			else if (entType == OBJ_PLATFORM)  cur = kCoinSlot + 1;
			else if (entType == OBJ_LEVEL_END) cur = kCoinSlot + 2;
			if (ImGui::Combo("Type", &cur, names.data(), (int)names.size())) {
				int obj = -1;
				if      (cur == 1)             obj = OBJ_PLAYER;
				else if (cur >= kEnemyFirst && cur < kCoinSlot) {
					curDef = cur - kEnemyFirst;
					obj = (kEnemyDefs[curDef].ai == EnemyAIType::Chase) ? OBJ_ENEMY_CHASE : OBJ_ENEMY_PATROL;
					m_level.SetProperty(c.properties, BinaryProps::kEnemyDef, (float)curDef);
				}
				else if (cur == kCoinSlot)     obj = OBJ_COIN;
				else if (cur == kCoinSlot + 1) obj = OBJ_PLATFORM;
				else if (cur == kCoinSlot + 2) obj = OBJ_LEVEL_END;
				m_level.SetProperty(c.properties, BinaryProps::kEntityType, (float)obj);
				entType = obj;
			}

			// Per-type parameter fields (defaults match MapReader/LevelInstantiator).
			if (entType == OBJ_PLATFORM) {
				// targetB is ABSOLUTE world coords; default = this cell's own center = stationary.
				float cellCX = m_selCol * m_level.tileWidth + m_level.tileWidth * 0.5f;
				float cellCY = m_selRow * m_level.tileHeight + m_level.tileHeight * 0.5f;
				floatField("Target B X", BinaryProps::kTargetBX, cellCX);
				floatField("Target B Y", BinaryProps::kTargetBY, cellCY);
				// Click-to-pick alternative to typing world coords: arm the pick, then click the
				// destination cell on the grid -- HandleInput writes that cell's center into the
				// two fields above (they re-read the property each frame, so they update live).
				if (m_pickingTargetB) {
					ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
						"Click destination cell... (Esc/RMB cancels)");
				} else if (ImGui::Button("Pick Target B (click a cell)")) {
					m_pickingTargetB = true;
				}
				floatField("Move Speed", BinaryProps::kMoveSpeed, 64.0f);
				// Default matches LevelInstantiator's 1.0f: platform waits for the player to board
				// before it starts moving. Uncheck for a platform that moves from the start.
				boolField ("Is Triggered", BinaryProps::kIsTriggered, 1.0f);
				boolField ("Can Fall",   BinaryProps::kCanFall);
				floatField("Fall Delay", BinaryProps::kFallDelay, 5.0f);
				floatField("Group ID",   BinaryProps::kGroupID, 101.0f);
			}
			else if (entType == OBJ_PLAYER) {
				floatField("Jump Force", BinaryProps::kJumpForce, 420.0f);
				floatField("Health",     BinaryProps::kHealth, 3.0f);
			}
			else if (entType == OBJ_ENEMY_PATROL || entType == OBJ_ENEMY_CHASE) {
				// Param fields keyed off the DEF's AI (not the OBJ value), with the table's own
				// defaults shown -- so a chase-AI enemy always gets a radius field and a patrol
				// one gets bounds, whatever row it is.
				const EnemyDef& def = kEnemyDefs[(curDef >= 0 && curDef < kEnemyDefCount) ? curDef : 0];
				// Bomb gets BOTH field sets: it patrols its bounds AND has a proximity trigger.
				// Its displayed radius default matches the loader's Bomb default (60, not 150 --
				// a trigger far outside the 80px blast always detonates harmlessly).
				if (def.ai == EnemyAIType::Patrol || def.ai == EnemyAIType::Bomb) {
					floatField("Patrol Left",  BinaryProps::kPatrolLeft, -48.0f);
					floatField("Patrol Right", BinaryProps::kPatrolRight, 48.0f);
				}
				if (def.ai != EnemyAIType::Patrol) {
					floatField("Chase Radius", BinaryProps::kChaseRadius,
						(def.ai == EnemyAIType::Bomb) ? 60.0f : 150.0f);
				}
				floatField("Speed",  BinaryProps::kSpeed, def.speed);
				floatField("Health", BinaryProps::kHealth, def.health);
			}
			// OBJ_COIN has no params.

			ImGui::Separator();
			if (ImGui::Button("Clear this cell")) EraseAt(m_selCol, m_selRow);
		}
	}
	ImGui::End();
}

void EditorScene::LoadLevel()
{
	BinaryLevel loaded = BinaryMapFormat::Load(MapPathFor(m_mapName));
	if (loaded.width == 0) return; // missing/invalid file -- don't clobber the current level
	m_level = std::move(loaded);
	m_newW = m_level.width; m_newH = m_level.height; // Resize inputs reflect the loaded size
	m_selCol = m_selRow = -1;      // selection indices are meaningless against the new level
}

void EditorScene::Draw()
{
	DrawLevel(); // renderer-only -- safe every frame, including the entry frame

	// ImGui panels only once a frame has actually started. On the entry frame (scene pushed
	// mid-Update, after main's loop-top NewFrame check) NewFrame did NOT run -- any ImGui
	// call here would AV. See m_firstDraw's declaration.
	if (!*imguiEnabled) return;
	if (m_firstDraw) { m_firstDraw = false; return; }
	DrawPanels();
	DrawInspector();
}

EditorScene::~EditorScene()
{
	*imguiEnabled = false;
}
