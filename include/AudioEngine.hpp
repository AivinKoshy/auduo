#pragma once

#include "AudioDefines.hpp"
#include "RingBuffer.hpp"
#include <thread>
#include <mutex>
#include <functional>

namespace Auduo {

// Linear resampler / channel converter helper
class Resampler {
public:
    static void ResampleAndMix(
        const float* input, size_t inFrames, uint32_t inRate, uint32_t inChannels,
        std::vector<float>& output, uint32_t outRate, uint32_t outChannels
    );
};

// Represents one audio playback pipe (Output Device A or B)
class AudioPipeline {
public:
    AudioPipeline(const std::wstring& name, uint32_t bufferCapacityFrames = 48000 * 2);
    ~AudioPipeline();

    bool Initialize(const std::wstring& endpointDeviceId);
    void Shutdown();

    void PushAudio(const float* frames, size_t frameCount, uint32_t sampleRate, uint32_t channels);
    void UpdateVUMeterOnly(const float* frames, size_t frameCount, uint32_t channels);

    void SetVolume(float volume) { m_volume.store(volume, std::memory_order_relaxed); }
    float GetVolume() const { return m_volume.load(std::memory_order_relaxed); }

    void SetMasterVolume(float master) { m_masterVolume.store(master, std::memory_order_relaxed); }
    float GetMasterVolume() const { return m_masterVolume.load(std::memory_order_relaxed); }

    void SetDelayMs(int delayMs);
    int GetDelayMs() const { return m_delayMs.load(std::memory_order_relaxed); }
    void ClearBuffer() {
        std::lock_guard<std::mutex> lock(m_pushMutex);
        m_ringBuffer.Clear();
    }

    void SetAutoSync(bool enabled) { m_autoSync = enabled; }
    bool GetAutoSync() const { return m_autoSync; }

    float GetPeakVU() const { return m_metrics.peakVU.load(std::memory_order_relaxed); }
    bool IsRunning() const { return m_running.load(std::memory_order_relaxed); }
    bool HasDeviceError() const { return m_deviceError.load(std::memory_order_relaxed); }

    const std::wstring& GetDeviceId() const { return m_deviceId; }
    const std::wstring& GetName() const { return m_name; }

private:
    void RenderLoop();

    std::wstring m_name;
    std::wstring m_deviceId;
    RingBuffer m_ringBuffer;
    std::mutex m_pushMutex;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_deviceError{false};
    std::atomic<float> m_volume{1.0f};
    std::atomic<float> m_masterVolume{1.0f};
    std::atomic<int> m_delayMs{0};
    bool m_autoSync{false};

    StreamMetrics m_metrics;
    std::thread m_renderThread;

    IMMDevice* m_pDevice = nullptr;
    IAudioClient* m_pAudioClient = nullptr;
    IAudioRenderClient* m_pRenderClient = nullptr;
    HANDLE m_hRenderEvent = NULL;

    WAVEFORMATEX* m_pwfx = nullptr;
    uint32_t m_bufferFrameCount = 0;
    bool m_comInitialized{false};
};

// Master Audio Engine managing loopback capture and twin pipelines (A & B)
class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    // Query available audio render endpoints
    static std::vector<AudioDevice> EnumerateOutputDevices();

    // Query available audio capture/recording endpoints (microphones)
    static std::vector<AudioDevice> EnumerateInputDevices();

    // Query active desktop audio sessions/processes
    static std::vector<std::pair<DWORD, std::wstring>> EnumerateAudioProcesses();

    bool Initialize();
    void Shutdown();

    bool StartStreaming();
    void StopStreaming();
    bool IsStreaming() const { return m_isStreaming.load(std::memory_order_relaxed); }

    void SetMirrorSystemAudio(bool mirror) { m_mirrorSystemAudio = mirror; }
    bool GetMirrorSystemAudio() const { return m_mirrorSystemAudio; }

    void SetSmartAutoSync(bool enable) { m_smartAutoSync = enable; }
    bool GetSmartAutoSync() const { return m_smartAutoSync; }

    void SelectDeviceA(const std::wstring& deviceId);
    void SelectDeviceB(const std::wstring& deviceId);
    const std::wstring& GetDeviceIdA() const { return m_deviceIdA; }
    const std::wstring& GetDeviceIdB() const { return m_deviceIdB; }

    void SetProcessFilterA(DWORD pid) { m_pidFilterA = pid; }
    void SetProcessFilterB(DWORD pid) { m_pidFilterB = pid; }

    std::shared_ptr<AudioPipeline> GetPipelineA() { return m_pipelineA; }
    std::shared_ptr<AudioPipeline> GetPipelineB() { return m_pipelineB; }

    // Play high-precision synchronized test chime across both channels
    void PlayTestBeep();

    void SetPreventEchoOnDefault(bool enable) { m_preventEchoOnDefault = enable; }
    bool GetPreventEchoOnDefault() const { return m_preventEchoOnDefault; }
    static std::wstring GetDefaultDeviceId();
    bool IsVirtualCableDefault() const;
    bool IsVirtualCableInstalled() const;

