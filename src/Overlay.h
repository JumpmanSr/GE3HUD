// Overlay.h -- in-game monster HP HUD (D3D11 + Dear ImGui).
//
// ge3.exe statically imports d3d11.dll and dxgi.dll, so we hook
// IDXGISwapChain::Present (vtable slot 8) and draw there.
//
// Passive by design: WndProc is not hooked and the ImGui window is created
// with NoInputs, so the overlay can never swallow a keypress or a click. The
// only input read is a GetAsyncKeyState poll for the hotkeys.

#pragma once

#define GE3MT_NAME "GE3HUD"
#define GE3MT_VERSION "1.0.1"
#define GE3MT_AUTHOR "Jumpman"
#define GE3MT_HINT "F1 toggle  -  F2 log"

namespace Overlay {

// Call from DllMain's DLL_PROCESS_ATTACH. Spawns a worker and returns
// immediately -- no work happens under the loader lock.
void Install();

// Call from DLL_PROCESS_DETACH.
void Remove();

extern int g_toggleKey;   // default VK_F1
extern int g_dumpKey;     // default VK_F2 -- console dump, works standalone

extern bool g_visible;    // starts false: dormant until asked
extern bool g_hideOutsideMissions;

// Placement and size as fractions of the screen, so it lands the same on any
// resolution. The window is NoInputs and therefore cannot be dragged.
extern float g_posXPct;
extern float g_posYPct;
extern float g_uiScale;

// When true, g_posXPct is where the HUD's RIGHT edge sits and it grows
// leftwards. Set false to anchor the left edge instead.
extern bool g_anchorRight;

}  // namespace Overlay
