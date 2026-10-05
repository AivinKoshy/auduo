#pragma once

#include "AudioEngine.hpp"
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

private:
    void RenderHeader();
    void RenderChannel(const char* label, std::shared_ptr<AudioPipeline> pipeline, int& selectedDeviceIdx, int& selectedAppIdx);
    void RefreshDeviceList();
    void RefreshInputDeviceList();

    AudioEngine& m_engine;
    std::vector<AudioDevice> m_devices;
    std::vector<AudioDevice> m_inputDevices;

    int m_selectedDeviceA = 0;
    int m_selectedDeviceB = 0;
    int m_selectedMicDevice = 0;

    float m_volA = 0.70f;
    float m_volB = 0.85f;
    float m_masterVol = 1.0f;
    bool m_syncWindowsVol = true;
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
};

} // namespace Auduo
