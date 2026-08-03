#pragma once
#include <DirectXMath.h>

// Flat per-tile colors for GameplayScene::Draw() -- DirectX12/include/Colors.h only defines
// CornflowerBlue/White/Black, none of which match the original raylib project's tile palette.
namespace TileColors {
    constexpr DirectX::XMFLOAT4 DarkGray = { 0.20f, 0.20f, 0.20f, 1.0f };
    constexpr DirectX::XMFLOAT4 Gray     = { 0.50f, 0.50f, 0.50f, 1.0f };
    constexpr DirectX::XMFLOAT4 Orange   = { 1.00f, 0.65f, 0.00f, 1.0f };
    constexpr DirectX::XMFLOAT4 SkyBlue  = { 0.40f, 0.75f, 1.00f, 1.0f };
}
