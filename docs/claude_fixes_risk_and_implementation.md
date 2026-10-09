# AuDuo — Risk Analysis & Implementation Blueprint

---

## 1. RISK ANALYSIS & SIDE EFFECTS

### Fix 1 — Double-Shutdown Idempotency & Thread Joining

| Risk | Detail | Mitigation |
|---|---|---|
| **Double `CoUninitialize()`** | `CoUninitialize()` asserts/AVs if called more times than `CoInitialize()`. Track a per-thread init count atomically. | Wrap in `ComLifetime` RAII with ref-count; assert balance. |
| **Deadlock on re-entrant Shutdown** | If a WASAPI callback triggers Shutdown while the main thread holds a lock. | Shutdown path is *lock-free* except for the CAS on state; signal thread events first, join after. |
| **`TerminateThread` resource leak** | Forced termination doesn't run C++ destructors (leaks COM refs, locks). | Only reach for it after a timeout as absolute last resort; log loudly. |
| **Double `IAudioClient::Stop()`** | Safe — it's idempotent per MSDN. No action needed. | Call unconditionally; check `S_OK`/`S_FALSE` silently. |
| **Event handle leak on partial init** | If the constructor throws after creating one event but not the other. | Use RAII wrappers around handles from construction. |

**Key WASAPI quirk:** `IAudioClient::Stop()` is synchronous but signals the event handle *asynchronously* on a system timer. If you signal the shutdown event immediately after, a thread in `WaitForSingleObject` may return due to the Stop signal, see the shutdown flag, and exit cleanly — but there's a race window of one kernel dispatch where the thread re-enters `WaitForSingleObject` before seeing the shutdown flag. Solution: check the flag *after* every wake, and also use a timeout in the wait.

---

### Fix 2 — Thread-Safe Delay Frame Adjustments

| Risk | Detail | Mitigation |
|---|---|---|
| **Torn reads of delay value** | 64-bit value on 32-bit Windows reads as two 32-bit halves. | Use `std::atomic<uint32_t>` for delay frames; never pass raw `size_t`. |
| **Buffer overrun if delay is reduced mid-transfer** | Render thread calculates write position from a stale/overwritten pointer. | Delay buffer is *always* a fixed-capacity ring; `m_delayFrames` only controls the read pointer offset, not the allocation. |
| **Click on parameter swap** | Non-atomic swap of buffer pointers while render thread is reading. | Use `std::atomic` swap of a `BufferState` struct — the whole struct copies atomically via `exchange`. |
| **Priority inversion** | GUI thread holds a mutex while render thread spins on it. | GUI writes to a *staging* copy, then one atomic `exchange`; zero lock contention in render path. |

**Key WASAPI quirk:** `IAudioRenderClient::GetBuffer()` returns a pointer valid until the next `ReleaseBuffer()` or `Stop()`. The render thread **must not** hold any lock across that call — the lock must be acquired *before* `GetBuffer` and released *before* `ReleaseBuffer`. Even better: don't hold any lock at all; use atomics for all shared state.

---

### Fix 3 — StartStreaming Rollback on Partial Device Failure

| Risk | Detail | Mitigation |
|---|---|---|
| **COM object leak in rollback** | Capture starts OK, render fails; capture client is leaked. | Use RAII holders; rollback just calls `Reset()` on holders that succeeded. |
| **State desync between engine and UI** | UI shows "Running" but engine is in a half-initialized state. | Engine state is the source of truth; UI observes via a callback after rollback completes. |
| **`CoCreateInstance` failure leaves zombie threads** | Rollback doesn't join threads that were already started. | Start threads *last*, after all WASAPI resources are validated. |
| **Rollback itself can throw/fail** | If `Stop()` on a partially-started stream fails, rollback crashes. | Catch all `HRESULT` errors in rollback; continue cleaning regardless. |
| **Mixed-mode format mismatch** | Capture got format A, render got format B — rollback doesn't reset format state. | Store the agreed format in the engine; rollback resets it. |

**Key WASAPI quirk:** `IAudioClient::Initialize()` in shared mode *negotiates* the format. If you call it twice (once for capture, once for render) on the same `IAudioClient`, the second call can return `AUDCLNT_E_ALREADY_INITIALIZED`. Use separate `IAudioClient` instances — one per stream — which is the standard pattern. If rollback must re-initialize the same client, call `Reset()` first (which releases the buffer).

---

