#pragma once

#include "AudioEngine.hpp"
#include "imgui.h"
#include <d3d11.h>

namespace Auduo {

class UI {
public:
    UI(AudioEngine& engine);
    ~UI();

    bool Initialize(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();

    // Render one frame of ImGui controls
    void Render();

    // Persistence (resolves to portable or %APPDATA% path automatically)
    void LoadConfig(const std::string& filename = "");
    void SaveConfig(const std::string& filename = "");

    bool ShouldMinimizeToTray() const { return m_minimizeToTray; }
    void ResetMinimizeToTray() { m_minimizeToTray = false; }
    bool IsAlwaysMinimizeToTray() const { return m_alwaysMinimizeToTray; }

    int GetIdealClientHeight() const;

    bool IsAudioRoutingEnabled() const { return m_audioRoutingEnabled; }
    void SetAudioRoutingEnabled(bool enabled) { m_audioRoutingEnabled = enabled; }

private:
    void RenderHeader();
    void RenderChannel(const char* label, std::shared_ptr<AudioPipeline> pipeline, int& selectedDeviceIdx, int& selectedAppIdx);
    void RefreshDeviceList();
    void RefreshInputDeviceList();

    struct StreamChannel {
        std::string label;
        int deviceIdx = 0;
        std::wstring deviceId;
        float volume = 0.80f;
        int syncDelayMs = 0;
        std::shared_ptr<AudioPipeline> pipeline;
        ImVec4 accentColor = ImVec4(0.4f, 0.8f, 1.0f, 1.0f);

        // Per-App Audio Routing
        int appIdx = 0;              // 0 = "[All System Audio]", 1..N = app from m_activeAppSessions
        DWORD routedAppPid = 0;
        std::wstring routedAppName;  // e.g. L"spotify.exe"
    };

    void RefreshAppSessionList();
    void ApplyStreamAppRouting(size_t chIdx);
    void SyncAllAppRouting();

    AudioEngine& m_engine;
    std::vector<AudioDevice> m_devices;
    std::vector<AudioDevice> m_inputDevices;
    std::vector<StreamChannel> m_channels;

    int m_selectedDeviceA = 0;
    int m_selectedDeviceB = 0;
    int m_selectedMicDevice = 0;

    float m_volA = 0.70f;
    float m_volB = 0.85f;
    float m_masterVol = 1.0f;
    bool m_syncWindowsVol = true;
    bool m_audioRoutingEnabled = false;
    std::vector<AppAudioSession> m_activeAppSessions;
    bool m_micMonitorEnabled = false;
    int m_syncOffsetA = 0; // milliseconds
    int m_syncOffsetB = 40; // milliseconds

    bool m_minimizeToTray = false;
    bool m_alwaysMinimizeToTray = false;
    bool m_dontShowDonationAgain = false;
    bool m_firstFrame = true;
    bool m_initialized = false;

    // Simultaneous Dual-Channel Auto-Sync tracking
    int m_autoSyncDelta = 0;
    bool m_autoSyncDone = false;
    std::string m_autoSyncMsg;
    bool m_metronomeEnabled = false;
    std::string m_calibMsg;

    char m_statusMessage[128] = "Ready";

    std::vector<std::string> m_terminalLogs;
    std::string m_lastCalibStatus;
    void AddTerminalLog(const std::string& line);
};

} // namespace Auduo
