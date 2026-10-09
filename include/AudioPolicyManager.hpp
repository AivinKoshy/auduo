#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <unordered_map>

namespace Auduo {

// Manages per-process audio endpoint routing via Windows AudioPolicyConfig
class AudioPolicyManager {
public:
    AudioPolicyManager();
    ~AudioPolicyManager();

    // Returns true if AudioPolicyConfig is supported and initialized
    bool IsSupported() const { return m_supported.load(std::memory_order_relaxed); }

    // Route a specific process to an audio output endpoint.
    // Pass empty string for deviceId to reset the process to Windows default.
    bool SetAppOutputDevice(DWORD pid, const std::wstring& deviceId);

    // Reset a process to its system default audio endpoint
    bool ResetAppToDefault(DWORD pid);

    // Clear all active per-app routing rules managed by this instance
    void ClearAllActiveRoutes();

    // Re-apply routing for a given process (e.g. after target device changes or app restart)
    bool ReapplyRoute(DWORD pid, const std::wstring& deviceId);

private:
    bool Initialize();
    void Shutdown();

    typedef void* HSTRING;
    typedef HRESULT (WINAPI *pfn_RoInitialize)(int initType);
    typedef void    (WINAPI *pfn_RoUninitialize)();
    typedef HRESULT (WINAPI *pfn_WindowsCreateString)(const wchar_t* sourceString, UINT32 length, HSTRING* string);
    typedef HRESULT (WINAPI *pfn_WindowsDeleteString)(HSTRING string);
    typedef PCWSTR  (WINAPI *pfn_WindowsGetStringRawBuffer)(HSTRING string, UINT32* length);
    typedef HRESULT (WINAPI *pfn_RoGetActivationFactory)(HSTRING activatableClassId, REFIID iid, void** factory);

    HMODULE m_hComBase = nullptr;
    pfn_RoInitialize m_pRoInitialize = nullptr;
    pfn_RoUninitialize m_pRoUninitialize = nullptr;
    pfn_WindowsCreateString m_pWindowsCreateString = nullptr;
    pfn_WindowsDeleteString m_pWindowsDeleteString = nullptr;
    pfn_WindowsGetStringRawBuffer m_pWindowsGetStringRawBuffer = nullptr;
    pfn_RoGetActivationFactory m_pRoGetActivationFactory = nullptr;

    void* m_pFactory = nullptr;
    mutable std::mutex m_mutex;
    std::atomic<bool> m_supported{false};
    std::atomic<bool> m_initialized{false};

    // Track active routes: PID -> target DeviceId
    std::unordered_map<DWORD, std::wstring> m_activeRoutes;
};

} // namespace Auduo
