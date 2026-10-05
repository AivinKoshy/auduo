#include "AudioEngine.hpp"
#include "UI.hpp"

#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#include <tchar.h>
#include <iostream>
#include <string>
#include <chrono>
#include <io.h>
#include <fcntl.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

// Forward declare Win32 message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Global DirectX 11 state
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;
static NOTIFYICONDATAW          g_nid = {};
static const UINT               WM_TRAYICON = WM_USER + 1;
static bool                     g_alwaysMinimizeToTray = false;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Helper to get application icon
static HICON GetAppIcon() {
    HICON hIcon = LoadIconW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(1));
    if (!hIcon) {
        hIcon = LoadIconW(NULL, IDI_APPLICATION);
    }
    return hIcon;
}

// Setup system tray icon
void SetupTrayIcon(HWND hWnd) {
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = GetAppIcon();
    wcscpy_s(g_nid.szTip, L"AuDuo - Dual Audio Synchronizer");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

int RunSmokeTest() {
    std::cout << "[Auduo Smoke Test] Initializing Win32 and DirectX 11 headless mock..." << std::endl;
    HICON hIcon = GetAppIcon();
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), hIcon, nullptr, nullptr, nullptr, L"AuduoSmokeTest", hIcon };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Auduo Smoke Test", WS_OVERLAPPEDWINDOW, 100, 100, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    std::cout << "[Auduo Smoke Test] hwnd: " << hwnd << std::endl;

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        std::cout << "[Auduo Smoke Test] Failed to create D3D11 Device" << std::endl;
        return 1;
    }

    Auduo::AudioEngine engine;
    engine.Initialize();
    Auduo::UI ui(engine);
    if (!ui.Initialize(hwnd, g_pd3dDevice, g_pd3dDeviceContext)) {
        std::cerr << "[Auduo Smoke Test] Failed to initialize UI" << std::endl;
        return 1;
    }

    // Run 10 frames of UI rendering
    for (int i = 0; i < 10; ++i) {
        ui.Render();
    }

    ui.Shutdown();
    engine.Shutdown();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    std::cout << "[Auduo Smoke Test] PASSED: DirectX 11 & ImGui loop executed successfully." << std::endl;
    return 0;
}