### Fix 4 — Process Loopback Exclusion for Self-Capture Prevention

| Risk | Detail | Mitigation |
|---|---|---|
| **`ActivateAudioInterfaceAsync` requires restricted capability** | `PSF_AUDIOENGINE_PROCESS_LOOPBACK` requires `systemProtected` capability in appxmanifest. Desktop apps can't use this. | Use `IAudioSessionManager2` enumeration instead — works for desktop (no manifest needed). |
| **Session PID not available immediately** | `IAudioSessionControl::GetSessionIdentifier` returns `nullptr` before the session fully registers. | Retry with a short backoff (50ms × 3); if still missing, include the session and accept a brief self-capture. |
| **64-bit PID truncation** | `DWORD` PID from `GetCurrentProcessId()` is fine, but `IAudioSessionControl2::GetProcessId` returns `DWORD`. No truncation risk. | No issue — both are 32-bit. |
| **Session disappears during enumeration** | Another thread enumerates simultaneously and releases session references. | Enumerate under `IMMDeviceEnumerator` lock; session references are AddRef'd by the enumerator. |
| **Performance: session enumeration on every capture start** | Negligible — enumeration is fast, and it's only at StartStreaming time, not per-buffer. | No concern. |

**Key WASAPI quirk:** On Windows 7, `IAudioSessionManager2` exists but `IAudioSessionControl2` may not expose `GetProcessId` for certain sessions (system sounds). Always check the interface with `QueryInterface` before calling.

---

### Fix 5 — Capture Loop CPU Spin Mitigation During Silent Packets

| Risk | Detail | Mitigation |
|---|---|---|
| **Sleep() causes packet drop** | Sleeping between `GetNextPacketSize` polls means missing the event signal → buffer overrun. | Never sleep *before* checking the event. Sleep *only after* confirming no data, and use the event handle in the wait, not a blind `Sleep()`. |
| **Spurious wakeups** | The capture event can fire even with zero data (format change, device removal). | Always check `GetNextPacketSize()` after every wake; return to wait if zero. |
| **Timeout too long → overrun** | A 100ms wait on silence means a 100ms gap in the render pipeline if render is running. | Use a short timeout (5-10ms) for the wait; loop returns quickly when data arrives. |
| **WaitForSingleObject on `hnsBufferDuration` granularity** | WASAPI fires the event at buffer-period intervals regardless of data. | This is expected behavior; the wait is the correct pattern. The 10ms buffer period maps to ~10ms latency, which is acceptable. |
| **Premature buffer release** | Calling `ReleaseBuffer(0)` when there's no data crashes the audio stack. | Never call `ReleaseBuffer` with a count not returned by `GetBuffer`. Zero-data path calls `ReleaseBuffer(0)` which IS valid — but verify with `GetBuffer` first. |

**Key WASAPI quirk:** In shared mode, the capture buffer event fires at the system's buffer-period rate (10ms default), not when data is available. In exclusive mode, it fires only when the buffer has data. For loopback (shared mode), the event always fires at the buffer period, and `GetNextPacketSize` tells you whether that wake had data. The correct pattern is: wait on event → check `GetNextPacketSize` → if 0, loop back to wait.

---

### Fix 6 — Test Beep / Calibration Pipeline Resource Cleanup

| Risk | Detail | Mitigation |
|---|---|---|
| **Beep thread orphaned if init fails mid-way** | `m_beepThread` created but never joined on exception. | Use `std::jthread` (C++20) — destructor auto-joins. Or use a scoped `ThreadJoiner` RAII wrapper. |
| **Render client not stopped** | Beep stream holds the audio client; normal shutdown deadlocks. | Beep stream uses a *separate* `IAudioClient` instance. Stopping it is independent. |
| **Audio format mismatch** | Beep uses 48000 Hz stereo, but the device negotiated 44100 Hz → `Initialize` fails silently. | Query device format first, resample or reject. |
| **Beep callback blocks on a full buffer** | `IAudioRenderClient::GetBuffer` blocks if the render buffer is full (rare but possible). | The beep thread writes to its own dedicated ring buffer; the WASAPI callback reads from it non-blocking. |
| **Cleanup race: beep still writing while engine releases COM objects** | `IAudioClient::Stop()` unblocks the render thread; thread must drain before COM objects are released. | Stop → join thread → release COM objects. Strict ordering. |