    // Simultaneous Dual-Frequency Acoustic Calibration (Fires orthogonal chirps across A and B at once)
    void AutoSyncBothChannels(std::function<void(bool success, int deltaMs, const std::string& msg)> onComplete);

    // Two-Step Acoustic Mic Calibration (Single channel fallback)
    void CalibrateChannel(int channelIndex, std::function<void(bool success, int latencyMs, const std::string& msg)> onComplete);
    bool IsCalibrating() const { return m_isCalibrating.load(std::memory_order_relaxed); }
    std::string GetCalibrationStatus() const;

    // Interactive Auditory Sync Metronome Clapper
    void StartMetronome(int intervalMs = 500);
    void StopMetronome();
    bool IsMetronomeRunning() const { return m_metronomeRunning.load(std::memory_order_relaxed); }

    // Live Mic Level Monitor (peak VU 0.0 .. 1.0)
    void StartMicMonitor();
    void StopMicMonitor();
    void SelectMicDevice(const std::wstring& deviceId);
    const std::wstring& GetMicDeviceId() const { return m_micDeviceId; }
    float GetMicPeakVU() const { return m_micPeakVU.load(std::memory_order_relaxed); }
    void SetMicGain(float gain) { m_micGain.store(gain, std::memory_order_relaxed); }
    float GetMicGain() const { return m_micGain.load(std::memory_order_relaxed); }
    bool IsMicRawActive() const { return m_micRawActive.load(std::memory_order_relaxed); }
    bool IsMicMonitorRunning() const { return m_micMonitorRunning.load(std::memory_order_relaxed); }

    // Master Volume & PC Volume Sync
    void SetMasterVolume(float vol);
    float GetMasterVolume() const { return m_masterVolume.load(std::memory_order_relaxed); }
    void SetSyncWithWindowsVolume(bool sync) { m_syncWithWindowsVolume.store(sync, std::memory_order_relaxed); }
    bool GetSyncWithWindowsVolume() const { return m_syncWithWindowsVolume.load(std::memory_order_relaxed); }
    bool IsMuted() const { return m_isMuted.load(std::memory_order_relaxed); }
    void UpdateWindowsVolumeSync();

    // Headless automated test suites
    static int RunTestDevices();
    static int RunTestBuffer();
    static int RunTestCapture(int durationSeconds);
    static int RunTestCalibration();
    static int RunTestAutoSync();
    static int RunTestBeep(const std::wstring& targetMatch = L"");
    static int RunTestVolume();

private:
    void CaptureLoop();
    void SingleChannelCalibrationWorker(int channelIndex, std::function<void(bool, int, const std::string&)> onComplete);
    void DualChannelAutoSyncWorker(std::function<void(bool, int, const std::string&)> onComplete);
    void MicMonitorLoop();
    void MetronomeLoop(int intervalMs);

    std::atomic<bool> m_isStreaming{false};
    std::atomic<bool> m_stopRequested{false};

    bool m_mirrorSystemAudio{true};
    bool m_smartAutoSync{true};
    bool m_preventEchoOnDefault{true};

    std::wstring m_deviceIdA;
    std::wstring m_deviceIdB;
    DWORD m_pidFilterA{0};
    DWORD m_pidFilterB{0};

    std::shared_ptr<AudioPipeline> m_pipelineA;
    std::shared_ptr<AudioPipeline> m_pipelineB;

    std::thread m_captureThread;
    IMMDevice* m_pCaptureDevice = nullptr;
    IAudioClient* m_pCaptureClient = nullptr;
    IAudioCaptureClient* m_pCaptureService = nullptr;
    HANDLE m_hCaptureEvent = NULL;
    WAVEFORMATEX* m_pCaptureFormat = nullptr;

    std::atomic<bool> m_playingBeep{false};
    std::atomic<bool> m_isCalibrating{false};
    std::thread m_calibrationThread;
    mutable std::mutex m_calibrationMutex;
    std::string m_calibrationStatus;

    std::atomic<bool> m_metronomeRunning{false};
    std::thread m_metronomeThread;

    std::atomic<float> m_micPeakVU{0.0f};
    std::atomic<float> m_micGain{1.0f};
    std::atomic<bool> m_micRawActive{false};
    std::atomic<bool> m_micMonitorRunning{false};
    std::atomic<bool> m_stopMicMonitor{false};
    std::thread m_micMonitorThread;
    std::wstring m_micDeviceId;

    std::atomic<float> m_masterVolume{1.0f};
    std::atomic<bool> m_syncWithWindowsVolume{true};
    std::atomic<bool> m_isMuted{false};

    void AttachWindowsVolumeEndpoint();
    void DetachWindowsVolumeEndpoint();
    IMMDevice* m_pDefaultEndpointDevice = nullptr;
    IAudioEndpointVolume* m_pDefaultEndpointVolume = nullptr;
    IAudioEndpointVolumeCallback* m_pVolumeWatcher = nullptr;

public:
    void HandleEndpointVolumeNotification(float vol, bool muted, const GUID& context);
};

} // namespace Auduo
