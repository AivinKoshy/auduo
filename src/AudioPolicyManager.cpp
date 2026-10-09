#include "AudioPolicyManager.hpp"
#include <unknwn.h>
#include <iostream>
#include <initguid.h>

namespace Auduo {

namespace {
    // Windows 11 (build >= 21390)
    DEFINE_GUID(IID_IAudioPolicyConfigWin11, 0xab3d4648, 0xe242, 0x459f, 0xb0, 0x2f, 0x54, 0x1c, 0x70, 0x30, 0x63, 0x24);
    // Windows 10 (build < 21390)
    DEFINE_GUID(IID_IAudioPolicyConfigWin10, 0x2a59116d, 0x6c4f, 0x45e0, 0xa7, 0x4f, 0x70, 0x7e, 0x3f, 0xef, 0x92, 0x58);

    // VTable index 24 is SetPersistedDefaultAudioEndpoint(void* thisPtr, DWORD pid, DWORD flow, DWORD role, HSTRING deviceId)
    constexpr size_t VTABLE_IDX_SET_PERSISTED = 24;
}

AudioPolicyManager::AudioPolicyManager() {
    Initialize();
}

AudioPolicyManager::~AudioPolicyManager() {
    Shutdown();
}

bool AudioPolicyManager::Initialize() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized.load(std::memory_order_relaxed)) return m_supported.load(std::memory_order_relaxed);

    m_hComBase = LoadLibraryW(L"combase.dll");
    if (!m_hComBase) {
        m_initialized.store(true, std::memory_order_relaxed);
        m_supported.store(false, std::memory_order_relaxed);
        return false;
    }

    m_pRoInitialize = (pfn_RoInitialize)(void*)GetProcAddress(m_hComBase, "RoInitialize");
    m_pRoUninitialize = (pfn_RoUninitialize)(void*)GetProcAddress(m_hComBase, "RoUninitialize");
    m_pWindowsCreateString = (pfn_WindowsCreateString)(void*)GetProcAddress(m_hComBase, "WindowsCreateString");
    m_pWindowsDeleteString = (pfn_WindowsDeleteString)(void*)GetProcAddress(m_hComBase, "WindowsDeleteString");
    m_pWindowsGetStringRawBuffer = (pfn_WindowsGetStringRawBuffer)(void*)GetProcAddress(m_hComBase, "WindowsGetStringRawBuffer");
    m_pRoGetActivationFactory = (pfn_RoGetActivationFactory)(void*)GetProcAddress(m_hComBase, "RoGetActivationFactory");

    if (!m_pRoGetActivationFactory || !m_pWindowsCreateString || !m_pWindowsDeleteString) {
        FreeLibrary(m_hComBase);
        m_hComBase = nullptr;
        m_initialized.store(true, std::memory_order_relaxed);
        m_supported.store(false, std::memory_order_relaxed);
        return false;
    }

    if (m_pRoInitialize) {
        // RO_INIT_MULTITHREADED (1)
        m_pRoInitialize(1);
    }

    HSTRING hsClassName = nullptr;
    const wchar_t* className = L"Windows.Media.Internal.AudioPolicyConfig";
    HRESULT hr = m_pWindowsCreateString(className, static_cast<UINT32>(wcslen(className)), &hsClassName);
    if (FAILED(hr) || !hsClassName) {
        m_initialized.store(true, std::memory_order_relaxed);
        m_supported.store(false, std::memory_order_relaxed);
        return false;
    }

    hr = m_pRoGetActivationFactory(hsClassName, IID_IAudioPolicyConfigWin11, &m_pFactory);
    if (FAILED(hr) || !m_pFactory) {
        hr = m_pRoGetActivationFactory(hsClassName, IID_IAudioPolicyConfigWin10, &m_pFactory);
    }

    m_pWindowsDeleteString(hsClassName);

    if (SUCCEEDED(hr) && m_pFactory) {
        m_supported.store(true, std::memory_order_relaxed);
    } else {
        m_pFactory = nullptr;
        m_supported.store(false, std::memory_order_relaxed);
    }

    m_initialized.store(true, std::memory_order_relaxed);
    return m_supported.load(std::memory_order_relaxed);
}

