#include "Overlay.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "ActorList.h"
#include "Rtti.h"
#include "Signature.h"
#include "detours.h"
#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace Overlay {

int g_toggleKey = VK_F1;
int g_dumpKey = VK_F2;
bool g_visible = false;            // dormant until F1
bool g_hideOutsideMissions = true;
// Anchored to the right edge: g_posXPct is where that edge sits, and the
// window is pivoted so it grows leftwards. Without the pivot an auto-resizing
// window would spill off-screen, since its width is not known until after it
// has been laid out.
float g_posXPct = 0.985f;
float g_posYPct = 0.40f;
float g_uiScale = 1.18f;
bool g_anchorRight = true;

namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT,
                                             DXGI_FORMAT, UINT);

PresentFn oPresent = nullptr;
ResizeFn oResizeBuffers = nullptr;

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
HWND g_hwnd = nullptr;
bool g_imguiReady = false;
bool g_installed = false;

// ---------------------------------------------------------------- console

void EnsureConsole() {
    static bool tried = false;
    if (tried) return;
    tried = true;
    if (GetConsoleWindow()) return;
    if (AllocConsole()) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        SetConsoleTitleA(GE3MT_NAME " " GE3MT_VERSION);
    }
}

// Deliberately independent of the overlay: if ImGui or the render target
// failed, this still answers "is the mod reading the game correctly?".
void DumpToConsole() {
    EnsureConsole();
    ActorList::Init();
    ActorList::Refresh();

    std::printf("\n" GE3MT_NAME " " GE3MT_VERSION " by " GE3MT_AUTHOR "\n");
    std::printf("  module     0x%p\n",
                reinterpret_cast<void*>(Sig::ModuleBase()));
    std::printf("  classes    %zu actor vtables\n", Rtti::ClassCount());
    std::printf("  singleton  0x%p -> 0x%p\n",
                reinterpret_cast<void*>(ActorList::ManagerPointerAddress()),
                reinterpret_cast<void*>(ActorList::ManagerAddress()));
    std::printf("  list       ActorManager + 0x%zX%s\n",
                ActorList::ListOffset(),
                ActorList::ListOffsetWasRecovered() ? "  (auto-recovered)" : "");
    std::printf("  status     %s\n", ActorList::StatusText());

    const auto& list = ActorList::Enemies();
    std::printf("  %zu enemy actor(s)\n", list.size());
    if (list.empty()) return;
    std::printf("\n  %-16s %10s %10s %6s   %s\n",
                "MONSTER", "HP", "MAX", "PCT", "ADDRESS");
    for (const auto& e : list) {
        std::printf("  %-16s %10.0f %10.0f %5.0f%%   0x%p%s\n",
                    e.name ? e.name : "?", e.life, e.maxLife,
                    e.Fraction() * 100.0f,
                    reinterpret_cast<void*>(e.address),
                    e.IsAlive() ? "" : "  (down)");
    }
    std::fflush(stdout);
}

// ------------------------------------------------------------- appearance

// Rasterise a real TTF at the size we draw at. io.FontGlobalScale stretches
// an already-baked bitmap, and ImGui's built-in font is a 13px pixel font, so
// scaling it that way looks blurry.
void LoadFont(ImGuiIO& io, float scale) {
    char dir[MAX_PATH]{};
    if (!GetWindowsDirectoryA(dir, MAX_PATH)) {
        io.Fonts->AddFontDefault();
        return;
    }
    const char* candidates[] = {"\\Fonts\\segoeui.ttf", "\\Fonts\\consola.ttf"};
    const float px = floorf(13.0f * scale + 0.5f);
    for (const char* rel : candidates) {
        char path[MAX_PATH];
        std::snprintf(path, sizeof(path), "%s%s", dir, rel);
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
        if (io.Fonts->AddFontFromFileTTF(path, px)) return;
    }
    io.Fonts->AddFontDefault();
}

void CenteredText(const char* text, const ImVec4& colour) {
    float avail = ImGui::GetContentRegionAvail().x;
    float textW = ImGui::CalcTextSize(text).x;
    if (avail > textW)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - textW) * 0.5f);
    ImGui::TextColored(colour, "%s", text);
}

