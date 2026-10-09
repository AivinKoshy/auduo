# Technical Design Specification: Per-App Audio Routing for AuDuo

**Document Version:** 1.0  
**Feature Name:** **Per-App Audio Routing** (UI Label: *App Routing* / *App Audio Router*)  
**Target Platform:** Windows 10 (Build 1803+) and Windows 11 (All Builds)  
**Author:** Antigravity Engineering  
**Status:** Architecture Proposal & Specification  

---

## 1. Executive Summary & Recommended Nomenclature

### Feature Name Recommendation
- **Primary Technical Term:** **Per-App Audio Routing**
- **User-Facing UI Label:** **App Routing** (Tab name: `[App Routing]`)
- **Alternative / Consumer Name:** **App Device Mapping**
- **Why this name?**  
  "Per-App Audio Routing" is the universally accepted terminology across Windows system documentation, open-source audio tooling (EarTrumpet, AudioSwitch), and Linux audio daemons (PulseAudio/PipeWire). In the UI, shortening it to **App Routing** keeps the navigation bar clean alongside **Devices**, **Auto-Sync**, and **Settings**.

### Goal
Allow AuDuo users to assign specific running applications (e.g., Spotify, Discord, Chrome, VLC, game processes) to specific physical output devices (e.g., Spotify -> Speakers [Device A], Discord -> Headset [Device B], Game -> Secondary Headset [Device C]), or duplicate individual applications across multiple selected devices with independent volume, mute, and delay adjustments.

---

## 2. Technical Evaluation of Windows Audio Approaches

Redirecting application audio on Windows has historically been complex due to how WASAPI and the Windows Audio Engine (`audiodg.exe`) manage endpoints. We evaluate four architectural approaches:

| Approach | Latency | CPU Usage | Multi-Device Duplication | OS Support | DRM / Protected Audio | Stability / Safety |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Approach 1: `IPolicyConfig` COM Redirection** | **0 ms** | **0%** | No (1-to-1 only) | Win 10 1803+ / Win 11 | Full Support | High (Native OS) |
| **Approach 2: WASAPI Process Loopback Capture** | ~5–15 ms | Low (~1%) | **Yes (1-to-N)** | Win 10 20348+ / Win 11 | Limited by DRM | High (Official API) |
| **Approach 3: Virtual Audio Cable (Driver Matrix)** | ~10–20 ms | Low | Yes | All Windows | Full Support | Requires Driver Install |
| **Approach 4: DLL Injection / API Hooking** | Variable | Low | Yes | Fragile | Fails Anti-Cheat | **Unsafe (Rejected)** |

### Approach 1: Native Windows Redirection (`IPolicyConfig` / `IAudioPolicyConfigFactory`)
- **Mechanism:** Windows 10 (1803+) and Windows 11 provide an internal undocumented COM interface (`IPolicyConfig` / `IAudioPolicyConfigFactory`) with `SetPersistedDefaultAudioEndpoint(DWORD processId, ERole role, LPCWSTR endpointDeviceId)`. This is the exact mechanism used by Windows Settings ("App volume and device preferences") and EarTrumpet.
- **Advantages:**
  - **Zero CPU & Zero Buffer Latency:** Windows directly directs the application to open its WASAPI render stream on the chosen endpoint. AuDuo does not touch the audio PCM buffers.
  - **Zero Echo:** The target process renders exclusively to the selected physical device.
  - **Full DRM Compatibility:** Protected audio streams (Spotify, Netflix) play normally.
- **Drawbacks:**
  - **1-to-1 Only:** It routes an application to exactly *one* physical device. It cannot split or duplicate Spotify to play on *both* Device A and Device B simultaneously without a loopback/virtual device.
  - **VTable Differences Across OS Versions:** The COM interface GUID and vtable indices have minor shifts between Windows 10 versions and Windows 11 builds (resolved using version-aware vtables).

### Approach 2: WASAPI Process-Specific Loopback Capture (`AUDCLNT_PROCESS_LOOPBACK_PARAMS`)
- **Mechanism:** Starting in Windows 10 build 20348 and Windows 11, WASAPI provides `ActivateAudioInterfaceAsync` targeting `VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK` with `AUDCLNT_PROCESS_LOOPBACK_PARAMS` (`PROCESS_LOOPBACK_MODE_INCLUDE_PROCESS_TREE`).
- **Advantages:**
  - **True Many-to-Many Routing & Duplication:** AuDuo captures the raw PCM stream of a single app (and its child processes, e.g. Chrome/Discord renderers) and feeds it into AuDuo's `RingBuffer`. AuDuo can then push that audio stream to Device A, Device B, Device C, or any subset of devices simultaneously!
  - **Full DSP Control:** AuDuo can apply independent volume scaling, EQ, and acoustic latency compensation on a per-app basis.
  - **Officially Documented API:** Part of the modern Windows SDK.