void AudioPolicyManager::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_initialized.load(std::memory_order_relaxed)) return;

    // Reset any active routes before shutting down
    ClearAllActiveRoutes();

    if (m_pFactory) {
        reinterpret_cast<IUnknown*>(m_pFactory)->Release();
        m_pFactory = nullptr;
    }

    if (m_pRoUninitialize) {
        m_pRoUninitialize();
    }

    if (m_hComBase) {
        FreeLibrary(m_hComBase);
        m_hComBase = nullptr;
    }

    m_supported.store(false, std::memory_order_relaxed);
    m_initialized.store(false, std::memory_order_relaxed);
}

bool AudioPolicyManager::SetAppOutputDevice(DWORD pid, const std::wstring& deviceId) {
    if (pid == 0) return false;

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_pFactory || !m_supported.load(std::memory_order_relaxed)) return false;

    void** vtable = *reinterpret_cast<void***>(m_pFactory);
    if (!vtable || !vtable[VTABLE_IDX_SET_PERSISTED]) return false;

    typedef HRESULT (STDMETHODCALLTYPE *pfn_SetPersisted)(void*, DWORD, DWORD, DWORD, HSTRING);
    pfn_SetPersisted SetPersisted = reinterpret_cast<pfn_SetPersisted>(vtable[VTABLE_IDX_SET_PERSISTED]);

    if (deviceId.empty()) {
        // Passing nullptr resets the application to system default
        HRESULT hr1 = SetPersisted(m_pFactory, pid, 0 /* eRender */, 0 /* eConsole */, nullptr);
        HRESULT hr2 = SetPersisted(m_pFactory, pid, 0 /* eRender */, 1 /* eMultimedia */, nullptr);
        m_activeRoutes.erase(pid);
        return SUCCEEDED(hr1) && SUCCEEDED(hr2);
    }

    HSTRING hsDev = nullptr;
    HRESULT hrStr = m_pWindowsCreateString(deviceId.c_str(), static_cast<UINT32>(deviceId.length()), &hsDev);
    if (FAILED(hrStr) || !hsDev) return false;

    HRESULT hr1 = SetPersisted(m_pFactory, pid, 0 /* eRender */, 0 /* eConsole */, hsDev);
    HRESULT hr2 = SetPersisted(m_pFactory, pid, 0 /* eRender */, 1 /* eMultimedia */, hsDev);

    m_pWindowsDeleteString(hsDev);

    if (SUCCEEDED(hr1) || SUCCEEDED(hr2)) {
        m_activeRoutes[pid] = deviceId;
        return true;
    }
    return false;
}

bool AudioPolicyManager::ResetAppToDefault(DWORD pid) {
    return SetAppOutputDevice(pid, L"");
}

void AudioPolicyManager::ClearAllActiveRoutes() {
    if (!m_pFactory || !m_supported.load(std::memory_order_relaxed)) {
        m_activeRoutes.clear();
        return;
    }

    void** vtable = *reinterpret_cast<void***>(m_pFactory);
    if (vtable && vtable[VTABLE_IDX_SET_PERSISTED]) {
        typedef HRESULT (STDMETHODCALLTYPE *pfn_SetPersisted)(void*, DWORD, DWORD, DWORD, HSTRING);
        pfn_SetPersisted SetPersisted = reinterpret_cast<pfn_SetPersisted>(vtable[VTABLE_IDX_SET_PERSISTED]);

        for (const auto& [pid, _] : m_activeRoutes) {
            SetPersisted(m_pFactory, pid, 0, 0, nullptr);
            SetPersisted(m_pFactory, pid, 0, 1, nullptr);
        }
    }
    m_activeRoutes.clear();
}

bool AudioPolicyManager::ReapplyRoute(DWORD pid, const std::wstring& deviceId) {
    return SetAppOutputDevice(pid, deviceId);
}

} // namespace Auduo
