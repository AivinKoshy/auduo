# Claude Code Review: AuDuo Architecture & Per-App Audio Routing

**Model:** `claude-fable-5-1`  
**Tokens:** {'prompt_tokens': 3, 'completion_tokens': 8192, 'total_tokens': 8195}

---

# AuDuo Audio Architecture & Per-App Routing — Rigorous Code Review

---

## 1. ARCHITECTURAL COMPATIBILITY & INTEGRATION

### 1.1 The Fundamental Capture Model Conflict

The most significant architectural tension is between the **existing single-source capture model** and the **multi-source capture model** required by Per-App Routing.

**Current model (system mirror):**
```
System Loopback (1× IAudioCaptureClient)
    │
    ├── PushAudio() ──► Pipeline A ──► RenderThread A ──► Device A
    ├── PushAudio() ──► Pipeline B ──► RenderThread B ──► Device B
    └── PushAudio() ──► ExtraPipelines[i] ──► RenderThread ──► Device[i]
```
One capture source, fan-out to N pipelines. Clean SPSC per pipeline.

**Required model (Per-App Routing):**
```
IPolicyConfig (direct 1-to-1, zero capture)
    │
    ├── Spotify.exe ──► Device A (bypasses AuDuo pipelines entirely)
    └── Discord.exe ──► Device B

ProcessCaptureClient(Spotify) ──► Pipeline A ──► RenderThread A ──► Device A
ProcessCaptureClient(Discord) ──► Pipeline B ──► RenderThread B ──► Device B
ProcessCaptureClient(Chrome)  ──► Pipeline A + Pipeline B (multi-device mirror)

System Loopback (remaining apps) ──► Pipelines for "mirror all" mode
```

This is a **fundamental shift**: the engine must now manage **multiple independent capture sources** that may target the same pipeline (requiring mixing), while also supporting the original system-wide mirror mode.

### 1.2 Pipeline Compatibility Assessment

The `AudioPipeline` class is **largely compatible** with the Per-App plan because:

- `PushAudio()` already accepts format parameters (`sampleRate`, `channels`), enabling format-agnostic input from different capture sources.
- The `RingBuffer` SPSC model works for a single producer per pipeline — each `ProcessCaptureClient` would be the sole producer for its assigned pipeline(s).
- Per-pipeline `m_volume` and `m_masterVolume` provide the per-app volume control needed for matrix mode.

**However, there is a critical gap: no mixing logic exists.** If two `ProcessCaptureClient` instances push to the same `AudioPipeline` (e.g., both Spotify and Discord routed to Device A), their audio will be **overwritten**, not mixed. The `RingBuffer` is a pure FIFO — it has no concept of multiple producers or sample-level mixing.

### 1.3 What Must Change in AudioEngine

The `AudioEngine` class needs a **capture source registry** replacing the single `m_pCaptureDevice`/`m_pCaptureClient`/`m_captureThread` trio:

| Current Member | Required Replacement |
|---|---|
| `m_pCaptureDevice` | `std::vector<ProcessCaptureClient*> m_activeCaptures` + optional system-loopback capture |
| `m_captureThread` | Per-capture threads managed by `ProcessCaptureManager`, plus optional system thread |
| `m_pipelineA/B` | Remain, but routing logic must map apps → pipelines |
| `m_mirrorSystemAudio` | Becomes a mode flag: when false, system loopback is disabled entirely |

The `AudioPipeline::PushAudio` calls in `CaptureLoop` must be replaced by a **routing dispatcher** that examines each captured frame's source app and forwards it to the correct pipeline(s), mixing when multiple apps share a destination.

---

## 2. SPECIFIC PITFALLS IN THE PLAN & CODE

### 2.1 IPolicyConfig: Undocumented Interface Fragility

**The plan correctly identifies vtable differences, but underestimates the scope:**

- `IPolicyConfigVista` (GUID `{f8679f50-850a-412f-9c75-beeeac9122a7}`) has **14 methods** in its vtable. The per-process extension `SetPersistedDefaultAudioEndpoint` is **not** on `IPolicyConfigVista` — it's on a separate interface. On Windows 10 1803+, the per-process methods live on `IPolicyConfig` (different GUID: `{568b9108-44bf-40b4-9006-86afe5b5a620}`) or on `IAudioPolicyConfigFactory`.
- The plan's vtable listing mixes methods from both interfaces. In production, you must **detect which interface the OS exposes** at runtime and use the correct vtable.
- `SetPersistedDefaultAudioEndpoint` writes to the registry (`HKCU\Software\Microsoft\Windows\CurrentVersion\Audio\Policy`) and requires the change to be **propagated** via `IAudioPolicyConfigFactory::Reset()` or a device-enumeration refresh to take effect immediately.

**Critical pitfall:** If you call `SetPersistedDefaultAudioEndpoint` and the target app has already opened its WASAPI render stream, the redirection **won't take effect until the app reinitializes audio**. You must either:
1. Signal the app to restart its audio (not generally possible), or
2. Use the process loopback approach as a fallback when the app doesn't pick up the new endpoint.