ImU32 HealthColour(float f) {
    if (f > 0.5f) return IM_COL32(90, 210, 110, 255);
    if (f > 0.2f) return IM_COL32(230, 190, 70, 255);
    return IM_COL32(225, 80, 80, 255);
}

void DrawBar(ImDrawList* dl, ImVec2 pos, float w, float h, float fraction) {
    fraction = fraction < 0.0f ? 0.0f : (fraction > 1.0f ? 1.0f : fraction);
    ImVec2 a = pos, b = ImVec2(pos.x + w, pos.y + h);
    dl->AddRectFilled(a, b, IM_COL32(20, 20, 24, 190), 2.0f);
    if (fraction > 0.0f)
        dl->AddRectFilled(a, ImVec2(pos.x + w * fraction, b.y),
                          HealthColour(fraction), 2.0f);
    dl->AddRect(a, b, IM_COL32(0, 0, 0, 200), 2.0f);
}

void DrawHud() {
    const auto& list = ActorList::Enemies();
    ActorList::Status status = ActorList::GetStatus();
    bool broken = status == ActorList::Status::NoSignature ||
                  status == ActorList::Status::NoRtti;

    // Hide outside missions, but never hide a real error -- otherwise a
    // failed signature looks identical to "no monsters here".
    if (g_hideOutsideMissions && list.empty() && !broken) return;

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(
        ImVec2(io.DisplaySize.x * g_posXPct, io.DisplaySize.y * g_posYPct),
        ImGuiCond_Always,
        ImVec2(g_anchorRight ? 1.0f : 0.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.42f);
    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;

    if (ImGui::Begin("##" GE3MT_NAME, nullptr, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float barW = 190.0f * g_uiScale;
        const float barH = 9.0f * g_uiScale;

        CenteredText(GE3MT_NAME " " GE3MT_VERSION,
                     ImVec4(0.55f, 0.80f, 1.00f, 1.00f));
        CenteredText("By " GE3MT_AUTHOR, ImVec4(0.90f, 0.22f, 0.22f, 1.00f));
        ImGui::Separator();
        ImGui::Spacing();

        if (broken) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
                               ActorList::StatusText());
            ImGui::TextDisabled("this build of the game may differ");
        } else if (list.empty()) {
            ImGui::TextDisabled("no monsters");
        }

        for (const auto& e : list) {
            if (e.IsAlive())
                ImGui::TextUnformatted(e.name ? e.name : "?");
            else
                ImGui::TextDisabled("%s (down)", e.name ? e.name : "?");

            ImGui::SameLine();
            char hp[64];
            std::snprintf(hp, sizeof(hp), "%.0f / %.0f", e.life, e.maxLife);
            float avail = ImGui::GetContentRegionAvail().x;
            float textW = ImGui::CalcTextSize(hp).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                 (avail > textW ? avail - textW : 0.0f));
            ImGui::TextUnformatted(hp);

            ImVec2 p = ImGui::GetCursorScreenPos();
            DrawBar(dl, p, barW, barH, e.Fraction());
            ImGui::Dummy(ImVec2(barW, barH + 5.0f * g_uiScale));
        }

        ImGui::Spacing();
        ImGui::Separator();
        CenteredText(GE3MT_HINT, ImVec4(0.85f, 0.75f, 0.42f, 1.00f));
    }
    ImGui::End();
}

// --------------------------------------------------------------- plumbing

void ReleaseTarget() {
    if (g_rtv) {
        g_rtv->Release();
        g_rtv = nullptr;
    }
}

bool EnsureTarget(IDXGISwapChain* swap) {
    if (g_rtv) return true;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swap->GetBuffer(0, __uuidof(ID3D11Texture2D),
                               reinterpret_cast<void**>(&back))) || !back)
        return false;
    HRESULT hr = g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
    return SUCCEEDED(hr);
}