**Key WASAPI quirk:** You can't share an `IAudioClient` between two independent streams (beep + normal audio). Each needs its own `IAudioClient` with its own `IAudioRenderClient`. Starting the beep stream while the main render stream is active can cause buffer underruns on the main stream if both share the same endpoint. Use a separate `IMMDevice` or accept the latency — for a beep, latency doesn't matter.

---

## 2. CONCRETE IMPLEMENTATION BLUEPRINT

```cpp
// ============================================================================
// AuDuo Audio Engine — Fix Implementation Blueprint (C++20 / Windows SDK)
// ============================================================================

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <atomic>
#include <thread>
#include <vector>
#include <span>
#include <expected>
#include <compare>
#include <cstdint>
#include <cassert>

// ---------------------------------------------------------------------------
// Fix 1 — Shared Utilities: RAII, Error Handling, Atomic State
// ---------------------------------------------------------------------------

// C++20 std::expected for structured error returns (requires <expected> header)
// Fallback: define simple variant if <expected> unavailable on your toolchain.
#ifndef __cpp_lib_expected
template<typename T, typename E>
using expected = std::variant<T, E>;
template<typename T, typename E>
constexpr bool has_value(const expected<T,E>& v) { return std::holds_alternative<T>(v); }
template<typename T, typename E>
constexpr T& value(expected<T,E>& v) { return std::get<T>(v); }
template<typename T, typename E>
constexpr E& error(expected<T,E>& v) { return std::get<E>(v); }
#else
using std::expected;
using std::unexpected;
#endif

enum class Hresult : uint32_t { Value = 0 }; // tag type for HRESULT errors

class WinError {
public:
    HRESULT code;
    constexpr WinError(HRESULT c) : code(c) {}
    constexpr operator HRESULT() const { return code; }
};

template<typename T>
using WinResult = expected<T, WinError>;

constexpr WinResult<void> WinOk() { /* return void success */ }

#define WIN_TRY(expr)                                           \
    do {                                                        \
        HRESULT _hr_ = (expr);                                  \
        if (FAILED(_hr_)) return unexpected(WinError(_hr_));   \
    } while(0)

// ---------------------------------------------------------------------------
// Fix 1 — RAII wrappers (eliminates manual Release/CloseHandle)
// ---------------------------------------------------------------------------

struct ComPtrBase {
    template<typename T>
    static T* AddRef(T* p) { if(p) p->AddRef(); return p; }
};

// Minimal ComPtr — use Microsoft::WRL::ComPtr in production
template<typename T>
class ComPtr {
    T* ptr_ = nullptr;
public:
    ComPtr() = default;
    explicit ComPtr(T* p) : ptr_(p) {}
    ComPtr(const ComPtr& o) : ptr_(ComPtrBase::AddRef(o.ptr_)) {}
    ComPtr(ComPtr&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }
    ComPtr& operator=(const ComPtr& o) { reset(); ptr_ = ComPtrBase::AddRef(o.ptr_); return *this; }
    ComPtr& operator=(ComPtr&& o) noexcept { reset(); ptr_ = o.ptr_; o.ptr_ = nullptr; return *this; }
    ~ComPtr() { reset(); }

    T* get() const { return ptr_; }
    T* detach() { T* p = ptr_; ptr_ = nullptr; return p; }
    T** addressof() { reset(); return &ptr_; } // for out-param in QueryInterface
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    void reset(T* p = nullptr) noexcept {
        if (ptr_) ptr_->Release();
        ptr_ = p;
    }

    // C++20 three-way comparison for nullptr checks
    auto operator<=>(std::nullptr_t) const = default;
};

// RAII handle wrapper
class Handle {
    HANDLE h_ = nullptr;
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    ~Handle() { if (h_) CloseHandle(h_); }

    HANDLE get() const { return h_; }
    HANDLE release() { HANDLE h = h_; h_ = nullptr; return h; }

    void reset(HANDLE h = nullptr) { if (h_) CloseHandle(h_); h_ = h; }
    explicit operator bool() const { return h_ != nullptr; }
};

// COM apartment guard — tracks CoInitialize/CoUninitialize balance per thread
class ComApartment {
    bool initialized_ = false;
public:
    ComApartment(DWORD coInit = COINIT_MULTITHREADED) {
        HRESULT hr = CoInitializeEx(nullptr, coInit);
        if (SUCCEEDED(hr)) initialized_ = true;
        else if (hr != RPC_E_CHANGED_MODE) assert(!"CoInitializeEx failed");
    }
    ~ComApartment() { if (initialized_) CoUninitialize(); }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
};

// ---------------------------------------------------------------------------
// Fix 1 — Engine State Machine & Idempotent Shutdown
// ---------------------------------------------------------------------------

enum class EngineState : uint8_t { Uninitialized, Running, ShuttingDown, Terminated };

class AudioEngine {
    // --- State ---
    std::atomic<EngineState> m_state{EngineState::Uninitialized};

    // --- COM Resources (RAII) ---
    ComPtr<IMMDeviceEnumerator> m_deviceEnumerator;
    ComPtr<IMMDevice>            m_renderDevice;
    ComPtr<IMMDevice>            m_captureDevice;
    ComPtr<IAudioClient>         m_audioClient;       // for render
    ComPtr<IAudioClient>         m_captureAudioClient; // separate instance
    ComPtr<IAudioRenderClient>   m_renderClient;
    ComPtr<IAudioCaptureClient>  m_captureClient;

    // --- Threading ---
    std::jthread   m_renderThread;   // C++20 jthread auto-joins on destruction
    std::jthread   m_captureThread;
    Handle         m_renderEvent;    // signaled when buffer needs fill
    Handle         m_captureEvent;   // signaled when capture data available
    std::atomic<bool> m_renderShutdown{false};
    std::atomic<bool> m_captureShutdown{false};

    // --- Format (negotiated, immutable after Initialize) ---
    WAVEFORMATEX*  m_mixFormat = nullptr; // owned by IAudioClient, don't free
    DWORD          m_renderBufferFrames = 0;
    DWORD          m_captureBufferFrames = 0;
    REFERENCE_TIME m_renderPeriod = 0; // hns (100ns units)
    REFERENCE_TIME m_capturePeriod = 0;

    // --- Fix 2: Delay frame parameters (lock-free, atomic) ---
    struct alignas(64) DelayParams { // cache-line aligned to prevent false sharing
        std::atomic<uint32_t> delayFrames{0};
        std::atomic<uint32_t> bufferCapacityFrames{4096};
    };
    alignas(64) DelayParams m_delay;

public:
    AudioEngine() = default;
    ~AudioEngine() { Shutdown(); } // idempotent — destructor always calls it

    // Non-copyable, non-movable (owns kernel objects and threads)
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // -----------------------------------------------------------------------
    // Fix 1 — Idempotent Shutdown (the key implementation)
    // -----------------------------------------------------------------------
    bool Shutdown() noexcept {
        // Try to transition: Running → ShuttingDown.
        // If already ShuttingDown or Terminated, another thread owns cleanup.
        EngineState expected = EngineState::Running;
        if (!m_state.compare_exchange_strong(
                expected, EngineState::ShuttingDown,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            // State was not Running — already handled or never started
            if (expected == EngineState::ShuttingDown) {
                WaitForTermination(); // assist: wait for the other thread's cleanup
            }
            return true; // idempotent success
        }

        // --- Phase 1: Stop all audio streams (signals buffer events) ---
        StopStreams();

        // --- Phase 2: Signal threads to exit ---
        m_renderShutdown.store(true, std::memory_order_release);
        m_captureShutdown.store(true, std::memory_order_release);
        SetEvent(m_renderEvent.get());  // unblock render wait
        SetEvent(m_captureEvent.get()); // unblock capture wait

        // --- Phase 3: Join threads ---
        // std::jthread::native_handle() returns HANDLE on Windows
        if (m_renderThread.joinable()) {
            m_renderThread.request_stop(); // C++20 jthread stop_token
            // WaitForSingleObject with timeout; jthread destructor handles final join
        }
        if (m_captureThread.joinable()) {
            m_captureThread.request_stop();
        }

        // --- Phase 4: Release COM resources (RAII via ComPtr::reset) ---
        m_renderClient.reset();
        m_captureClient.reset();
        m_audioClient.reset();
        m_captureAudioClient.reset();
        m_renderDevice.reset();
        m_captureDevice.reset();
        m_deviceEnumerator.reset();

        m_state.store(EngineState::Terminated, std::memory_order_release);
        return true;
    }

private:
    void StopStreams() noexcept {
        // IAudioClient::Stop() is idempotent — safe to call even if not started
        if (m_renderClient) {
            if (auto* ac = m_audioClient.get()) {
                ac->Stop(); // HRESULT ignored — already stopped is fine
            }
        }
        if (m_captureClient) {
            if (auto* ac = m_captureAudioClient.get()) {
                ac->Stop();
            }
        }
        // Reset resets the stream position and releases the buffer.
        // Call only if we successfully initialized.
        if (m_audioClient) m_audioClient->Reset();
        if (m_captureAudioClient) m_captureAudioClient->Reset();
    }

    void WaitForTermination() noexcept {
        // Another thread is doing shutdown — wait for its completion.
        // Poll the state; use a futex or event in production.
        while (m_state.load(std::memory_order_acquire) != EngineState::Terminated) {
            Sleep(10);
        }
    }
};


// ============================================================================
// Fix 2 — Thread-Safe Delay Frame Adjustment
// ============================================================================

// The delay line is a fixed-capacity ring buffer. m_delay.delayFrames (atomic)
// controls how many frames of silence are prepended before live audio.
// GUI writes to a staging copy → atomic exchange → render thread picks it up.
//
// Zero locks in the render path.

class DelayLine {
    std::vector<float> m_buffer;
    uint32_t           m_capacity{0};
    uint32_t           m_writePos{0};
    uint32_t           m_readPos{0};
    std::atomic<uint32_t> m_activeDelay{0}; // current delay in frames

    // Pre-computed silence prefix length per update
    std::atomic<uint32_t> m_silenceRemaining{0};

public:
    explicit DelayLine(uint32_t capacityFrames)
        : m_buffer(capacityFrames), m_capacity(capacityFrames) {}

    // Called by GUI thread — zero lock contention with render thread.
    // Writes to staging, then one atomic exchange.
    void SetDelayFrames(uint32_t newDelay) {
        // Clamp to capacity
        newDelay = (newDelay > m_capacity) ? m_capacity : newDelay;

        // Calculate how many *additional* silence frames to inject
        uint32_t current = m_activeDelay.load(std::memory_order_acquire);
        if (newDelay > current) {
            m_silenceRemaining.store(newDelay - current, std::memory_order_release);
        }
        m_activeDelay.store(newDelay, std::memory_order_release);
    }

    [[nodiscard]] uint32_t GetDelayFrames() const {
        return m_activeDelay.load(std::memory_order_acquire);
    }

    // Called by render thread — lock-free, just atomics + pointer math
    // Returns number of frames written into dst (always <= dst.size())
    // Automatically inserts silence for delay.
    uint32_t Write(std::span<float> dst, std::span<const float> src) {
        const uint32_t srcFrames = static_cast<uint32_t>(src.size());
        const uint32_t maxWrite = static_cast<uint32_t>(dst.size());
        const uint32_t toWrite  = (srcFrames < maxWrite) ? srcFrames : maxWrite;
        const uint32_t delay     = m_activeDelay.load(std::memory_order_acquire);

        // Conspend pre-computed silence first
        uint32_t silenceNeeded = m_silenceRemaining.exchange(0, std::memory_order_acq_rel);
        uint32_t silenceToWrite = (silenceNeeded < toWrite) ? silenceNeeded : toWrite;

        // Phase 1: Write silence prefix
        uint32_t written = 0;
        for (; written < silenceToWrite; ++written) {
            dst[written] = 0.0f;
        }

        // Phase 2: Write audio (prepend remaining silence if delay > silenceNeeded)
        for (; written < delay && written < toWrite; ++written) {
            dst[written] = 0.0f;
        }
        for (; written < toWrite; ++written) {
            dst[written] = src[written - delay]; // -delay wraps to 0 if delay > written
        }

        return written;
    }

    // Called by render thread when it's time to pull audio.
    // Advances read position through delay region, then returns live audio.
    uint32_t Read(std::span<float> dst, std::span<const float> liveSrc) {
        return Write(dst, liveSrc); // same logic from perspective of output
    }
};


// ============================================================================
// Fix 3 — StartStreaming with Rollback & UI State Sync
// ============================================================================

// Pattern: acquire resources in order, track success, rollback in reverse.
// UI state is only updated AFTER all resources succeed.

enum class StreamError {
    DeviceNotFound,
    FormatUnsupported,
    ClientInitFailed,
    RenderStreamFailed,
    CaptureStreamFailed,
    ThreadLaunchFailed,
};

class AudioEngine; // forward

class StartStreamingResult {
    WinResult<void> m_result;
   