### 2.2 COM Apartment & Threading Issues

**`EnumerateAudioProcesses()` — CoInitialize/CoUninitialize per call:**

```cpp
HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
bool shouldUninit = SUCCEEDED(hr);
// ... work ...
if (shouldUninit) CoUninitialize();
```

This is **fragile**. If this static method is ever called from a thread that already has COM initialized (e.g., the capture thread, a render thread, or a callback thread), `CoInitializeEx` returns `RPC_E_CHANGED_MODE` and `shouldUninit` becomes `false` — but the thread's COM state is now inconsistent. The function proceeds with a half-initialized COM environment.

**Fix:** Use a thread-local COM guard or require the caller to have COM initialized.

**`IAudioSessionNotification` callback threading:**

The `IAudioSessionNotification::OnNewSession` callback is invoked on an **MTA thread** (since `IAudioSessionManager2` was activated from MTA). Your session manager must be thread-safe. The current `AudioEngine` uses `std::mutex` for some state but the session enumeration in `EnumerateAudioProcesses` has no synchronization with the streaming state.

**`EndpointVolumeWatcher` callback threading:**

The `IAudioEndpointVolumeCallback::OnNotify` is invoked on a thread owned by the Windows audio service (`Audiosrv`). This thread is in an MTA. Your `HandleEndpointVolumeNotification` must be thread-safe and must **not** call COM methods that require STA (most don't, but `IAudioSessionManager2` methods should be called from MTA, which is fine here).

### 2.3 RingBuffer SPSC Violation via `SetDelayFrames`

```cpp
void SetDelayFrames(size_t targetBufferedFrames) {
    // ...
    size_t written = Write(zeroChunk, step);  // ← PRODUCER API
    // ...
    size_t readIdx = m_readIndex.load(std::memory_order_relaxed);
    m_readIndex.store(readIdx + diff, std::memory_order_release);  // ← CONSUMER API
}
```

`SetDelayFrames` is a public method that can be called from **any thread** (UI thread, render thread, calibration thread). It calls both `Write()` (producer) and directly manipulates `m_readIndex` (consumer). This **breaks the SPSC invariant** — the ring buffer now has multiple writers to `m_writeIndex` (capture thread + `SetDelayFrames` caller) and multiple readers of `m_readIndex` (render thread + `SetDelayFrames` caller).

**Consequence:** Under contention, frames can be lost or duplicated, and the `usedFrames` calculation in `Write()` can produce incorrect results because `m_readIndex` may change between the load and the write.

### 2.4 CaptureLoop Format Conversion — Heap Allocation in Real-Time Path

```cpp
floatConversionBuffer.resize(numFramesAvailable * channels);
```

This `std::vector::resize()` call **allocates or deallocates heap memory on every capture packet**. In a real-time audio thread, heap operations are unacceptable — they can cause page faults and priority inversions. The buffer should be pre-allocated to the maximum expected packet size (typically 10ms at 48kHz = 480 frames × 8 channels = 3840 floats = ~15KB, well within stack limits).

**Additionally, the 24-bit conversion has a subtle sign-extension correctness issue** that works on most compilers but is technically undefined behavior:

```cpp
int32_t val = (static_cast<int8_t>(bytes[i * 3 + 2]) << 16) | ...;
```

`static_cast<int8_t>` produces a value in `[-128, 127]`. The integer promotion rules promote it to `int` before the shift, which preserves the sign. This is correct in practice, but the cleaner approach is:

```cpp
int32_t val = (static_cast<int32_t>(static_cast<int8_t>(bytes[i * 3 + 2])) << 16) | ...;
```

### 2.5 EnumerateAudioProcesses — Incomplete Session Discovery

The current implementation has three significant gaps:

1. **Default-device-only enumeration:** It only queries sessions on the default render endpoint. If an app has been redirected to a non-default device (via `IPolicyConfig` or Windows Settings), its session won't appear. You must enumerate **all** active render endpoints:
   ```cpp
   // Current: only default device
   pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev);
   
   // Required: enumerate all devices
   IMMDeviceCollection* pCollection = nullptr;
   pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
   // Then iterate and activate IAudioSessionManager2 on each
   ```

2. **No session state filtering:** Idle/expired sessions appear in the list. Filter by `IAudioSessionControl::GetState()` returning `AudioSessionStateActive`.

3. **No `IAudioSessionNotification` registration:** The plan mentions it in section 4.1 but the current code doesn't implement it. Without real-time session notifications, the UI list is stale between manual refreshes.

### 2.6 Echo Prevention Is Fragile

```cpp
if (!m_playingBeep.load(...) && !m_isCalibrating.load(...) && !m_metronomeRunning.load(...)) {
    // Push to pipelines
}
```

This gates **all** pipeline pushes during internal audio generation. But the echo problem is specific: if the default device is also a pipeline target, internal audio played through the system will be captured and re-routed. The current approach works but is overly broad — it suppresses legitimate external audio during calibration/metronome. A better approach is per-pipeline echo gating based on whether the pipeline's device is the default device.

---

## 3. CONCRETE CODE RECOMMENDATIONS & REFACTORINGS

### 3.1 AudioPolicyManager — Production-Ready COM Wrapper

```cpp
// include/AudioPolicyManager.hpp
#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <string>
#include <unordered_map>
#include <atomic>
#include <mutex>

namespace Auduo {

// Version-aware IPolicyConfig interface
// GUID {568b9108-44bf-40b4-9006-86afe5b5a620} — Windows 10 1803+ per-process extension
class AudioPolicyManager {
public:
    AudioPolicyManager();
    ~AudioPolicyManager();

    // Returns true if the OS supports per-app redirection (Win 10 1803+)
    bool IsSupported() const { return m_supported.load(std::memory_order_relaxed); }

    // Route a process to a specific device. Returns S_OK on success.
    // Pass empty deviceId to reset to system default.
    HRESULT SetAppOutputDevice(DWORD pid, const std::wstring& deviceId);

    // Reset a process to its default device
    HRESULT ResetAppToDefault(DWORD pid);

    // Query which device a process is currently routed to
    HRESULT GetAppCurrentOutputDevice(DWORD pid, std::wstring& outDeviceId) const;

    // Clear all persisted per-app routes
    HRESULT ClearAllRoutes();

    // Refresh internal device ID cache (call after device changes)
    void RefreshDeviceCache();

private:
    HRESULT InitializePolicyConfig();
    void ShutdownPolicyConfig();

    // Internal interface wrapper — abstracts vtable differences
    struct PolicyConfigVTable {
        // Index 14 on IPolicyConfig (Win 10 1803+)
        HRESULT(STDMETHODCALLTYPE *SetPersistedDefaultAudioEndpoint)(
            void* thisPtr, DWORD processId, ERole role, LPCWSTR endpointId);
        // Index 15
        HRESULT(STDMETHODCALLTYPE *ClearAllPersistedDefaultAudioEndpoints)(void* thisPtr);
    };

    HRESULT GetPolicyConfigInterface(void** ppv);
    HRESULT GetDeviceIdFromFriendlyName(const std::wstring& friendlyName, std::wstring& outId) const;

    mutable std::mutex m_mutex;
    std::atomic<bool> m_supported{false};
    std::atomic<bool> m_initialized{false};

    // Cached device ID → friendly name mapping
    mutable std::unordered_map<std::wstring, std::wstring> m_deviceIdToName;
    mutable std::unordered_map<std::wstring, std::wstring> m_nameToDeviceId;

    void* m_pPolicyConfig = nullptr;  // IPolicyConfig or IAudioPolicyConfigFactory
    PolicyConfigVTable m_vtable{};
};

} // namespace Auduo
```

```cpp
// src/AudioPolicyManager.cpp
#include "AudioPolicyManager.hpp"
#include <functiondiscoverykeys_devpkey.h>
#include <devpkey.h>
#include <propvarutil.h>
#include <newdev.h>
#include <combaseapi.h>

namespace Auduo {

namespace {
    // CLSID_AudioPolicyConfigFactory — the factory that creates the policy config
    constexpr GUID CLSID_AudioPolicyConfigFactory = 
        { 0x870AF99C, 0x171D, 0x4F9E, { 0xAF, 0x0D, 0xE6, 0x3D, 0xF4, 0x0C, 0x2B, 0xC9 } };

    // IPolicyConfig GUID (Win 10 1803+ with per-process extension)
    constexpr GUID IID_IPolicyConfig = 
        { 0x568b9108, 0x44bf, 0x40b4, { 0x90, 0x06, 0x86, 0xAF, 0xE5, 0xB5, 0xA6, 0x20 } };

    // Fallback: IPolicyConfigVista (no per-process methods, but useful for detection)
    constexpr GUID IID_IPolicyConfigVista = 
        { 0xf8679f50, 0x850a, 0x412f, { 0x9c, 0x75, 0xbe, 0xee, 0xac, 0x91, 0x22, 0xa7 } };
}

AudioPolicyManager::AudioPolicyManager() {
    std::lock_guard<std::mutex> lock(m_mutex);
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool comInit = SUCCEEDED(hr);
    
    hr = InitializePolicyConfig();
    if (SUCCEEDED(hr)) {
        m_supported.store(true, std::memory_order_relaxed);
        RefreshDeviceCache();
    }
    
    m_initialized.store(true, std::memory_order_relaxed);
    if (comInit) CoUninitialize();
}

AudioPolicyManager::~AudioPolicyManager() {
    std::lock_guard<std::mutex> lock(m_mutex);
    ShutdownPolicyConfig();
}

HRESULT AudioPolicyManager::InitializePolicyConfig() {
    // Try IPolicyConfig first (has per-process methods on Win 10 1803+)
    HRESULT hr = GetPolicyConfigInterface(reinterpret_cast<void**>(&m_pPolicyConfig));
    if (SUCCEEDED(hr) && m