bool EnsureImGui(IDXGISwapChain* swap) {
    if (g_imguiReady) return true;
    if (FAILED(swap->GetDevice(__uuidof(ID3D11Device),
                               reinterpret_cast<void**>(&g_device))))
        return false;
    g_device->GetImmediateContext(&g_context);

    DXGI_SWAP_CHAIN_DESC desc{};
    swap->GetDesc(&desc);
    g_hwnd = desc.OutputWindow;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;      // do not litter the game folder
    io.MouseDrawCursor = false;
    LoadFont(io, g_uiScale);
    ImGui_ImplWin32_Init(g_hwnd);  // no WndProc hook: the HUD is passive
    ImGui_ImplDX11_Init(g_device, g_context);

    g_imguiReady = true;
    return true;
}

HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* swap, UINT interval,
                                    UINT flags) {
    static bool toggleDown = false;
    bool down = (GetAsyncKeyState(g_toggleKey) & 0x8000) != 0;
    if (down && !toggleDown) {
        g_visible = !g_visible;
        if (g_visible) ActorList::Init();
    }
    toggleDown = down;

    static bool dumpDown = false;
    bool dump = (GetAsyncKeyState(g_dumpKey) & 0x8000) != 0;
    if (dump && !dumpDown) DumpToConsole();
    dumpDown = dump;

    if (g_visible) {
        // Walking the game's own list costs microseconds, so it happens every
        // frame. There is no cache, and therefore nothing that can go stale.
        ActorList::Refresh();
        if (EnsureImGui(swap) && EnsureTarget(swap)) {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            DrawHud();
            ImGui::Render();
            g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
    }
    return oPresent(swap, interval, flags);
}

HRESULT STDMETHODCALLTYPE hkResizeBuffers(IDXGISwapChain* swap, UINT count,
                                          UINT w, UINT h, DXGI_FORMAT fmt,
                                          UINT flags) {
    ReleaseTarget();  // the back buffer is about to be recreated
    return oResizeBuffers(swap, count, w, h, fmt, flags);
}

bool GrabSwapchainVtable(void** presentOut, void** resizeOut) {
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "ge3mt_probe";
    if (!RegisterClassExA(&wc)) return false;

    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPEDWINDOW,
                                0, 0, 8, 8, nullptr, nullptr, wc.hInstance,
                                nullptr);
    if (!hwnd) {
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return false;
    }

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    IDXGISwapChain* swap = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1,
        D3D11_SDK_VERSION, &sd, &swap, &dev, nullptr, &ctx);

    bool ok = false;
    if (SUCCEEDED(hr) && swap) {
        void** vt = *reinterpret_cast<void***>(swap);
        *presentOut = vt[8];   // IDXGISwapChain::Present
        *resizeOut = vt[13];   // IDXGISwapChain::ResizeBuffers
        ok = true;
    }
    if (swap) swap->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return ok;
}

DWORD WINAPI Worker(LPVOID) {
    void* present = nullptr;
    void* resize = nullptr;
    for (int attempt = 0; attempt < 40 && !present; ++attempt) {
        if (GrabSwapchainVtable(&present, &resize)) break;
        Sleep(250);
    }
    if (!present) {
        std::printf("[" GE3MT_NAME "] could not obtain the swapchain vtable\n");
        return 0;
    }

    oPresent = reinterpret_cast<PresentFn>(present);
    oResizeBuffers = reinterpret_cast<ResizeFn>(resize);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&reinterpret_cast<PVOID&>(oPresent), hkPresent);
    DetourAttach(&reinterpret_cast<PVOID&>(oResizeBuffers), hkResizeBuffers);
    LONG err = DetourTransactionCommit();

    if (err == NO_ERROR) {
        g_installed = true;
        std::printf("[" GE3MT_NAME "] hooked Present at 0x%p\n", present);
    } else {
        std::printf("[" GE3MT_NAME "] DetourTransactionCommit failed: %ld\n", err);
    }
    return 0;
}

}  // namespace

void Install() { CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr); }

void Remove() {
    if (!g_installed) return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&reinterpret_cast<PVOID&>(oPresent), hkPresent);
    DetourDetach(&reinterpret_cast<PVOID&>(oResizeBuffers), hkResizeBuffers);
    DetourTransactionCommit();
    g_installed = false;
}

}  // namespace Overlay
