# Claude Architectural Proposal: Per-App Audio Routing on Windows

**Model:** `claude-fable-5-1`  
**Tokens Used:** Prompt 3, Completion 8192, Total 8195  

---

# Per-Application Audio Routing on Windows: Architectural Proposal for AuDuo

---

## 1. FEATURE NOMENCLATURE

**Industry-standard technical term:** **Per-Application Audio Device Routing** (often shortened to **Per-App Audio Routing** or **App-Specific Audio Routing**).

**User-facing naming conventions observed in the ecosystem:**
- **Audio Router** (the de-facto colloquial term, popularized by the open-source AudioRouter utility by "extradmd")
- **App Audio Switcher** (used by EarTrumpet's feature set)
- **Per-App Audio Device Assignment** (Microsoft's internal terminology, visible in Windows 11 Settings → System → Sound → App volume and device preferences)
- **Application Audio Endpoint Routing** (formal/technical documentation style)

**Recommended for AuDuo:** Use **"Per-App Audio Routing"** as the primary feature name, with **"Audio Router"** as the colloquial shorthand in marketing materials. Internally, refer to it as **Application-Specific Endpoint Assignment (AEA)**.

---

## 2. VIABLE TECHNICAL APPROACHES ON WINDOWS

### Approach A: IPolicyConfig / IAudioPolicyConfigFactory COM Interface (Semi-Documented)

**Mechanism:** Reverse-engineered COM interfaces that sit at the Windows Audio Stack level. `IPolicyConfig` (Vista+) and its successor `IAudioPolicyConfigFactory` (Windows 10 1803+) allow setting the audio endpoint for a specific audio session directly at the audio engine level, before the audio is rendered.

```cpp
// Conceptual interface (semi-documented)
interface IAudioPolicyConfigFactory : IUnknown {
    HRESULT SetDeviceEndpoint(LPCWSTR sessionId, LPCWSTR endpointId);
    HRESULT GetDeviceEndpoint(LPCWSTR sessionId, LPWSTR* endpointId);
    // ... additional methods
};
```

**Key characteristics:**
- Operates at the `AudioSes` service level (same level as the Windows Sound control panel)
- Session identification uses the **session GUID** (obtained via `IAudioSessionControl::GetSessionIdentifier`)
- Requires the application to run with **administrative privileges** on some Windows builds
- The interface CLSID/interface IID must be obtained from the Windows SDK headers or reverse-engineered

### Approach B: WASAPI Loopback Capture + Cross-Device Render (Fully Documented)

**Mechanism:** For each target application's audio session:
1. Open a **WASAPI loopback capture stream** on the default render endpoint, filtered to the specific session
2. Read audio buffers from the capture stream
3. Open a **WASAPI render stream** on the target physical device
4. Write audio buffers to the render stream

```cpp
// Conceptual flow
IAudioClient* pCaptureClient; // Loopback, session-specific
IAudioClient* pRenderClient;  // Target device

// Capture from session X on default device
pCaptureClient->GetService(__uuidof(IAudioCaptureClient), ...);

// Render to target device
pRenderClient->GetService(__uuidof(IAudioRenderClient), ...);

// Pump loop: capture -> buffer -> render
```

**Key characteristics:**
- Fully documented and supported by Microsoft
- Session-specific capture requires `IAudioSessionControl::GetSessionIdentifier` and passing it to `IAudioClient::Initialize` via the `AudioClientProperties` structure
- Adds **one full buffer period of latency** (typically 10-20ms at 10ms buffer size)
- CPU overhead for buffer copying and format conversion
- **Does NOT capture DRM-protected content** (Protected Media Path blocks loopback capture)

### Approach C: Virtual Audio Driver (VAC-Style Kernel Driver)

**Mechanism:** Install a **virtual audio device driver** (similar to Virtual Audio Cable, VB-Cable, or BlackHole on macOS). The virtual device appears as a standard audio endpoint to Windows. Applications are routed to the virtual device, and a user-mode service reads from the virtual device and writes to physical devices.

**Key characteristics:**
- Requires a **signed kernel-mode driver** (WHQL or EV code signing)
- The virtual device is a **WDM audio driver** implementing the `PortCls` (Port Class) driver model
- Provides the lowest possible latency for 1-to-N routing
- Most flexible architecture but highest barrier to entry
- Driver installation requires admin rights and system reboot on first install

### Approach D: API Hooking / DLL Detours (In-Process Injection)

**Mechanism:** Inject a DLL into the target application process and hook audio APIs (`waveOutWrite`, `DirectSoundBuffer::Play`, `IAudioRenderClient::GetBuffer`, etc.). Redirect audio buffers to a shared memory region or named pipe, then read from the user-mode service and render to the target device.

**Key characteristics:**
- Extremely fragile across Windows versions and application updates
- Breaks with **Protected Audio Path** (DRM) — the hook may be blocked or the application may detect the injection
- Anti-cheat software (EAC, BattlEye, Vanguard) flags DLL injection as malicious
- 64-bit vs 32-bit process architecture mismatches require separate injection binaries
- **Not recommended for production** — included here for completeness

### Approach E: Windows 10 1903+ `IAudioSessionControl2` + `IAudioSessionEvents` Notification Model

**Mechanism:** Use the documented session management APIs to enumerate and monitor sessions, combined with the undocumented `IPolicyConfig` for actual routing. This is the "hybrid" approach — documented for discovery, semi-documented for control.

**Key characteristics:**
- `IAudioSessionEnumerator` + `IAudioSessionControl2` provide full session lifecycle visibility
- `IAudioSessionEvents` provides notifications for session creation, expiration, and state changes
- The routing itself still requires `IPolicyConfig` or WASAPI loopback

---

## 3. DETAILED TRADE-OFF COMPARISON

| Criterion | IPolicyConfig (A) | WASAPI Loopback+Render (B) | Virtual Audio Driver (C) | API Hooking (D) |
|---|---|---|---|---|
| **Buffer Latency** | **~0ms added** (direct routing at audio engine level) | **1 buffer period** (10-20ms typical) | **~0ms** (direct kernel-to-user) | **~0ms** (in-process) |
| **CPU Overhead** | Negligible (COM call) | Moderate (buffer copy + resample if needed) | Low (DMA transfer) | Low-Moderate (hook overhead) |
| **Memory Overhead** | Negligible | ~2-4 buffers per routed session | Driver pool memory | Injection overhead per target |
| **1-to-1 Routing** | ✅ Native | ✅ Native | ✅ Native | ✅ Native |
| **1-to-N Duplication** | ❌ Not supported natively | ✅ Capture once, render N times | ✅ Native (splitter device) | ⚠️ Possible but complex |
| **Win10 1803+** | ✅ IAudioPolicyConfigFactory | ✅ WASAPI stable since Vista | ✅ PortCls stable | ⚠️ Fragile |
| **Win10 1909/22H2** | ✅ Works | ✅ Works | ✅ Works | ⚠️ Degraded |
| **Win11 21H2-24H2** | ⚠️ Partial (some builds broke it) | ✅ Works | ✅ Works | ❌ Often blocked |
| **DRM/Protected Path** | ✅ Routes DRM content | ❌ Loopback blocked for DRM | ✅ Routes DRM content | ❌ Often blocked |
| **Driver Signing** | ❌ Not required | ❌ Not required | ✅ Required (WHQL/EV) | ❌ Not required |
| **Admin Rights** | ⚠️ Sometimes required | ❌ Not required | ✅ Required (install) | ⚠️ Required (injection) |
| **System Stability** | ✅ High (read-only COM) | ✅ High | ⚠️ Medium (driver bugs = BSOD) | ❌ Low (crashes target) |
| **OS Update Resilience** | ⚠️ Medium (undocumented) | ✅ High (documented) | ✅ High | ❌ Low |

### Critical Observations:

1. **DRM is the discriminator:** If AuDuo needs to route Spotify (with DRM), Netflix, or games using Protected Media Path, **Approach A or C are the only viable options**. WASAPI loopback (B) will silently fail to capture DRM content.

2. **1-to-N duplication requires Approach B or C:** `IPolicyConfig` only supports assigning a session to a single endpoint. For splitting one app across multiple devices, you must capture and re-render (B) or use a virtual device (C).

3. **Windows 11 compatibility risk for IPolicyConfig:** Windows 11 22H2 and 23H2 introduced changes to the audio stack. Some builds of `IAudioPolicyConfigFactory` have broken or changed behavior. This is the primary risk for Approach A.

4. **The "Sweet Spot" for a lightweight desktop app:** Approach A (for 1-to-1, DRM-compatible routing) + Approach B (as fallback and for 1-to-N duplication) provides the best balance of functionality, stability, and deployment simplicity.

---

## 4. RECOMMENDED PRODUCTION ARCHITECTURE & C++20 IMPLEMENTATION

### 4.1 Recommended Architecture: Hybrid A+B with Graceful Degradation

**Primary path:** `IPolicyConfig`/`IAudioPolicyConfigFactory` for direct 1-to-1 routing (low latency, DRM-compatible).

**Fallback path:** WASAPI loopback capture + render when:
- The COM interface is unavailable (older Windows, broken builds)
- 1-to-N duplication is requested
- The target session is DRM-protected and direct routing fails

**Why this hybrid:**
- No driver signing burden (unlike Approach C)
- Handles 95% of use cases with near-zero latency via direct routing
- Gracefully handles the 5% edge cases (duplication, DRM fallback, OS compatibility)
- Can be deployed as a standard desktop application (no installer with kernel drivers)

### 4.2 Session Discovery & Process Mapping

**The fundamental challenge:** Windows audio sessions don't map 1:1 to processes. A single application (Chrome, Discord, Electron apps) may have:
- Multiple sessions (one per tab, one per audio stream)
- Sessions that persist after the process exits ("ghost sessions")
- Child processes that share sessions with parents
- Sessions that migrate between processes

**Discovery architecture:**

```
┌─────────────────────────────────────────────────────┐
│                   AuDuo Audio Engine                 │
├─────────────────────────────────────────────────────┤
│                                                     │
│  ┌──────────────┐    ┌──────────────────────────┐  │
│  │ Process       │    │ Session                   │  │
│  │ Monitor       │◄──►│ Manager                   │  │
│  │ (WMI +        │    │ (IAudioSessionEnumerator) │  │
│  │  Toolhelp32)  │    │ + IAudioSessionEvents     │  │
│  └──────┬───────┘    └──────────┬───────────────┘  │
│         │                       │                   │
│         ▼                       ▼                   │
│  ┌──────────────────────────────────────────┐       │
│  │         Session-Process Resolver         │       │
│  │  • Maps session → process(es)            │       │
│  │  • Handles PID recycling                 │       │
│  │  • Identifies "app identity" (exe path)  │       │
│  │  • Groups child sessions to parent app   │       │
│  └──────────────────────────────────────────┘       │
│                                                     │
│  ┌──────────────┐    ┌──────────────────────────┐  │
│  │ Device        │    │ Routing                  │  │
│  │ Enumerator    │◄──►│ Controller               │  │
│  │ (IMMDevice)   │    │ (IPolicyConfig +         │  │
│  │               │    │  WASAPI fallback)        │  │
│  └──────────────┘    └──────────────────────────┘  │
└─────────────────────────────────────────────────────┘
```

**Process enumeration strategy:**
1. Use `IAudioSessionEnumerator` to get all active sessions
2. For each session, call `IAudioSessionControl2::GetProcessId` to get the owning PID
3. Use `Toolhelp32Snapshot` (or `QueryFullProcessImageName`) to resolve PID → executable path
4. Maintain a **session registry** that maps `{session-id} → {pid, exe-path, display-name, icon}`

**Handling multi-process apps:**
- **Chrome:** Each renderer process may have its own session. Group by the **parent Chrome process** (use `PROCESSENTRY32::th32ParentProcessID` recursively until you reach the main browser process).
- **Discord:** Electron-based — similar to Chrome. The main process owns the audio; child processes are typically silent.
- **Grouping heuristic:** Sessions sharing the same `exe-path` directory and having parent-child relationship are grouped under a single "application" entry in the UI.

### 4.3 Edge Case Handling

**PID Recycling:**
```cpp
// Problem: PID 1234 was Spotify, exited, and PID 1234 is now Notepad
// Solution: Correlate by (session-id, exe-path, creation-time)
struct SessionIdentity {
    std::wstring sessionId;      // From IAudioSessionControl::GetSessionIdentifier
    std::wstring exePath;        // Full path to executable
    uint32_t    processId;       // Current PID
    uint64_t    creationTime;    // Process creation time (FILETIME)
    uint64_t    lastSeenTick;    // For ghost detection
};
```
When a session's PID changes but the `session-id` remains the same, it's a ghost. When the `session-id` is new but the `exe-path` matches a recently-seen session, it's a restart. Use a **time-windowed identity cache** (5-minute sliding window) to detect recycling.

**App Restarts:**
- `IAudioSessionEvents::OnStateChanged` fires when a session transitions to `AudioSessionStateActive`
- `IAudioSessionEvents::OnSessionDisconnected` fires with `SessionDisconnectReasonServerShutdown` or `SessionDisconnectReasonSessionExpired`
- Maintain a **routing policy map** keyed by `exe-path` (not session-id). When a new session appears for a known `exe-path`, re-apply the routing.

**Target Device Disconnection:**
- Register for `IMMNotificationClient::OnDefaultDeviceChanged` and `OnDeviceStateChanged`
- When a routed device is disconnected:
  1. Mark all sessions routed to that device as **"unrouted"**
  2. Attempt to fall back to the system default device
  3. Queue a **re-routing attempt** when the device reconnects (`OnDeviceStateChanged` with `DEVICE_STATE_ACTIVE`)
  4. Show a notification to the user

**Ghost Sessions:**
- Sessions can persist in the audio engine for 30-60 seconds after the process exits
- Detect via: `IAudioSessionControl2::IsSystemSoundsSession()`, `GetState()` returning `AudioSessionStateExpired`, or PID no longer valid
- Implement a **session garbage collector** that runs every 5 seconds, checking for sessions whose PID is no longer in the system process list

### 4.4 Core C++20 Class Architecture

```cpp
// ============================================================
// CORE INTERFACES
// ============================================================

// Forward declarations
class IAudioDevice;
class IAudioSession;
class IRoutingPolicy;
class IAudioCaptureEngine;
class IAudioRenderEngine;

// ============================================================
// AUDIO DEVICE ABSTRACTION
// ============================================================

class AudioDevice {
public:
    struct Info {
        std::wstring id;           // IMMDevice::GetId()
        std::wstring friendlyName; // IMMDevice::GetFriendlyName()
        std::wstring interfaceName;// e.g., "{0.0.0.00000000}.{a4a78127-...}"
        AudioDeviceState state;
        DataFlow dataFlow;         // Render, Capture, All
        DeviceRole role;           // Console, Multimedia, Communications
        uint32_t channels;
        uint32_t sampleRate;
        uint32_t bitsPerSample;
    };

    [[nodiscard]] std::future<void> InitializeAsync() const;
    [[nodiscard]] const Info& GetInfo() const noexcept { return m_info; }

private:
    Info m_info;
    wil::com_ptr<IMMDevice> m_device;
    wil::com_ptr::IAudioClient> m_audioClient;
};

// ============================================================
// AUDIO SESSION ABSTRACTION
// ============================================================

class AudioSession {
public:
    struct Identity {
        std::wstring sessionId;       // IAudioSessionControl::GetSessionIdentifier()
        std::wstring instanceId;      // IAudioSessionControl::GetSessionInstanceIdentifier()
        uint32_t     processId;
        std::wstring exePath;
        std::wstring displayName;
        std::wstring appIconPath;
        uint64_t     processCreationTime;
        uint64_t     lastActivityTick;
    };

    struct State {
        AudioSessionState state;      // Active, Inactive, Expired
        float            peakValue;   // 0.0 - 1.0 (from IAudioMeterInformation)
        uint32_t         channelCount;
    };

    // Lifecycle
    [[nodiscard]] std::future<void> InitializeAsync(
        wil::com_ptr<IAudioSessionControl> pControl
    ) const;

    // Routing
    [[nodiscard]] std::future<RoutingResult> SetTargetDevice(
        const AudioDevice& device,
        RoutingMethod method  // Direct (IPolicyConfig) or WASAPI fallback
    ) const;

    [[nodiscard]] std::future<std::optional<AudioDevice>> GetCurrentDevice() const;

    // Monitoring
    using StateCallback = std::function<void(const AudioSession&, const State&)>;
    [[nodiscard]] winrt::event_token RegisterStateCallback(StateCallback cb);

private:
    Identity m_identity;
    State    m_currentState;

    wil::com_ptr<IAudioSessionControl>    m_sessionControl;
    wil::com_ptr<IAudioSessionControl2>   m_sessionControl2;
    wil::com_ptr<IAudioMeterInformation>  m_meterInfo;
    wil::com_ptr<IAudioSessionEvents>     m_eventSink;

    // WASAPI fallback members (only active when using loopback routing)
    std::unique_ptr<IAudioCaptureEngine>  m_captureEngine;
    std::unique_ptr<IAudioRenderEngine>   m_renderEngine;
    std::atomic<bool>                     m_isUsingFallback{false};
};

// ============================================================
// SESSION MANAGER (Singleton)
// ============================================================

class AudioSessionManager {
public:
    struct SessionMap {
        std::unordered_map<std::wstring, std::unique_ptr<AudioSession>> bySessionId;
        std::unordered_map<uint32_t, std::vector<AudioSession*>> byProcessId;
        std::unordered_map<std::wstring, std::vector<AudioSession*>> byExePath;
    };

    // Singleton access
    static AudioSessionManager& Instance();

    // Lifecycle
    [[nodiscard]] std::future<void> InitializeAsync() const;
    void Shutdown();

    // Query
    [[nodiscard]] const SessionMap& GetSessions() const { return m_sessions; }
    [[nodiscard]] std::vector<AudioSession*> GetSessionsForProcess(uint32_t pid) const;
    [[nodiscard]] std::vector<AudioSession*> GetSessionsForApp(const std::wstring& exePath) const;

    // Routing policy
    void SetRoutingPolicy(const std::wstring& exePath, const AudioDevice& device);
    void ClearRoutingPolicy(const std::wstring& exePath);
    [[nodiscard]] std::optional<AudioDevice> GetRoutedDevice(const AudioSession& session) const;

    // Notifications
    using SessionAddedCallback    = std::function<void(AudioSession&)>;
    using SessionRemovedCallback  = std::function<void(const AudioSession::Identity&)>;
    using SessionChangedCallback  = std::function<void(AudioSession&, const AudioSession::State&)>;

    [[nodiscard]] winrt::event_token OnSessionAdded(SessionAddedCallback cb);
    [[nodiscard]] winrt::event_token OnSessionRemoved(SessionRemovedCallback cb);
    [[nodiscard]] winrt::event_token OnSessionChanged(SessionChangedCallback cb);

private:
    AudioSessionManager() = default;
    std::future<void> RefreshSessions();
    std::future<void> GarbageCollectGhostSessions();

    wil::com_ptr<IMMDeviceEnumerator>        m_deviceEnumerator;
    wil::com_ptr<IAudioSessionManager2>      m_sessionManager;
    wil::com_ptr<IAudioSessionNotification>  m_notificationSink;

    SessionMap m_sessions;
    std::unordered_map<std::wstring, AudioDevice> m_routingPolicies; // exe-path → device

    // Ghost detection
    std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> m_lastSeen;
    std::jthread m_gcThread;
    std::atomic<bool> m_gcRunning{false};
};

// ============================================================
// ROUTING CONTROLLER
// ============================================================

class AudioRoutingController {
public:
    enum class RoutingMethod : uint8_t {
        Direct,        // IPolicyConfig (preferred)
        WASAPILoopback,// Capture + re-render (fallback)
        VirtualDevice  // Via virtual audio device (1-to-N)
    };

    struct RoutingResult {
        bool success;
        RoutingMethod methodUsed;
        std::string errorMessage; // UTF-8 for logging
    };

    // Route a single session to a device
    [[nodiscard]] std::future<RoutingResult> RouteSession(
        AudioSession& session,
        const AudioDevice& targetDevice
    ) const;

    // Route all sessions for an application (by exe path)
    [[nodiscard]] std::future<std::vector<RoutingResult>> RouteApp(
        const std::wstring& exePath,
        const AudioDevice& targetDevice
    ) const;

    // Split one session to multiple devices (1-to-N)
    [[nodiscard]] std::future<std::vector<RoutingResult>> SplitSession(
        AudioSession& session,
        std::span<const AudioDevice* const> targetDevices
    ) const;

    // Restore session to system default
    [[nodiscard]] std::future<RoutingResult> UnrouteSession(AudioSession& session) const;

    // Query current routing
    [[nodiscard]] std::future<std::vector<std::pair<AudioSession*, AudioDevice>>> 
    GetCurrentRouting() const;

private:
    [[nodiscard]] std::future<RoutingResult> RouteDirect(
        const AudioSession& session,
        const AudioDevice& device
    ) const;

    [[nodiscard]] std::future<RoutingResult> RouteWASAPIFallback(
        AudioSession& session,
        const AudioDevice& device
    ) const;

    // IPolicyConfig wrapper (semi-documented)
    class PolicyConfigClient {
    public:
        [[nodiscard]] std::future<HRESULT> SetSessionEndpoint(
            LPCWSTR sessionId,
            LPCWSTR endpointId
        ) const;

        [[nodiscard]] std::future<std::optional<std::wstring>> GetSessionEndpoint(
            LPCWSTR sessionId
        ) const;

    private:
        // CLSID/Interface obtained at runtime from the system
        // to avoid hardcoding undocumented IIDs
        wil::com_ptr<IUnknown> m_policyConfig;
    };

    mutable PolicyConfigClient m_policyConfig;
};

// ============================================================
// AUDIO CAPTURE ENGINE (WASAPI Loopback)
// ============================================================

class AudioCaptureEngine {
public:
    struct Config {
        uint32_t deviceId;          // Target device for loopback
        std::optional<std::wstring> sessionId; // nullptr = all sessions
        uint32_t bufferDurationMs;  // Typically 10-20ms
        AudioStreamCategory category;
    };

    [[nodiscard]] std::future<void> InitializeAsync(const Config& config);
    void Shutdown();

    // Callback-based buffer delivery
    using BufferCallback = std::function<void(
        const float* data,
        uint32_t frameCount,
        uint32_t channels,
        uint32_t sampleRate
    )>;

    [[nodiscard]] winrt::event_token RegisterBufferCallback(BufferCallback cb);
    void UnregisterCallback(winrt::event_token token);

    [[nodiscard]] uint32_t GetChannelCount() const { return m_channels; }
    [[nodiscard]] uint32_t GetSampleRate() const { return m_sampleRate; }
    [[nodiscard]] uint32_t GetBufferDurationMs() const { return m_bufferDurationMs; }

private:
    std::future<void> CaptureLoop();

    Config m_config;
    wil::com_ptr<IMMDevice> m_device;
    wil::com_ptr<IAudioClient> m_audioClient;
    wil::com_ptr<IAudioCaptureClient> m_captureClient;

    uint32_t m_channels{0};
    uint32_t m_sampleRate{0};
    uint32_t m_bufferDurationMs{10};
    std::jthread m_captureThread;
    std::atomic<bool> m_running{false};
    winrt::event<BufferCallback> m_bufferEvent;
};

// ============================================================
// AUDIO RENDER ENGINE (WASAPI Render)
// ============================================================

class AudioRenderEngine {
public:
    struct Config {
        std::wstring deviceId;
        uint32_t bufferDurationMs;
        AudioStreamCategory category;
        AudioClientStreamFlags flags; // AUDCLNT_STREAMFLAGS_EVENTCALLBACK
    };

    [[nodiscard]] std::future<void> InitializeAsync(const Config& config);
    void Shutdown();

    // Write audio data (blocking if buffer full)
    [[nodiscard]] std::future<void> WriteAsync(
        const float* data,
        uint32_t frameCount
    ) const;

    [[nodiscard]] uint32_t GetChannelCount() const { return m_channels; }
    [[nodiscard]] uint32_t GetSampleRate() const { return m_sampleRate; }

private:
    Config m_config;
    wil::com_ptr<IMMDevice> m_device;
    wil::com_ptr<IAudioClient> m_audioClient;
    wil::com_ptr<IAudioRenderClient> m_renderClient;
    wil::com_ptr<IAudioClock> m_clock;

    uint32_t m_channels{0};
    uint32_t m_sampleRate{0};
    uint32_t m_bufferFrameCount{0};
    std::jthread m_renderThread;
    std::atomic<bool> m_running{false};
};

// ============================================================
// DEVICE ENUMERATOR
// ============================================================

class AudioDeviceEnumerator {
public:
    [[nodiscard]] std::future<std::vector<AudioDevice>> EnumerateRenderDevices() const;
    [[nodiscard]] std::future<std::optional<AudioDevice>> GetDefaultRenderDevice() const;
    [[nodiscard]] std::future<std::optional<AudioDevice>> GetDeviceById(const std::wstring& id) const;
    [[nodiscard]] std::future<std::optional<AudioDevice>> GetDeviceCommunications() const; // For headset routing

    // Device change notifications
    using DeviceChangedCallback = std::function<void(
        AudioDevice device,
        DeviceEventType eventType  // Added, Removed, StateChanged, DefaultChanged
    )>;

    [[nodiscard]] winrt::event_token RegisterDeviceChangedCallback(DeviceChangedCallback cb);

private:
    class NotificationClient : public IMMNotificationClient {
        // Standard COM notification implementation
    };

    wil::com_ptr<IMMDeviceEnumerator> m_enumerator;
    wil::com_ptr<IMMNotificationClient> m_notificationClient;
    winrt::event<DeviceChangedCallback> m_deviceChangedEvent;
};

// ============================================================
// MAIN APPLICATION ORCHESTRATOR
// ============================================================

class AuDuoAudioEngine {
public:
    struct Config {
        bool enableDirectRouting{true};
        bool enableWASAPIFallback{true};
        bool enable1ToNDuplication{true};
        uint32_t ghostSessionTimeoutMs{60000};
        uint32_t sessionGcIntervalMs{5000};
    };

    [[nodiscard]] std::future<void> InitializeAsync(const Config& config);
    void Shutdown();

    // Public API
    [[nodiscard]] std::future<RoutingResult> RouteAppToDevice(
        const std::wstring& exePath,
        const std::wstring& deviceId
    ) const;

    [[nodiscard]] std::future<RoutingResult> SplitAppToDevices(
        const std::wstring& exePath,
        std::span<const std::wstring> deviceIds
    ) const;

    [[nodiscard]] std::future<RoutingResult> RestoreAppToDefault(
        const std::wstring& exePath
    ) const;

    [[nodiscard]] std::future<std::vector<AudioDevice>> GetAvailableDevices() const;
    [[nodiscard]] std::future<std::vector<AudioSession>> GetActiveSessions() const;

    // UI-facing state
    struct AppRoutingInfo {
        std::wstring     exePath;
        std::wstring     displayName;
        std::wstring     iconPath;
        std::wstring     routedDeviceId;
        std::wstring     routedDeviceName;
        RoutingMethod    routingMethod;
        bool             isDuplicated;
        float            peakLevel;
    };

    [[nodiscard]] std::future<std::vector<AppRoutingInfo>> GetAppRoutingState() const;

private:
    Config m_config;

    std::unique_ptr<AudioDeviceEnumerator>    m_deviceEnumerator;
    std::unique_ptr<AudioSessionManager>      m_sessionManager;
    std::unique_ptr<AudioRoutingController>   m_routingController;

    // Policy persistence
    std::future<void> PersistRoutingPolicies() const;
    std::future<void> LoadRoutingPolicies() const;

    // Routing policy store (exe-path → device-id)
    std::unordered_map<std::wstring, std::wstring> m_routingPolicies;
    std::unordered_map<std::wstring, std::vector<std::wstring>> m_splitPolicies;
};
```

### 4.5 Key Implementation Details

**IPolicyConfig Resolution (Runtime Discovery):**
Since the interface is semi-documented, resolve it at runtime to avoid hardcoding IIDs that may change:

```cpp
// In PolicyConfigClient::PolicyConfigClient()
// Try IAudioPolicyConfigFactory first (Win10 1803+)
const CLSID CLSID_AudioPolicyConfigFactory = 
    /* resolved from registry or known value */;
const IID IID_IAudioPolicyConfigFactory = 
    /* resolved similarly */;

HRESULT hr = CoCreateInstance(
    CLSID_AudioPolicyConfigFactory,
    nullptr,
    CL