- **Drawbacks:**
  - **OS Gate:** Only supported on Windows 10 Build 20348+ and Windows 11.
  - **Echo Hazard:** If the app is currently outputting to the Default Render device, and AuDuo captures it and plays it to Device A (which might also be the Default device), the sound will play twice unless the app's native output is muted or redirected to a null/virtual endpoint.

---

## 3. The AuDuo Dual-Tier Hybrid Architecture (Recommended)

To deliver the best of both worlds (maximum compatibility, zero CPU when doing simple 1-to-1 routing, and advanced duplication when sending an app to multiple devices), AuDuo will employ a **Dual-Tier Hybrid Architecture**:

```
+-----------------------------------------------------------------------------------+
|                                 AuDuo UI (Dear ImGui)                             |
|                                                                                   |
|  [Active App]         [Target Route]                 [Volume]   [VU Meter]        |
|  Spotify.exe          -> [ Device A (Speakers) ]     [ 100% ]   [||||||||  ]      |
|  Discord.exe          -> [ Device B (Headset)  ]     [  85% ]   [||||      ]      |
|  Chrome.exe           -> [ ALL DEVICES (Mirror)]     [  90% ]   [||||||    ]      |
+-----------------------------------------------------------------------------------+
                                         |
                                         v
                      +--------------------------------------+
                      |       AppAudioRouter Coordinator      |
                      +--------------------------------------+
                                  /              \
       [Simple 1-to-1 Routing]   /                \   [1-to-Many / Mirror Mode]
                                v                  v
        +-------------------------------+   +------------------------------------+
        |      AudioPolicyManager       |   |       ProcessCaptureManager        |
        |  (Native IPolicyConfig COM)   |   | (WASAPI Process Loopback Win10/11) |
        |                               |   |                                    |
        |  Calls:                       |   |  - Captures PCM for target PID     |
        |  SetPersistedDefaultAudio-    |   |  - Feeds AuDuo AudioPipeline(s)    |
        |  Endpoint(pid, eConsole, dev) |   |  - Applies volume, sync delay      |
        +-------------------------------+   +------------------------------------+
```

### Routing Modes:
1. **Direct Mode (Default for 1-to-1):**
   - User selects single device: App -> Device A.
   - Handled instantly via `AudioPolicyManager` (`IPolicyConfig`).
   - Latency: **0ms**, CPU: **0%**.
2. **Matrix / Multi-Route Mode (For 1-to-Many or Duo Duplication):**
   - User selects: App -> "Device A + Device B" or "All Devices".
   - Handled via `ProcessCaptureManager` (`AUDCLNT_PROCESS_LOOPBACK_PARAMS`).
   - AuDuo captures the process loopback and feeds its frames into the selected `AudioPipeline` instances.

---

## 4. Detailed Component Design

### 4.1. `AppAudioSessionManager` (Session Discovery & Monitoring)
- **Role:** Enumerate all running desktop applications currently holding an active WASAPI audio session, and notify the UI of new apps starting or closing.
- **Implementation:**
  - Queries `IMMDeviceEnumerator::GetDefaultAudioEndpoint(eRender, eConsole)`.
  - Activates `IAudioSessionManager2`.
  - Obtains `IAudioSessionEnumerator` to inspect all active sessions.
  - Registers `IAudioSessionNotification` to listen asynchronously for new audio sessions (e.g. user opens Spotify or launches a video game while AuDuo is running).
  - Queries `IAudioSessionControl2::GetProcessId()` and uses `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` + `QueryFullProcessImageNameW()` to obtain the executable filename and friendly window title.
  - Extracts the application icon via `ExtractIconExW` / `SHGetFileInfoW` to render a 16x16 / 24x24 icon next to each app in the ImGui interface.

### 4.2. `AudioPolicyManager` (OS-Level Redirection Engine)
- **Role:** Directs Windows audio policy for individual processes.
- **COM Interface Definition:**
  - Wraps `IPolicyConfigVista` / `IPolicyConfig` / `IAudioPolicyConfigFactory`.
  - Defines the interface vtable compatible with Windows 10 (1803 through 22H2) and Windows 11 (21H2 through 24H2):
    ```cpp
    MIDL_INTERFACE("f8679f50-850a-412f-9c75-beeeac9122a7")
    IPolicyConfig : public IUnknown {
    public:
        virtual HRESULT STDMETHODCALLTYPE GetMixFormat(...) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(...) = 0;
        virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(...) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(...) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(...) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(...) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetShareMode(...) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetShareMode(...) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(...) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(...) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(LPCWSTR wszDeviceId, ERole eRole) = 0;
        virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(...) = 0;
        // Windows 10/11 Per-Process Extension:
        virtual HRESULT STDMETHODCALLTYPE SetPersistedDefaultAudioEndpoint(
            DWORD processId,
            ERole role,
            LPCWSTR endpointId
        ) = 0;
        virtual HRESULT STDMETHODCALLTYPE ClearAllPersistedDefaultAudioEndpoints() = 0;
    };
    ```
  - Also implements fallback for `IAudioPolicyConfigFactory` (`CLSID_AudioPolicyConfigFactory` = `{870AF99C-171D-4F9E-AF0D-E63DF40C2BC9}`).
