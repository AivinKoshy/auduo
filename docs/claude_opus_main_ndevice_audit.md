# AuDuo N-Device Canonical Engine Audit (main branch)

**Model:** `claude-opus-5-5`  
**Branch Audited:** `main`  
**Tokens:** {'prompt_tokens': 3, 'completion_tokens': 8192, 'total_tokens': 8195}

---

# Engineering Review: AuDuo N-Device WASAPI Audio Engine

This is a thorough, unvarnished review. The codebase demonstrates real expertise — the silent keep-alive trick, the 4-tier initialization fallback, and the orthogonal chirp design are all production-caliber ideas. But there are several structural bugs that will cause crashes, audio leaks, and silent failures in the field.

---

## 1. ARCHITECTURAL CRITIQUE OF THE N-DEVICE SYSTEM

### What's Working Well

The per-pipeline WASAPI client model is architecturally sound. Each `AudioPipeline` owns its own `IAudioClient`, `IAudioRenderClient`, render thread, and `RingBuffer` — this is the correct isolation boundary. The SPSC `RingBuffer` between the capture-side producer and each render-side consumer is the right primitive. The N-device extension via `m_extraPipelines` follows the same pattern as A/B, which is consistent.

### Structural Flaws

**1.1 `GetAllPipelines()` allocates on every call — and it's called from hot paths**

```cpp
// Called from: SetMasterVolume, PlayTestBeep, MetronomeLoop, StartStreaming, UpdateWindowsVolumeSync
std::vector<std::shared_ptr<AudioPipeline>> AudioEngine::GetAllPipelines() const {
    std::vector<std::shared_ptr<AudioPipeline>> list; // heap allocation every call
    if (m_pipelineA) list.push_back(m_pipelineA);
    if (m_pipelineB) list.push_back(m_pipelineB);
    {
        std::lock_guard<std::mutex> lock(m_extraPipelinesMutex);
        for (const auto& pipe : m_extraPipelines) {
            if (pipe) list.push_back(pipe);
        }
    }
    return list;
}
```

`SetMasterVolume()` calls this and then iterates the copy to set volume on each pipeline. During active streaming, volume changes from the UI thread cause a heap allocation + vector copy + mutex hold on every slider tick. For N pipelines this is O(N) allocation per call. The fix is a non-allocating iteration pattern:

```cpp
// Replace GetAllPipelines() in hot paths with:
void AudioEngine::ForEachPipeline(std::function<void(std::shared_ptr<AudioPipeline>)> fn) const {
    if (m_pipelineA) fn(m_pipelineA);
    if (m_pipelineB) fn(m_pipelineB);
    std::lock_guard<std::mutex> lock(m_extraPipelinesMutex);
    for (const auto& pipe : m_extraPipelines) {
        if (pipe) fn(pipe);
    }
}

// Then SetMasterVolume becomes:
void AudioEngine::SetMasterVolume(float vol) {
    // ... clamping logic ...
    float effective = m_isMuted.load(std::memory_order_relaxed) ? 0.0f : vol;
    ForEachPipeline([effective](auto p) { p->SetMasterVolume(effective); });
    // ... Windows volume sync ...
}
```

**1.2 No backpressure — capture loop is O(N) blocking on every frame**

In `CaptureLoop()`, every captured packet is pushed to every pipeline sequentially:

```cpp
if (m_pipelineA && m_pipelineA->IsRunning())
    m_pipelineA->PushAudio(floatConversionBuffer.data(), numFramesAvailable, sampleRate, channels);
if (m_pipelineB && m_pipelineB->IsRunning())
    m_pipelineB->PushAudio(...);
// ... then all extra pipelines, each under a separate mutex lock
```

Each `PushAudio` locks `m_pushMutex`, potentially resamples (heap allocates), and writes to the ring buffer. If any pipeline's render thread is in a `SetDelayMs` or `ClearBuffer` call (which also locks `m_pushMutex`), the **entire capture loop stalls**. With N pipelines, this is N serialized mutex acquisitions per capture packet. At 44.1kHz with 128-frame packets, that's ~344 lock acquisitions per second per pipeline, all serialized.

The `m_pushMutex` is also unnecessary for the SPSC ring buffer itself — it only exists to serialize multiple producers. But the capture loop is the primary producer, and the metronome/calibration are transient. A better design: make `PushAudio` lock-free for the common case (single producer) and use a `std::shared_mutex` or per-pipeline spinlock only for the secondary producers.

**1.3 No pipeline removal during streaming**

`UnregisterPipeline()` removes from `m_extraPipelines`, but if that pipeline's render thread is still running (because `Shutdown()` hasn't been called), the thread continues running with a dangling `this` pointer risk if the `shared_ptr` is the only reference. The method should require the caller to shut down the pipeline first, or the engine should shut it down:

```cpp
void AudioEngine::UnregisterPipeline(std::shared_ptr<AudioPipeline> pipeline) {
    if (!pipeline) return;
    pipeline->Shutdown(); // Ensure thread is stopped before removal
    std::lock_guard<std::mutex> lock(m_extraPipelinesMutex);
    m_extraPipelines.erase(
        std::remove(m_extraPipelines.begin(), m_extraPipelines.end(), pipeline),
        m_extraPipelines.end()
    );
}
```

**1.4 Fixed ring buffer capacity with no runtime adaptation**

The ring buffer is constructed with a fixed capacity (default 4 seconds at 48kHz). For N pipelines each with a 4-second buffer, that's N × 4s × 2 channels × 4 bytes = 32N MB of committed memory. For 8 pipelines, that's 256MB of ring buffer memory that's always allocated. There's no mechanism to reduce capacity for idle pipelines or to grow if the delay setting exceeds capacity.

---

## 2. CONCURRENCY & RACE CONDITIONS

### 2.1 RingBuffer::Clear() vs Read() Race Condition

This is the most dangerous concurrency bug in the codebase.

```cpp
// Render thread (lock-free, no mutex):
size_t Read(float* destination, size_t frames) {
    const size_t writeIdx = m_writeIndex.load(std::memory_order_acquire);
    const size_t readIdx = m_readIndex.load(std::memory_order_relaxed); // <-- relaxed!
    const size_t availableFrames = (writeIdx >= readIdx) ? (writeIdx - readIdx) : 0;
    // ... reads data based on readIdx ...
}

// UI/worker thread (via ClearBuffer):
void Clear() {
    const size_t writeIdx = m_writeIndex.load(std::memory_order_relaxed);
    m_readIndex.store(writeIdx, std::memory_order_release);
}
```

The race: `Read()` loads `writeIdx` with acquire, then `readIdx` with **relaxed**. Between these two loads, `Clear()` can execute, setting `readIdx = writeIdx`. But `Read()` already captured the old `writeIdx`, so:

```
Read(): writeIdx = W_old (acquire)
Clear():  readIdx  = W_new (release)  // W_new > W_old because Write advanced it
Read(): readIdx = W_new (relaxed, sees Clear's store)
available = W_new - W_old  // reads data that was supposed