int main(int argc, char* argv[]) {
    // If run with CLI arguments, ensure standard output handles work for both interactive consoles and piped subprocesses
    if (argc > 1) {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD outType = (hOut != INVALID_HANDLE_VALUE && hOut != NULL) ? GetFileType(hOut) : FILE_TYPE_UNKNOWN;

        if (outType == FILE_TYPE_DISK || outType == FILE_TYPE_PIPE || outType == FILE_TYPE_CHAR) {
            int fdOut = _open_osfhandle((intptr_t)hOut, _O_TEXT);
            if (fdOut != -1) {
                _dup2(fdOut, 1);
            }
            HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
            if (hErr != INVALID_HANDLE_VALUE && hErr != NULL) {
                int fdErr = _open_osfhandle((intptr_t)hErr, _O_TEXT);
                if (fdErr != -1) {
                    _dup2(fdErr, 2);
                }
            }
        } else {
            if (AttachConsole(ATTACH_PARENT_PROCESS)) {
                FILE* fp = nullptr;
                freopen_s(&fp, "CONOUT$", "w", stdout);
                freopen_s(&fp, "CONOUT$", "w", stderr);
                freopen_s(&fp, "CONIN$", "r", stdin);
            }
        }
        std::ios::sync_with_stdio(true);
    }

    // Check for diagnostic CLI flags
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--test-devices") {
            return Auduo::AudioEngine::RunTestDevices();
        } else if (arg == "--test-buffer") {
            return Auduo::AudioEngine::RunTestBuffer();
        } else if (arg == "--test-capture") {
            int duration = 3;
            if (i + 1 < argc && std::string(argv[i + 1]) == "--duration" && i + 2 < argc) {
                duration = std::atoi(argv[i + 2]);
            }
            return Auduo::AudioEngine::RunTestCapture(duration);
        } else if (arg == "--smoke-test") {
            return RunSmokeTest();
        } else if (arg == "--test-calib") {
            return Auduo::AudioEngine::RunTestCalibration();
        } else if (arg == "--test-autosync") {
            return Auduo::AudioEngine::RunTestAutoSync();
        } else if (arg == "--test-beep") {
            std::wstring match;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                std::string s = argv[i + 1];
                match = std::wstring(s.begin(), s.end());
            }
            return Auduo::AudioEngine::RunTestBeep(match);
        } else if (arg == "--test-vol") {
            return Auduo::AudioEngine::RunTestVolume();
        }
    }

    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    // Initialize Win32 Window
    HICON hAppIcon = GetAppIcon();
    WNDCLASSEXW wc = {
        sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
        GetModuleHandle(nullptr), hAppIcon, nullptr, nullptr, nullptr,
        L"AuduoWindowClass", hAppIcon
    };
    RegisterClassExW(&wc);

    const int clientWidth = 820;
    const int clientHeight = 598;
    DWORD winStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT wr = { 0, 0, clientWidth, clientHeight };
    AdjustWindowRect(&wr, winStyle, FALSE);

    HWND hwnd = CreateWindowW(
        wc.lpszClassName, L"AuDuo - Dual Audio Mirroring & Sync",
        winStyle,
        150, 100, wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        CoUninitialize();
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    Auduo::AudioEngine audioEngine;
    audioEngine.Initialize();

    Auduo::UI ui(audioEngine);
    if (!ui.Initialize(hwnd, g_pd3dDevice, g_pd3dDeviceContext)) {
        audioEngine.Shutdown();
        CleanupRenderTarget();
        CleanupDeviceD3D();
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        CoUninitialize();
        return 1;
    }

    SetupTrayIcon(hwnd);

    // Main Application Loop
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) {
                done = true;
            }
        }
        if (done) break;

        // Check if user requested tray minimization
        if (ui.ShouldMinimizeToTray()) {
            ui.ResetMinimizeToTray();
            ShowWindow(hwnd, SW_HIDE);
        }

        // Automatic microphone monitor lifecycle:
        // Run mic monitor when window is visible; stop and release mic when minimized/hidden in tray.
        bool isVisible = (IsWindowVisible(hwnd) != FALSE) && (IsIconic(hwnd) == FALSE);
        if (!isVisible) {
            if (audioEngine.IsMicMonitorRunning()) {
                audioEngine.StopMicMonitor();
            }
            Sleep(25);
            continue;
        } else if (!audioEngine.IsMicMonitorRunning() && !audioEngine.IsCalibrating()) {
            audioEngine.StartMicMonitor();
        }

        g_alwaysMinimizeToTray = ui.IsAlwaysMinimizeToTray();
        ui.Render();

        const float clearColor[4] = { 0.08f, 0.09f, 0.11f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0); // VSync enabled
    }

    RemoveTrayIcon();
    ui.Shutdown();
    audioEngine.Shutdown();

    CleanupRenderTarget();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    CoUninitialize();

    return 0;
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd,
        &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext
    );
    if (res == DXGI_ERROR_UNSUPPORTED) {
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
            featureLevelArray, 2, D3D11_SDK_VERSION, &sd,
            &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext
        );
    }
    if (res != S_OK) return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_MINIMIZE) {
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        }
        if ((wParam & 0xfff0) == SC_CLOSE && g_alwaysMinimizeToTray) {
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (g_alwaysMinimizeToTray) {
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        }
        break;
    case WM_TRAYICON:
        if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
            ShowWindow(hWnd, SW_RESTORE);
            SetForegroundWindow(hWnd);
        } else if (lParam == WM_RBUTTONUP) {
            HMENU hMenu = CreatePopupMenu();
            InsertMenuW(hMenu, 0, MF_BYPOSITION | MF_STRING, 1001, L"Open AuDuo");
            InsertMenuW(hMenu, 1, MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
            InsertMenuW(hMenu, 2, MF_BYPOSITION | MF_STRING, 1002, L"Exit");
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hWnd);
            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hWnd, nullptr);
            PostMessage(hWnd, WM_NULL, 0, 0); // Microsoft documented fix for tray menu dismissal
            DestroyMenu(hMenu);
            if (cmd == 1001) {
                ShowWindow(hWnd, SW_RESTORE);
                SetForegroundWindow(hWnd);
            } else if (cmd == 1002) {
                g_alwaysMinimizeToTray = false;
                DestroyWindow(hWnd);
            }
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