- **Functions:**
  - `SetAppOutputDevice(DWORD pid, const std::wstring& deviceId)`
  - `ResetAppToDefault(DWORD pid)`
  - `GetAppCurrentOutputDevice(DWORD pid, std::wstring& outDeviceId)`

### 4.3. `ProcessCaptureClient` (WASAPI Process-Loopback Engine)
- **Role:** Captures audio from a specific process tree when multi-device duplication or custom DSP/delay is needed.
- **Implementation:**
  - Uses `ActivateAudioInterfaceAsync`:
    ```cpp
    AUDCLNT_PROCESS_LOOPBACK_PARAMS params{};
    params.TargetProcessId = pid;
    params.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_PROCESS_TREE;

    PROPVARIANT activateParams;
    activateParams.vt = VT_BLOB;
    activateParams.blob.cbSize = sizeof(params);
    activateParams.blob.pBlobData = (BYTE*)&params;
    ```
  - In the async completion handler (`IActivateAudioInterfaceCompletionHandler`), retrieves `IAudioClient`, initializes in `AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK`, and fetches `IAudioCaptureClient`.
  - Runs a dedicated capture worker thread that decodes PCM (Float32 / Int16 / Int24) and pushes to the assigned `AudioPipeline` instances.

### 4.4. UI Integration (`UI.cpp` & `UI.hpp`)
- **New Tab:** `App Routing` added alongside `Devices`, `Acoustic Sync`, and `Settings`.
- **UI Elements:**
  - **Quick Search Filter:** Filter running apps by name.
  - **App List Row:**
    - App Icon + App Name (e.g. `[Icon] Spotify.exe (PID: 14208)`).
    - Session Peak VU Meter bar (`[||||||    ]`).
    - Destination Dropdown:
      - `[ System Default ]`
      - `[ Output A: Realtek Speakers ]`
      - `[ Output B: Sony WH-1000XM4 ]`
      - `[ Multi: Output A + B (Duplication) ]`
    - Volume Slider: `0%` to `100%`.
    - Mute Toggle button `[M]`.
    - Reset button `[X]`.

### 4.5. Persistence & State Management (`auduo_config.ini`)
- Routing by executable name (not just transient PID) is essential so that when Spotify or Discord is closed and reopened tomorrow, AuDuo remembers the preference:
  ```ini
  [AppRouting]
  spotify.exe={0.0.0.00000000}.{device-guid-for-speakers}
  discord.exe={0.0.0.00000000}.{device-guid-for-headset}
  chrome.exe=default
  ```
- On AuDuo launch or session change, if a newly spawned process matches a saved rule in `auduo_config.ini`, AuDuo automatically re-applies the route.

---

## 5. Edge Cases & Mitigations

1. **Process Lifecycle & PID Recycling:**
   - Applications frequently terminate and restart with new PIDs.
   - *Mitigation:* The routing map keys by process executable path/name (e.g. `L"spotify.exe"`), while holding the active PID in memory. The session watcher rebinds rules whenever a new session starts.
2. **Multi-Process Architectures (Chrome, Edge, Spotify Helper):**
   - Modern apps run separate processes for UI, GPU, and audio renderers (child processes).
   - *Mitigation:*
     - For `IPolicyConfig`: Passing the parent PID routes the application group in Windows.
     - For Process Loopback: `PROCESS_LOOPBACK_MODE_INCLUDE_PROCESS_TREE` guarantees all child processes spawned by the tree are captured in the single stream.
3. **Ghost Audio Sessions:**
   - Some applications leave idle/silent sessions open.
   - *Mitigation:* Filter sessions by state (`AudioSessionStateActive` vs `AudioSessionStateInactive` / `AudioSessionStateExpired`) or sort apps with active audio VU meters to the top of the list.
4. **Device Disconnection (e.g., Bluetooth Headset turned off):**
   - If a routed app's target device disappears, Windows audio would stall or fail.
   - *Mitigation:* AuDuo's device notification watcher catches device removal and falls the app back to `eConsole` Default Device.

---

## 6. Implementation Stages

- **Stage 1 (Foundation):** Create `AudioPolicyManager` with `IPolicyConfig` COM wrappers and test basic single-app redirection to Device A and Device B.
- **Stage 2 (Session Watcher):** Implement `AppAudioSessionManager` with `IAudioSessionEnumerator` and `IAudioSessionNotification` to actively detect audio-playing apps and their icons.
- **Stage 3 (Process Loopback for Multi-Device):** Implement `ProcessCaptureClient` with `ActivateAudioInterfaceAsync` for multi-device duplication.
- **Stage 4 (UI & Configuration):** Build the Dear ImGui `App Routing` tab, volume controls, search filter, and `auduo_config.ini` rule persistence.
