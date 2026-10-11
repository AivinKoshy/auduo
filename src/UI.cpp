#include "UI.hpp"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <windows.h>
#include <shellapi.h>

namespace Auduo {

static void OpenDonationPage() {
    // 1. Try docs folder relative to executable directory
    char exePath[MAX_PATH];
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH) > 0) {
        char* lastSlash = strrchr(exePath, '\\');
        if (lastSlash) *(lastSlash + 1) = '\0';
        std::string exeDoc = std::string(exePath) + "..\\docs\\donate\\index.html";
        DWORD attr = GetFileAttributesA(exeDoc.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            ShellExecuteA(NULL, "open", exeDoc.c_str(), NULL, NULL, SW_SHOWNORMAL);
            return;
        }
        std::string sameDirDoc = std::string(exePath) + "docs\\donate\\index.html";
        attr = GetFileAttributesA(sameDirDoc.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            ShellExecuteA(NULL, "open", sameDirDoc.c_str(), NULL, NULL, SW_SHOWNORMAL);
            return;
        }
    }

    // 2. Try current working directory
    const char* cwdDoc = "docs\\donate\\index.html";
    DWORD attr = GetFileAttributesA(cwdDoc);
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        ShellExecuteA(NULL, "open", cwdDoc, NULL, NULL, SW_SHOWNORMAL);
        return;
    }

    // 3. Fallback to official online donation page (or repository)
    ShellExecuteA(NULL, "open", "https://aivinkoshy.github.io/auduo/donate/", NULL, NULL, SW_SHOWNORMAL);
}

// Helper to convert wstring to utf8 string
static std::string WStringToUTF8(const std::wstring& wstr) {
    if (wstr.empty()) return std::string();
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), NULL, 0, NULL, NULL);
    if (sizeNeeded <= 0) return std::string();
    std::string strTo(sizeNeeded, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), (int)wstr.size(), &strTo[0], sizeNeeded, NULL, NULL);
    return strTo;
}

// Helper to resolve configuration file path (portable folder preferred, fallback to %APPDATA%\AuDuo\)
static std::string GetConfigFilePath(const std::string& filename = "auduo_config.ini") {
    char exePath[MAX_PATH] = {0};
    if (GetModuleFileNameA(NULL, exePath, MAX_PATH) > 0) {
        char* lastSlash = strrchr(exePath, '\\');
        if (lastSlash) *(lastSlash + 1) = '\0';
        std::string localConfig = std::string(exePath) + filename;
        DWORD attr = GetFileAttributesA(localConfig.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            HANDLE hTest = CreateFileA(localConfig.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            if (hTest != INVALID_HANDLE_VALUE) {
                CloseHandle(hTest);
                return localConfig;
            }
        }
    }

    char appData[MAX_PATH] = {0};
    if (GetEnvironmentVariableA("APPDATA", appData, MAX_PATH) > 0) {
        std::string dir = std::string(appData) + "\\AuDuo";
        CreateDirectoryA(dir.c_str(), NULL);
        return dir + "\\" + filename;
    }

    return filename;
}

UI::UI(AudioEngine& engine)
    : m_engine(engine)
{
    RefreshDeviceList();
    RefreshInputDeviceList();

    // Initialize default channel list (Stream A and Stream B)
    m_channels.clear();
    StreamChannel chanA;
    chanA.label = "Stream A";
    chanA.deviceIdx = m_selectedDeviceA;
    if (m_selectedDeviceA >= 0 && m_selectedDeviceA < (int)m_devices.size()) {
        chanA.deviceId = m_devices[m_selectedDeviceA].id;
    }
    chanA.volume = m_volA;
    chanA.syncDelayMs = m_syncOffsetA;
    chanA.pipeline = m_engine.GetPipelineA();
    if (chanA.pipeline) chanA.pipeline->SetDeviceId(chanA.deviceId);
    chanA.accentColor = ImVec4(0.4f, 0.8f, 1.0f, 1.0f); // Cool Cyan

    StreamChannel chanB;
    chanB.label = "Stream B";
    chanB.deviceIdx = m_selectedDeviceB;
    if (m_selectedDeviceB >= 0 && m_selectedDeviceB < (int)m_devices.size()) {
        chanB.deviceId = m_devices[m_selectedDeviceB].id;
    }
    chanB.volume = m_volB;
    chanB.syncDelayMs = m_syncOffsetB;
    chanB.pipeline = m_engine.GetPipelineB();
    if (chanB.pipeline) chanB.pipeline->SetDeviceId(chanB.deviceId);
    chanB.accentColor = ImVec4(1.0f, 0.6f, 0.2f, 1.0f); // Warm Amber

    m_channels.push_back(chanA);
    m_channels.push_back(chanB);

    LoadConfig();

    if (!m_devices.empty()) {
        if (!m_channels[0].deviceId.empty()) {
            m_engine.SelectDeviceA(m_channels[0].deviceId);
        } else if (m_selectedDeviceA >= 0 && m_selectedDeviceA < (int)m_devices.size()) {
            m_engine.SelectDeviceA(m_devices[m_selectedDeviceA].id);
        }
        if (!m_channels[1].deviceId.empty()) {
            m_engine.SelectDeviceB(m_channels[1].deviceId);
        } else if (m_selectedDeviceB >= 0 && m_selectedDeviceB < (int)m_devices.size()) {
            m_engine.SelectDeviceB(m_devices[m_selectedDeviceB].id);
        }
    }
    if (!m_inputDevices.empty()) {
        if (m_selectedMicDevice >= 0 && m_selectedMicDevice < (int)m_inputDevices.size()) {
            m_engine.SelectMicDevice(m_inputDevices[m_selectedMicDevice].id);
        }
    }

    AddTerminalLog("AuDuo Engine initialized. Ready for multi-device sync.");
}

UI::~UI() {
    Shutdown();
}

bool UI::Initialize(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context) {
    if (m_initialized) return true;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Dark sleek audio engineer theme
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.ItemSpacing = ImVec2(10, 8);
    style.FramePadding = ImVec2(8, 6);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);
    colors[ImGuiCol_Header] = ImVec4(0.20f, 0.25f, 0.35f, 0.80f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.59f, 0.98f, 0.80f);
    colors[ImGuiCol_Button] = ImVec4(0.18f, 0.22f, 0.30f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.06f, 0.53f, 0.98f, 1.00f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.16f, 0.20f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.26f, 0.59f, 0.98f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.40f, 0.70f, 1.00f, 1.00f);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.06f, 0.07f, 0.09f, 0.65f);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.25f, 0.30f, 0.40f, 0.85f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.35f, 0.45f, 0.60f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.45f, 0.60f, 0.80f, 1.00f);
    style.ScrollbarSize = 12.0f;
    style.ScrollbarRounding = 6.0f;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(device, context);

    static std::string s_imguiIniPath = GetConfigFilePath("imgui.ini");
    io.IniFilename = s_imguiIniPath.c_str();

    if (m_micMonitorEnabled) {
        m_engine.StartMicMonitor();
    }

    m_initialized = true;
    return true;
}

int UI::GetIdealClientHeight() const {
    const float panelHeight = m_audioRoutingEnabled ? 150.0f : 114.0f;
    const float addBtnHeight = 28.0f;
    const float itemSpacingY = 8.0f;
    const float fixedOverhead = 386.0f;

    size_t visibleStreams = (std::min)(m_channels.size(), static_cast<size_t>(3));
    float neededStreams = visibleStreams * (panelHeight + itemSpacingY) + addBtnHeight + itemSpacingY;
    int idealH = static_cast<int>(std::ceil(fixedOverhead + neededStreams));

    // Cap to work area height minus safety margin if available, or minimum 620
    RECT workArea;
    if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &workArea, 0)) {
        int maxWorkH = (workArea.bottom - workArea.top) - 24;
        if (idealH > maxWorkH) idealH = maxWorkH;
    }
    if (idealH < 620) idealH = 620;
    return idealH;
}

void UI::Shutdown() {
    if (!m_initialized) return;
    m_engine.StopMicMonitor();
    if (m_audioRoutingEnabled) {
        m_engine.ClearAllAppRouting();
    }
    SaveConfig();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    m_initialized = false;
}

void UI::AddTerminalLog(const std::string& line) {
    if (line.empty()) return;
    if (!m_terminalLogs.empty() && m_terminalLogs.back() == line) return;
    m_terminalLogs.push_back(line);
    if (m_terminalLogs.size() > 100) {
        m_terminalLogs.erase(m_terminalLogs.begin());
    }
}

void UI::RefreshDeviceList() {
    m_devices = AudioEngine::EnumerateOutputDevices();
    if (m_devices.empty()) return;

    // Resynchronize deviceIdx for all channels from their deviceId
    for (size_t i = 0; i < m_channels.size(); ++i) {
        auto& chan = m_channels[i];
        if (!chan.deviceId.empty()) {
            bool found = false;
            for (int d = 0; d < (int)m_devices.size(); ++d) {
                if (m_devices[d].id == chan.deviceId) {
                    chan.deviceIdx = d;
                    found = true;
                    break;
                }
            }
            if (!found) {
                chan.deviceIdx = -1; // Unplugged / Disconnected
            }
        } else {
            if (chan.deviceIdx >= (int)m_devices.size()) {
                chan.deviceIdx = (i < m_devices.size()) ? static_cast<int>(i) : 0;
            }
            if (chan.deviceIdx >= 0 && chan.deviceIdx < (int)m_devices.size()) {
                chan.deviceId = m_devices[chan.deviceIdx].id;
            }
        }
        if (i == 0) m_selectedDeviceA = chan.deviceIdx;
        if (i == 1) m_selectedDeviceB = chan.deviceIdx;
    }

    RefreshInputDeviceList();
}

void UI::RefreshInputDeviceList() {
    m_inputDevices = AudioEngine::EnumerateInputDevices();
    if (m_inputDevices.empty()) return;

    if (m_selectedMicDevice < 0 || m_selectedMicDevice >= (int)m_inputDevices.size()) {
        m_selectedMicDevice = 0;
        int micArrayIdx = -1;
        int defaultIdx = -1;
        for (int i = 0; i < (int)m_inputDevices.size(); ++i) {
            std::wstring lower = m_inputDevices[i].name;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
            if (lower.find(L"microphone array") != std::wstring::npos || lower.find(L"mic array") != std::wstring::npos) {
                micArrayIdx = i;
            }
            if (m_inputDevices[i].isDefault) {
                defaultIdx = i;
            }
        }
        if (micArrayIdx != -1) {
            m_selectedMicDevice = micArrayIdx;
        } else if (defaultIdx != -1) {
            m_selectedMicDevice = defaultIdx;
        }
    }
    if (m_selectedMicDevice >= 0 && m_selectedMicDevice < (int)m_inputDevices.size()) {
        m_engine.SelectMicDevice(m_inputDevices[m_selectedMicDevice].id);
    }
}

void UI::RefreshAppSessionList() {
    m_activeAppSessions = AudioEngine::EnumerateAudioSessions();

    // Re-synchronize each channel's appIdx and routedAppPid from active sessions
    for (size_t chIdx = 0; chIdx < m_channels.size(); ++chIdx) {
        auto& chan = m_channels[chIdx];
        if (chan.routedAppName.empty()) {
            chan.appIdx = 0;
            chan.routedAppPid = 0;
            continue;
        }

        // Try to match by process name
        int foundIdx = -1;
        for (int i = 0; i < (int)m_activeAppSessions.size(); ++i) {
            std::wstring sessionName = m_activeAppSessions[i].processName;
            std::wstring targetName = chan.routedAppName;
            std::transform(sessionName.begin(), sessionName.end(), sessionName.begin(), ::towlower);
            std::transform(targetName.begin(), targetName.end(), targetName.begin(), ::towlower);

            if (sessionName == targetName) {
                foundIdx = i + 1; // 1-based index (0 is [All System Audio])
                // Update PID if app restarted
                if (chan.routedAppPid != m_activeAppSessions[i].pid) {
                    chan.routedAppPid = m_activeAppSessions[i].pid;
                    if (m_audioRoutingEnabled && !chan.deviceId.empty()) {
                        m_engine.RouteAppToDevice(chan.routedAppPid, chan.deviceId);
                    }
                }
                break;
            }
        }

        if (foundIdx != -1) {
            chan.appIdx = foundIdx;
        } else {
            // Process not in active session list right now
            chan.appIdx = 0;
        }
    }
}

void UI::ApplyStreamAppRouting(size_t chIdx) {
    if (chIdx >= m_channels.size()) return;
    auto& chan = m_channels[chIdx];

    if (!m_audioRoutingEnabled) return;

    if (chan.appIdx == 0) {
        if (chan.routedAppPid != 0) {
            m_engine.ResetAppRouting(chan.routedAppPid);
            AddTerminalLog("App route reset to Windows default for " + chan.label + ".");
            chan.routedAppPid = 0;
        }
        chan.routedAppName.clear();
        return;
    }

    size_t sessionIdx = chan.appIdx - 1;
    if (sessionIdx < m_activeAppSessions.size()) {
        const auto& session = m_activeAppSessions[sessionIdx];

        // 1-to-1 routing enforcement:
        // If this app was assigned to another stream, unassign it from that stream
        for (size_t otherIdx = 0; otherIdx < m_channels.size(); ++otherIdx) {
            if (otherIdx != chIdx) {
                auto& otherChan = m_channels[otherIdx];
                if (otherChan.routedAppPid == session.pid ||
                    (!otherChan.routedAppName.empty() && otherChan.routedAppName == session.processName)) {
                    otherChan.appIdx = 0;
                    otherChan.routedAppPid = 0;
                    otherChan.routedAppName.clear();
                    AddTerminalLog("Unassigned '" + WStringToUTF8(session.displayName) + "' from " + otherChan.label + ".");
                }
            }
        }

        chan.routedAppPid = session.pid;
        chan.routedAppName = session.processName;

        if (!chan.deviceId.empty()) {
            bool ok = m_engine.RouteAppToDevice(session.pid, chan.deviceId);
            if (ok) {
                AddTerminalLog("Routed '" + WStringToUTF8(session.displayName) + "' to " + chan.label + ".");
            } else {
                AddTerminalLog("Failed to route '" + WStringToUTF8(session.displayName) + "' (OS returned error).");
            }
        }
    }
}

void UI::SyncAllAppRouting() {
    if (!m_audioRoutingEnabled) {
        m_engine.ClearAllAppRouting();
        return;
    }

    for (size_t chIdx = 0; chIdx < m_channels.size(); ++chIdx) {
        auto& chan = m_channels[chIdx];
        if (chan.routedAppPid != 0 && !chan.deviceId.empty()) {
            m_engine.RouteAppToDevice(chan.routedAppPid, chan.deviceId);
        }
    }
}

void UI::LoadConfig(const std::string& filename) {
    std::string path = filename.empty() ? GetConfigFilePath("auduo_config.ini") : filename;
    std::ifstream file(path);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if (val.empty()) continue;

        try {
            if (key == "vol_a") m_volA = std::clamp(std::stof(val), 0.0f, 1.0f);
            else if (key == "vol_b") m_volB = std::clamp(std::stof(val), 0.0f, 1.0f);
            else if (key == "master_vol") {
                m_masterVol = std::clamp(std::stof(val), 0.0f, 1.0f);
                m_engine.SetMasterVolume(m_masterVol);
            }
            else if (key == "sync_windows_vol") {
                m_syncWindowsVol = (val == "1");
                m_engine.SetSyncWithWindowsVolume(m_syncWindowsVol);
            }
            else if (key == "mic_preview") {
                m_micMonitorEnabled = (val == "1");
            }
            else if (key == "sync_a") m_syncOffsetA = std::clamp(std::stoi(val), 0, 2000);
            else if (key == "sync_b") m_syncOffsetB = std::clamp(std::stoi(val), 0, 2000);
            else if (key == "device_a_idx") m_selectedDeviceA = std::stoi(val);
            else if (key == "device_b_idx") m_selectedDeviceB = std::stoi(val);
            else if (key == "device_a_id") {
                int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, val.c_str(), (int)val.size(), NULL, 0);
                if (sizeNeeded > 0) {
                    std::wstring wId(sizeNeeded, 0);
                    MultiByteToWideChar(CP_UTF8, 0, val.c_str(), (int)val.size(), &wId[0], sizeNeeded);
                    if (m_channels.size() > 0) {
                        m_channels[0].deviceId = wId;
                        if (m_channels[0].pipeline) m_channels[0].pipeline->SetDeviceId(wId);
                    }
                    for (int i = 0; i < (int)m_devices.size(); ++i) {
                        if (m_devices[i].id == wId) {
                            m_selectedDeviceA = i;
                            if (m_channels.size() > 0) m_channels[0].deviceIdx = i;
                            m_engine.SelectDeviceA(m_devices[i].id);
                            break;
                        }
                    }
                }
            }
            else if (key == "device_b_id") {
                int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, val.c_str(), (int)val.size(), NULL, 0);
                if (sizeNeeded > 0) {
                    std::wstring wId(sizeNeeded, 0);
                    MultiByteToWideChar(CP_UTF8, 0, val.c_str(), (int)val.size(), &wId[0], sizeNeeded);
                    if (m_channels.size() > 1) {
                        m_channels[1].deviceId = wId;
                        if (m_channels[1].pipeline) m_channels[1].pipeline->SetDeviceId(wId);
                    }
                    for (int i = 0; i < (int)m_devices.size(); ++i) {
                        if (m_devices[i].id == wId) {
                            m_selectedDeviceB = i;
                            if (m_channels.size() > 1) m_channels[1].deviceIdx = i;
                            m_engine.SelectDeviceB(m_devices[i].id);
                            break;
                        }
                    }
                }
            }
            else if (key == "mic_device_idx") m_selectedMicDevice = std::stoi(val);
            else if (key == "mic_device_id") {
                int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, val.c_str(), (int)val.size(), NULL, 0);
                if (sizeNeeded > 0) {
                    std::wstring wId(sizeNeeded, 0);
                    MultiByteToWideChar(CP_UTF8, 0, val.c_str(), (int)val.size(), &wId[0], sizeNeeded);
                    for (int i = 0; i < (int)m_inputDevices.size(); ++i) {
                        if (m_inputDevices[i].id == wId) {
                            m_selectedMicDevice = i;
                            m_engine.SelectMicDevice(m_inputDevices[i].id);
                            break;
                        }
                    }
                }
            }
            else if (key == "mic_gain") m_engine.SetMicGain(std::clamp(std::stof(val), 0.1f, 20.0f));
            else if (key == "always_minimize_to_tray") m_alwaysMinimizeToTray = (val == "1");
            else if (key == "dont_show_donation") m_dontShowDonationAgain = (val == "1");
        } catch (...) {
            // Ignore malformed individual config lines safely
        }
    }

    if (m_channels.size() >= 2) {
        m_channels[0].volume = m_volA;
        m_channels[0].syncDelayMs = m_syncOffsetA;
        m_channels[0].deviceIdx = m_selectedDeviceA;
        if (m_selectedDeviceA >= 0 && m_selectedDeviceA < (int)m_devices.size()) {
            m_channels[0].deviceId = m_devices[m_selectedDeviceA].id;
        }

        m_channels[1].volume = m_volB;
        m_channels[1].syncDelayMs = m_syncOffsetB;
        m_channels[1].deviceIdx = m_selectedDeviceB;
        if (m_selectedDeviceB >= 0 && m_selectedDeviceB < (int)m_devices.size()) {
            m_channels[1].deviceId = m_devices[m_selectedDeviceB].id;
        }
    }

    if (m_selectedMicDevice < 0 || m_selectedMicDevice >= (int)m_inputDevices.size()) {
        m_selectedMicDevice = 0;
    }
    if (m_selectedDeviceA < 0 || m_selectedDeviceA >= (int)m_devices.size()) {
        m_selectedDeviceA = 0;
    }
    if (m_selectedDeviceB < 0 || m_selectedDeviceB >= (int)m_devices.size()) {
        m_selectedDeviceB = (m_devices.size() > 1) ? 1 : 0;
    }

    // Always start with Audio Routing turned off at startup
    m_audioRoutingEnabled = false;
}

void UI::SaveConfig(const std::string& filename) {
    std::string path = filename.empty() ? GetConfigFilePath("auduo_config.ini") : filename;
    std::ofstream file(path);
    if (!file.is_open()) return;

    file << "vol_a=" << m_volA << "\n";
    file << "vol_b=" << m_volB << "\n";
    file << "master_vol=" << m_masterVol << "\n";
    file << "sync_windows_vol=" << (m_syncWindowsVol ? "1" : "0") << "\n";
    file << "always_minimize_to_tray=" << (m_alwaysMinimizeToTray ? "1" : "0") << "\n";
    file << "dont_show_donation=" << (m_dontShowDonationAgain ? "1" : "0") << "\n";
    file << "audio_routing_enabled=" << (m_audioRoutingEnabled ? "1" : "0") << "\n";
    file << "mic_preview=" << (m_micMonitorEnabled ? "1" : "0") << "\n";
    file << "sync_a=" << m_syncOffsetA << "\n";
    file << "sync_b=" << m_syncOffsetB << "\n";
    file << "device_a_idx=" << m_selectedDeviceA << "\n";
    file << "device_b_idx=" << m_selectedDeviceB << "\n";
    if (m_selectedDeviceA >= 0 && m_selectedDeviceA < (int)m_devices.size()) {
        file << "device_a_id=" << WStringToUTF8(m_devices[m_selectedDeviceA].id) << "\n";
    }
    if (m_selectedDeviceB >= 0 && m_selectedDeviceB < (int)m_devices.size()) {
        file << "device_b_id=" << WStringToUTF8(m_devices[m_selectedDeviceB].id) << "\n";
    }
    file << "mic_device_idx=" << m_selectedMicDevice << "\n";
    if (m_selectedMicDevice >= 0 && m_selectedMicDevice < (int)m_inputDevices.size()) {
        file << "mic_device_id=" << WStringToUTF8(m_inputDevices[m_selectedMicDevice].id) << "\n";
    }
    file << "mic_gain=" << m_engine.GetMicGain() << "\n";
}

// Custom vector hardware LED widget (draws crisp circular LED with halo, bypassing font glyph issues)
static void DrawLED(bool active, const char* label, const char* tooltip = nullptr,
                    ImVec4 onColor = ImVec4(0.25f, 0.90f, 0.45f, 1.0f),
                    ImVec4 offColor = ImVec4(0.95f, 0.25f, 0.25f, 1.0f)) {
    ImGui::BeginGroup();
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float radius = 4.5f;
    float frameH = ImGui::GetFrameHeight();
    ImVec2 center(p.x + radius + 1.0f, p.y + frameH * 0.5f);

    ImVec4 chosenColor = active ? onColor : offColor;
    ImU32 col = ImGui::ColorConvertFloat4ToU32(chosenColor);

    if (active) {
        ImVec4 glowCol = chosenColor;
        glowCol.w = 0.35f;
        drawList->AddCircleFilled(center, radius + 3.0f, ImGui::ColorConvertFloat4ToU32(glowCol));
    }
    drawList->AddCircleFilled(center, radius, col);

    ImGui::Dummy(ImVec2(radius * 2.0f + 2.0f, frameH));
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(chosenColor, "%s", label);
    ImGui::EndGroup();

    if (tooltip && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
}

void UI::Render() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);

    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::Begin("AuduoMainWindow", nullptr, windowFlags);

    if (m_firstFrame) {
        m_firstFrame = false;
        if (!m_dontShowDonationAgain) {
            ImGui::OpenPopup("Support AuDuo##DonationModal");
        }
    }

    bool cableDefault = m_engine.IsVirtualCableDefault();

    // Symmetrical column metrics
    const float colSpacing = 16.0f;
    const float colStartX = ImGui::GetCursorPosX();
    const float colWidth = (ImGui::GetContentRegionAvail().x - colSpacing) * 0.5f;
    const float colB_X = colStartX + colWidth + colSpacing;

    // Row 1: Merged Streaming / Virtual Cable Button (Column A) | Master Volume Slider (Column B)
    const float streamBtnHeight = 32.0f;
    float row1StartY = ImGui::GetCursorPosY();
    bool streaming = m_engine.IsStreaming();
    if (!streaming) {
        if (!cableDefault) {
            ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22f, 0.24f, 0.28f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.24f, 0.28f, 1.0f));
            ImGui::Button("  Switch PC Output to Virtual Cable  ", ImVec2(colWidth, streamBtnHeight));
            ImGui::PopStyleColor(2);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("VB-Audio Virtual Cable is not set as your Windows Default Playback Device.\nPlease set 'CABLE Input' as default in Windows Sound settings to enable streaming.");
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.60f, 0.25f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.75f, 0.32f, 1.0f));
            if (ImGui::Button("  START STREAMING  ", ImVec2(colWidth, streamBtnHeight))) {
                if (!m_devices.empty()) {
                    for (size_t i = 0; i < m_channels.size(); ++i) {
                        if (m_channels[i].deviceIdx >= 0 && m_channels[i].deviceIdx < (int)m_devices.size()) {
                            m_channels[i].deviceId = m_devices[m_channels[i].deviceIdx].id;
                        }
                        if (i == 0 && !m_channels[i].deviceId.empty()) {
                            m_engine.SelectDeviceA(m_channels[i].deviceId);
                        } else if (i == 1 && !m_channels[i].deviceId.empty()) {
                            m_engine.SelectDeviceB(m_channels[i].deviceId);
                        } else if (m_channels[i].pipeline && !m_channels[i].deviceId.empty()) {
                            m_channels[i].pipeline->SetDeviceId(m_channels[i].deviceId);
                        }
                        if (m_channels[i].pipeline) {
                            m_channels[i].pipeline->SetVolume(m_channels[i].volume);
                            m_channels[i].pipeline->SetDelayMs(m_channels[i].syncDelayMs);
                        }
                    }
                }
                m_engine.SetMirrorSystemAudio(true);
                m_engine.StartStreaming();
                AddTerminalLog("Audio streaming started. Mirroring system audio across active streams.");
            }
            ImGui::PopStyleColor(2);
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.20f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.25f, 0.25f, 1.0f));
        if (ImGui::Button("  STOP STREAMING  ", ImVec2(colWidth, streamBtnHeight))) {
            m_engine.StopStreaming();
            AddTerminalLog("Audio streaming stopped.");
        }
        ImGui::PopStyleColor(2);
    }

    // Periodic Windows Volume Sync check (every ~30ms for instant Fn key responsiveness)
    static auto lastVolCheck = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastVolCheck).count() > 30) {
        lastVolCheck = now;
        m_engine.UpdateWindowsVolumeSync();
        float master = m_engine.IsMuted() ? 0.0f : m_engine.GetMasterVolume();
        for (size_t i = 2; i < m_channels.size(); ++i) {
            if (m_channels[i].pipeline) m_channels[i].pipeline->SetMasterVolume(master);
        }
    }

    // Periodic Device Reconnect Watchdog (every 3 seconds if streaming is active)
    static auto lastReconnectCheck = std::chrono::steady_clock::now();
    if (m_engine.IsStreaming() && std::chrono::duration_cast<std::chrono::seconds>(now - lastReconnectCheck).count() >= 3) {
        lastReconnectCheck = now;
        for (auto& chan : m_channels) {
            // Do NOT hammer Initialize on devices that encountered a driver/buffer failure (prevents wedging Windows Bluetooth stack)
            // Error states are safely cleared when the user clicks 'Refresh'
            if (chan.pipeline && !chan.pipeline->IsRunning() && !chan.deviceId.empty() && !chan.pipeline->HasDeviceError()) {
                chan.pipeline->Initialize(chan.deviceId);
                float master = m_engine.IsMuted() ? 0.0f : m_engine.GetMasterVolume();
                chan.pipeline->SetMasterVolume(master);
                chan.pipeline->SetVolume(chan.volume);
                chan.pipeline->SetDelayMs(chan.syncDelayMs);
            }
        }
    }

    // Periodic Audio Sessions enumeration (every 1.5 seconds if audio routing is enabled)
    static auto lastSessionCheck = std::chrono::steady_clock::now();
    if (m_audioRoutingEnabled && std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSessionCheck).count() >= 1500) {
        lastSessionCheck = now;
        RefreshAppSessionList();
    }

    // Grey out the rest of the application controls if streaming hasn't been started
    if (!streaming) {
        ImGui::BeginDisabled();
    }

    // Column B on Row 1: Master Volume Slider aligned with Column B, vertically centered with button
    float mvFrameH = ImGui::GetFrameHeight();
    float mvYOffset = (streamBtnHeight - mvFrameH) * 0.5f;
    ImGui::SetCursorPos(ImVec2(colB_X, row1StartY + mvYOffset));
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Master Volume:");
    ImGui::SameLine();
    float mvSliderStart = ImGui::GetCursorPosX();
    float mvMutedReserve = m_engine.IsMuted() ? 64.0f : 0.0f;
    float mvSliderWidth = (colB_X + colWidth) - mvSliderStart - mvMutedReserve;
    if (mvSliderWidth < 100.0f) mvSliderWidth = 100.0f;
    ImGui::SetNextItemWidth(mvSliderWidth);
    int masterPercent = static_cast<int>(std::round(m_engine.GetMasterVolume() * 100.0f));
    if (ImGui::SliderInt("##MasterVol", &masterPercent, 0, 100, "%d%%")) {
        m_masterVol = masterPercent / 100.0f;
        m_engine.SetMasterVolume(m_masterVol);
    }
    if (m_engine.IsMuted()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "[MUTED]");
    }

    ImGui::SetCursorPosY(row1StartY + streamBtnHeight + ImGui::GetStyle().ItemSpacing.y);

    // Row 2: Two Checkboxes (Column A: Sync with PC Volume Keys | Column B: Audio Routing)
    if (ImGui::Checkbox("Sync with PC Volume Keys", &m_syncWindowsVol)) {
        m_engine.SetSyncWithWindowsVolume(m_syncWindowsVol);
        SaveConfig();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("When checked, laptop physical volume keys (Fn+Vol) and Windows taskbar slider automatically scale both audio streams.");
    }

    ImGui::SameLine(colB_X);
    bool routingSupported = m_engine.IsAudioRoutingSupported();
    if (!routingSupported) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Checkbox("Audio Routing", &m_audioRoutingEnabled)) {
        if (m_audioRoutingEnabled) {
            RefreshAppSessionList();
            SyncAllAppRouting();
            AddTerminalLog("Audio Routing enabled: assign apps to specific streams below.");
        } else {
            m_engine.ClearAllAppRouting();
            for (auto& chan : m_channels) {
                chan.appIdx = 0;
                chan.routedAppPid = 0;
                chan.routedAppName.clear();
            }
            AddTerminalLog("Audio Routing disabled: all apps reverted to Windows default.");
        }
        SaveConfig();
    }
    if (!routingSupported) {
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("Per-app audio routing is not supported on this Windows build.");
        }
    } else if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Direct individual applications to play through specific output streams (1-to-1 audio routing).");
    }

    ImGui::Separator();

    // Dynamic Streams Region Height:
    // Automatically expands for up to 3 streams max, and scrolls smoothly for 4+ streams.
    const float panelHeight = m_audioRoutingEnabled ? 150.0f : 114.0f;
    const float addBtnHeight = 28.0f;
    const float itemSpacingY = ImGui::GetStyle().ItemSpacing.y;
    const float footerReservedHeight = 252.0f;

    size_t visibleStreams = (std::min)(m_channels.size(), static_cast<size_t>(3));
    float neededStreamsHeight = visibleStreams * (panelHeight + itemSpacingY) + addBtnHeight + itemSpacingY;
    float maxStreamsHeight = ImGui::GetContentRegionAvail().y - footerReservedHeight;
    if (maxStreamsHeight < 120.0f) maxStreamsHeight = 120.0f;

    float streamAreaHeight = (m_channels.size() <= 3)
                                 ? neededStreamsHeight
                                 : (std::min)(neededStreamsHeight, maxStreamsHeight);

    // Dedicated Scrollable Streams Region (Header & Footer remain pinned)
    // For 2 or 3 streams, the window resizes to show all streams without scrolling.
    // For 4+ streams, internal vertical scrolling is enabled.
    ImGuiWindowFlags streamsRegionFlags = (m_channels.size() <= 3)
                                              ? (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)
                                              : ImGuiWindowFlags_None;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("StreamsScrollRegion", ImVec2(0, streamAreaHeight), false, streamsRegionFlags);

    const float fullWidth = ImGui::GetContentRegionAvail().x;
    const float labelW = 68.0f;
    const float barHeight = ImGui::GetFrameHeight();

    int channelToRemove = -1;

    for (size_t chIdx = 0; chIdx < m_channels.size(); ++chIdx) {
        auto& chan = m_channels[chIdx];
        std::string panelId = "StreamPanel_" + std::to_string(chIdx);

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.14f, 0.18f, 0.75f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.24f, 0.28f, 0.38f, 0.65f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 8.0f));
        // Explicitly lock cards: NO internal scrollbar, NO mouse-wheel scroll capture
        ImGuiWindowFlags cardFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        ImGui::BeginChild(panelId.c_str(), ImVec2(fullWidth, panelHeight), true, cardFlags);

        // Header Row: Stream Title, Status, and Remove Button (for streams beyond 2)
        ImGui::TextColored(chan.accentColor, "%s", chan.label.c_str());

        bool hasErr = (chan.pipeline && (chan.pipeline->HasDeviceError() || (m_engine.IsStreaming() && !chan.pipeline->IsRunning())));
        // Only Channel 0 can be the Windows Default Passthrough mirror
        bool isDefault = (chIdx == 0 && !m_devices.empty() && chan.deviceIdx >= 0 && chan.deviceIdx < (int)m_devices.size() && m_devices[chan.deviceIdx].isDefault);
        if (hasErr) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.25f, 1.0f), " [DISCONNECTED]");
        } else if (isDefault) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.0f, 0.85f), " [Win Default Passthrough]");
        }

        // Allow removing custom extra channels (channels beyond standard A and B)
        if (chIdx >= 2) {
            ImGui::SameLine(fullWidth - 28.0f);
            float btnY = ImGui::GetCursorPosY() - 2.0f;
            ImGui::SetCursorPosY(btnY);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.32f, 0.12f, 0.14f, 0.60f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.22f, 0.22f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.98f, 0.10f, 0.10f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.88f, 0.88f, 0.88f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
            std::string remId = "x##Rem" + std::to_string(chIdx);
            if (ImGui::Button(remId.c_str(), ImVec2(20, 20))) {
                channelToRemove = static_cast<int>(chIdx);
            }
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(4);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Remove this stream output");
            }
        }

        ImGui::Spacing();

        std::string gridId = "Grid_" + std::to_string(chIdx);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f, 3.0f));
        if (ImGui::BeginTable(gridId.c_str(), 2, ImGuiTableFlags_None)) {
            ImGui::TableSetupColumn("Col_Left", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Col_Right", ImGuiTableColumnFlags_WidthStretch);

            // Row 1: Output Device (Left) | Volume (Right)
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Device:");
            ImGui::SameLine(labelW);
            ImGui::SetNextItemWidth(-1.0f);
            std::string curDev = (m_devices.empty() || chan.deviceIdx < 0 || chan.deviceIdx >= (int)m_devices.size())
                                     ? "None"
                                     : (WStringToUTF8(m_devices[chan.deviceIdx].name) + (m_devices[chan.deviceIdx].isDefault ? " [WIN DEFAULT]" : ""));
            std::string comboId = "##DevCombo_" + std::to_string(chIdx);
            if (ImGui::BeginCombo(comboId.c_str(), curDev.c_str())) {
                for (int d = 0; d < (int)m_devices.size(); ++d) {
                    bool isSelected = (chan.deviceIdx == d);
                    std::string dName = WStringToUTF8(m_devices[d].name) + (m_devices[d].isDefault ? " [WIN DEFAULT]" : "") + (m_devices[d].isBluetooth ? " [BT]" : "");
                    if (ImGui::Selectable(dName.c_str(), isSelected)) {
                        chan.deviceIdx = d;
                        chan.deviceId = m_devices[d].id;
                        if (chIdx == 0) {
                            m_selectedDeviceA = d;
                            m_engine.SelectDeviceA(m_devices[d].id);
                        } else if (chIdx == 1) {
                            m_selectedDeviceB = d;
                            m_engine.SelectDeviceB(m_devices[d].id);
                        } else {
                            m_engine.SelectPipelineDevice(chan.pipeline, m_devices[d].id);
                        }
                        if (m_audioRoutingEnabled && chan.routedAppPid != 0 && !chan.deviceId.empty()) {
                            m_engine.RouteAppToDevice(chan.routedAppPid, chan.deviceId);
                            AddTerminalLog("Re-routed " + WStringToUTF8(chan.routedAppName) + " to new device on " + chan.label + ".");
                        }
                        SaveConfig();
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Volume:");
            ImGui::SameLine(labelW);
            ImGui::SetNextItemWidth(-1.0f);
            std::string volId = "##Vol_" + std::to_string(chIdx);
            int volPercent = static_cast<int>(std::round(chan.volume * 100.0f));
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, chan.accentColor);
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(chan.accentColor.x * 1.15f, chan.accentColor.y * 1.15f, chan.accentColor.z * 1.15f, 1.0f));
            if (ImGui::SliderInt(volId.c_str(), &volPercent, 0, 100, "%d%%")) {
                chan.volume = volPercent / 100.0f;
                if (chIdx == 0) m_volA = chan.volume;
                if (chIdx == 1) m_volB = chan.volume;
                if (chan.pipeline) chan.pipeline->SetVolume(chan.volume);
                SaveConfig();
            }
            ImGui::PopStyleColor(2);

            // Row 2: Live VU (Left) | Sync Delay (Right)
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Live VU:");
            ImGui::SameLine(labelW);
            float vu = chan.pipeline ? chan.pipeline->GetPeakVU() : 0.0f;
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, chan.accentColor);
            ImGui::ProgressBar(vu, ImVec2(-1.0f, barHeight), "");
            ImGui::PopStyleColor();

            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Delay:");
            ImGui::SameLine(labelW);
            ImGui::SetNextItemWidth(-1.0f);
            std::string delayId = "##Delay_" + std::to_string(chIdx);
            ImGui::PushStyleColor(ImGuiCol_SliderGrab, chan.accentColor);
            ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(chan.accentColor.x * 1.15f, chan.accentColor.y * 1.15f, chan.accentColor.z * 1.15f, 1.0f));
            if (ImGui::SliderInt(delayId.c_str(), &chan.syncDelayMs, 0, 2000, "+%d ms")) {
                if (chIdx == 0) m_syncOffsetA = chan.syncDelayMs;
                if (chIdx == 1) m_syncOffsetB = chan.syncDelayMs;
                if (chan.pipeline) chan.pipeline->SetDelayMs(chan.syncDelayMs);
                SaveConfig();
            }
            ImGui::PopStyleColor(2);

            // Row 3: Per-App Routing (Left) | Reset Route (Right) - only visible when Audio Routing is enabled
            if (m_audioRoutingEnabled) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("App:");
                ImGui::SameLine(labelW);
                ImGui::SetNextItemWidth(-1.0f);

                std::string appComboId = "##AppCombo_" + std::to_string(chIdx);
                std::string curAppLabel = "[All System Audio]";
                if (chan.appIdx > 0 && (chan.appIdx - 1) < (int)m_activeAppSessions.size()) {
                    curAppLabel = WStringToUTF8(m_activeAppSessions[chan.appIdx - 1].displayName);
                } else if (!chan.routedAppName.empty()) {
                    curAppLabel = WStringToUTF8(chan.routedAppName) + " [Offline]";
                }

                if (ImGui::BeginCombo(appComboId.c_str(), curAppLabel.c_str())) {
                    bool isAllSelected = (chan.appIdx == 0 && chan.routedAppName.empty());
                    if (ImGui::Selectable("[All System Audio]", isAllSelected)) {
                        chan.appIdx = 0;
                        ApplyStreamAppRouting(chIdx);
                        SaveConfig();
                    }

                    for (int s = 0; s < (int)m_activeAppSessions.size(); ++s) {
                        const auto& sess = m_activeAppSessions[s];
                        bool isSelected = (chan.appIdx == (s + 1));

                        int routedToOther = -1;
                        for (size_t other = 0; other < m_channels.size(); ++other) {
                            if (other != chIdx && (m_channels[other].routedAppPid == sess.pid || 
                                (!m_channels[other].routedAppName.empty() && m_channels[other].routedAppName == sess.processName))) {
                                routedToOther = static_cast<int>(other);
                                break;
                            }
                        }

                        std::string itemText = WStringToUTF8(sess.displayName);
                        if (routedToOther != -1) {
                            itemText += " -> (" + m_channels[routedToOther].label + ")";
                        }

                        if (ImGui::Selectable(itemText.c_str(), isSelected)) {
                            chan.appIdx = s + 1;
                            ApplyStreamAppRouting(chIdx);
                            SaveConfig();
                        }
                    }
                    ImGui::EndCombo();
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::AlignTextToFramePadding();
                if (chan.appIdx > 0 || !chan.routedAppName.empty()) {
                    std::string clearBtnId = "Reset App Route##ClearApp_" + std::to_string(chIdx);
                    if (ImGui::SmallButton(clearBtnId.c_str())) {
                        chan.appIdx = 0;
                        ApplyStreamAppRouting(chIdx);
                        SaveConfig();
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("Clear this app routing and return to default audio");
                    }
                } else {
                    ImGui::TextDisabled("Plays system audio");
                }
            }

            ImGui::EndTable();
        }
        ImGui::PopStyleVar(); // CellPadding

        ImGui::EndChild();
        ImGui::PopStyleVar(3); // ChildRounding, ChildBorderSize, WindowPadding
        ImGui::PopStyleColor(2); // ChildBg, Border
    }

    // Handle channel removal if requested
    if (channelToRemove >= 2 && channelToRemove < (int)m_channels.size()) {
        if (m_channels[channelToRemove].pipeline) {
            m_engine.UnregisterPipeline(m_channels[channelToRemove].pipeline);
            m_channels[channelToRemove].pipeline->Shutdown();
        }
        if (m_channels[channelToRemove].routedAppPid != 0) {
            m_engine.ResetAppRouting(m_channels[channelToRemove].routedAppPid);
        }
        m_channels.erase(m_channels.begin() + channelToRemove);
        SaveConfig();
    }

    // Full-width sleek "+ Add Stream Device" card button matching stream cards
    ImGui::Spacing();
    const float addBtnWidth = fullWidth;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.13f, 0.16f, 0.22f, 0.75f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.30f, 0.46f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.10f, 0.45f, 0.85f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.26f, 0.34f, 0.48f, 0.65f));

    bool addTriggered = ImGui::Button("+  Add Stream Device", ImVec2(addBtnWidth, addBtnHeight));
    if (!addTriggered && (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd))) {
        addTriggered = true;
    }

    if (addTriggered) {
        char streamLetter = static_cast<char>('A' + m_channels.size());
        StreamChannel newChan;
        newChan.label = std::string("Stream ") + streamLetter;

        // Pick the first available device that is not currently assigned to another channel
        int chosenDevice = 0;
        for (int d = 0; d < (int)m_devices.size(); ++d) {
            bool inUse = false;
            for (const auto& ch : m_channels) {
                if (ch.deviceIdx == d) {
                    inUse = true;
                    break;
                }
            }
            if (!inUse) {
                chosenDevice = d;
                break;
            }
        }
        newChan.deviceIdx = chosenDevice;
        if (!m_devices.empty() && chosenDevice >= 0 && chosenDevice < (int)m_devices.size()) {
            newChan.deviceId = m_devices[chosenDevice].id;
        }
        newChan.volume = 0.80f;
        newChan.syncDelayMs = 0;

        // Distinct stylish accent colors for additional streams
        static const ImVec4 s_accents[] = {
            ImVec4(0.40f, 0.90f, 0.60f, 1.0f), // Mint Green
            ImVec4(0.95f, 0.45f, 0.85f, 1.0f), // Magenta
            ImVec4(0.95f, 0.85f, 0.35f, 1.0f), // Gold
            ImVec4(0.45f, 0.65f, 1.00f, 1.0f)  // Azure
        };
        size_t colorIdx = (m_channels.size() - 2) % 4;
        newChan.accentColor = s_accents[colorIdx];

        std::wstring wPipeName = L"STREAM " + std::wstring(1, streamLetter);
        newChan.pipeline = std::make_shared<AudioPipeline>(wPipeName);
        newChan.pipeline->SetDeviceId(newChan.deviceId);
        newChan.pipeline->SetVolume(newChan.volume);
        newChan.pipeline->SetDelayMs(newChan.syncDelayMs);
        m_engine.RegisterPipeline(newChan.pipeline);
        if (m_engine.IsStreaming() && !newChan.deviceId.empty()) {
            m_engine.SelectPipelineDevice(newChan.pipeline, newChan.deviceId);
            newChan.pipeline->SetVolume(newChan.volume);
            newChan.pipeline->SetDelayMs(newChan.syncDelayMs);
        }
        m_channels.push_back(newChan);
        SaveConfig();
        if (m_channels.size() > 3) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Add an additional synchronized audio output stream device (Stream C, D, ...)\nShortcut: '+' key");
    }

    ImGui::EndChild(); // End of StreamsScrollRegion
    ImGui::PopStyleVar(); // WindowPadding for StreamsScrollRegion

    ImGui::Separator();

    // Latency Calibration Section (Item 7)
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f), "Latency Calibration");

    // Row: Input Device (Mic) & RAW Mode indicator on the same row (Item 8)
    float micRowStartY = ImGui::GetCursorPosY();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Mic Input:");
    ImGui::SameLine();

    const float labelWidth = ImGui::CalcTextSize("Mic Input:").x;
    const float spacingX = ImGui::GetStyle().ItemSpacing.x;
    const float micComboWidth = colWidth - labelWidth - spacingX;

    ImGui::SetNextItemWidth(micComboWidth);
    auto formatMicLabel = [](const AudioDevice& dev) {
        std::string label = WStringToUTF8(dev.name);
        std::wstring lower = dev.name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
        if (lower.find(L"microphone array") != std::wstring::npos || lower.find(L"mic array") != std::wstring::npos) {
            label += " [Built-in]";
        }
        if (dev.isDefault) label += " [WIN DEFAULT]";
        if (dev.isBluetooth) label += " [BT]";
        return label;
    };
    if (m_selectedMicDevice < 0 || m_selectedMicDevice >= (int)m_inputDevices.size()) {
        m_selectedMicDevice = 0;
    }
    std::string curMic = m_inputDevices.empty() ? "No Microphones Active" : formatMicLabel(m_inputDevices[m_selectedMicDevice]);
    if (ImGui::BeginCombo("##CalibMic", curMic.c_str())) {
        for (int i = 0; i < (int)m_inputDevices.size(); ++i) {
            bool isSelected = (m_selectedMicDevice == i);
            std::string mName = formatMicLabel(m_inputDevices[i]);
            if (ImGui::Selectable(mName.c_str(), isSelected)) {
                m_selectedMicDevice = i;
                m_engine.SelectMicDevice(m_inputDevices[i].id);
                SaveConfig();
            }
        }
        ImGui::EndCombo();
    }

    // RAW Audio Indicator aligned with Column B, vertically centered with combo
    ImGui::SetCursorPos(ImVec2(colB_X, micRowStartY));
    bool rawActive = m_engine.IsMicRawActive();
    DrawLED(rawActive,
            rawActive ? "RAW Audio Mode" : "Standard Audio Mode",
            rawActive ? "Direct unsuppressed hardware capture. Windows/OEM noise gates and voice filters are bypassed." : "Standard audio capture mode.",
            ImVec4(0.25f, 0.90f, 0.45f, 1.0f),
            ImVec4(0.60f, 0.60f, 0.60f, 1.0f));

    // Cleanly advance cursor to the next line at Column A
    ImGui::SetCursorPosY(micRowStartY + ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y);

    // Row: Live Mic Level meter and Sensitivity on the same row (Items 1.2, 1.3, 9)
    float micVU = m_engine.GetMicPeakVU();
    float micGain = m_engine.GetMicGain();

    ImGui::Text("Mic Level:");
    ImGui::SameLine();
    float micLabelWidth = ImGui::CalcTextSize("Mic Level:").x;
    float barWidth = colWidth - micLabelWidth - spacingX;
    if (barWidth < 120.0f) barWidth = 120.0f;
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, (micVU > 0.05f) ? ImVec4(0.2f, 0.85f, 0.35f, 1.0f) : ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
    ImGui::ProgressBar(micVU, ImVec2(barWidth, 16), "");
    ImGui::PopStyleColor();

    // Sensitivity on the same row, aligned with Column B (Items 1.3, 9)
    ImGui::SameLine(colB_X);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Sensitivity:");
    ImGui::SameLine();
    float frameH = ImGui::GetFrameHeight();
    if (ImGui::Button("-##MicSensDown", ImVec2(frameH, frameH))) {
        float step = (micGain <= 1.05f) ? 0.1f : 0.5f;
        float newGain = (std::max)(0.1f, micGain - step);
        m_engine.SetMicGain(newGain);
        SaveConfig();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(55);
    if (ImGui::InputFloat("##MicGainInput", &micGain, 0.0f, 0.0f, "%.1fx")) {
        if (micGain < 0.1f) micGain = 0.1f;
        if (micGain > 20.0f) micGain = 20.0f;
        m_engine.SetMicGain(micGain);
        SaveConfig();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Microphone acoustic gain multiplier. Click to type (0.1x to 20.0x).");
    }
    ImGui::SameLine();
    if (ImGui::Button("+##MicSensUp", ImVec2(frameH, frameH))) {
        float step = (micGain < 1.0f) ? 0.1f : 0.5f;
        float newGain = (std::min)(20.0f, micGain + step);
        m_engine.SetMicGain(newGain);
        SaveConfig();
    }

    ImGui::Spacing();

    // Calibration Action Buttons (Aligned with Column A and Column B)
    bool calibrating = m_engine.IsCalibrating();
    if (calibrating) {
        ImGui::BeginDisabled();
    }

    const float infoBtnWidth = 32.0f;
    const float autoSyncBtnWidth = colWidth - infoBtnWidth - ImGui::GetStyle().ItemSpacing.x;

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.55f, 0.75f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.68f, 0.90f, 1.0f));
    if (ImGui::Button("  1-Click Auto-Sync  ", ImVec2(autoSyncBtnWidth, 32))) {
        std::vector<int> activeChannels;
        for (size_t i = 0; i < m_channels.size(); ++i) {
            if (m_channels[i].deviceIdx >= 0 && m_channels[i].deviceIdx < (int)m_devices.size()) {
                activeChannels.push_back(static_cast<int>(i));
                if (i == 0) m_engine.SelectDeviceA(m_channels[i].deviceId);
                else if (i == 1) m_engine.SelectDeviceB(m_channels[i].deviceId);
                else if (m_channels[i].pipeline) m_engine.SelectPipelineDevice(m_channels[i].pipeline, m_channels[i].deviceId);
            }
        }

        m_autoSyncDone = false;
        m_autoSyncDelta = 0;

        if (activeChannels.size() <= 2) {
            m_calibMsg = "Firing simultaneous orthogonal chirps (Stream A: 700-1400Hz, Stream B: 3200-5200Hz)...";
            AddTerminalLog("Auto-Sync: Firing dual orthogonal acoustic chirps (A: 700-1400Hz, B: 3200-5200Hz)...");
            m_engine.AutoSyncBothChannels([this](bool success, int deltaMs, const std::string& msg) {
                m_autoSyncDone = success;
                m_autoSyncDelta = deltaMs;
                m_calibMsg = msg;
                AddTerminalLog(msg);
                if (success) {
                    if (deltaMs > 0) {
                        m_syncOffsetA = 0;
                        m_syncOffsetB = (std::min)(deltaMs, 2000);
                    } else if (deltaMs < 0) {
                        m_syncOffsetA = (std::min)(-deltaMs, 2000);
                        m_syncOffsetB = 0;
                    } else {
                        m_syncOffsetA = 0;
                        m_syncOffsetB = 0;
                    }
                    if (m_channels.size() > 0) {
                        m_channels[0].syncDelayMs = m_syncOffsetA;
                        if (m_channels[0].pipeline) m_channels[0].pipeline->SetDelayMs(m_syncOffsetA);
                    }
                    if (m_channels.size() > 1) {
                        m_channels[1].syncDelayMs = m_syncOffsetB;
                        if (m_channels[1].pipeline) m_channels[1].pipeline->SetDelayMs(m_syncOffsetB);
                    }
                    SaveConfig();
                }
            });
        } else {
            m_calibMsg = "Calibrating " + std::to_string(activeChannels.size()) + " channels sequentially via mic...";
            AddTerminalLog("Auto-Sync: Initiating sequential acoustic scan across " + std::to_string(activeChannels.size()) + " channels...");
            m_engine.AutoSyncChannels(activeChannels, [this, activeChannels](bool success, const std::vector<int>& delays, const std::string& msg) {
                m_autoSyncDone = success;
                m_calibMsg = msg;
                AddTerminalLog(msg);
                if (success && delays.size() == activeChannels.size()) {
                    for (size_t k = 0; k < activeChannels.size(); ++k) {
                        int chIdx = activeChannels[k];
                        int d = (std::min)(delays[k], 2000);
                        m_channels[chIdx].syncDelayMs = d;
                        if (m_channels[chIdx].pipeline) {
                            m_channels[chIdx].pipeline->SetDelayMs(d);
                        }
                        if (chIdx == 0) m_syncOffsetA = d;
                        if (chIdx == 1) m_syncOffsetB = d;
                    }
                    SaveConfig();
                }
            });
        }
    }
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Measures acoustic arrival times via microphone and perfectly time-aligns all connected devices.\n2 devices: Simultaneous dual-chirp (zero mic distance error).\n3+ devices: Sequential multi-pulse acoustic calibration.");
    }

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.28f, 0.40f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.40f, 0.58f, 1.0f));
    if (ImGui::Button("?##AutoSyncHelp", ImVec2(infoBtnWidth, 32))) {
        ImGui::OpenPopup("Auto-Sync Guide##Help");
    }
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Click for instructions on how to run Auto-Sync and how multi-channel acoustic calibration works.");
    }

    ImGui::SameLine(colB_X);

    bool metRunning = m_engine.IsMetronomeRunning();
    if (metRunning) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.35f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.45f, 0.20f, 1.0f));
        if (ImGui::Button("  Stop Metronome Clapper  ", ImVec2(colWidth, 32))) {
            m_engine.StopMetronome();
            m_metronomeEnabled = false;
            AddTerminalLog("Metronome Sync Clapper stopped.");
        }
        ImGui::PopStyleColor(2);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.50f, 0.35f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.62f, 0.42f, 1.0f));
        if (ImGui::Button("  Metronome Sync Clapper  ", ImVec2(colWidth, 32))) {
            float master = m_engine.IsMuted() ? 0.0f : m_engine.GetMasterVolume();
            for (size_t i = 0; i < m_channels.size(); ++i) {
                auto& chan = m_channels[i];
                if (chan.deviceIdx >= 0 && chan.deviceIdx < (int)m_devices.size()) {
                    if (i == 0) m_engine.SelectDeviceA(m_devices[chan.deviceIdx].id);
                    else if (i == 1) m_engine.SelectDeviceB(m_devices[chan.deviceIdx].id);
                    else if (chan.pipeline) m_engine.SelectPipelineDevice(chan.pipeline, m_devices[chan.deviceIdx].id);
                }
                if (chan.pipeline) {
                    chan.pipeline->SetVolume(chan.volume);
                    chan.pipeline->SetDelayMs(chan.syncDelayMs);
                    chan.pipeline->SetMasterVolume(master);
                }
            }

            m_engine.StartMetronome(500);
            m_metronomeEnabled = true;
            AddTerminalLog("Metronome Sync Clapper active @ 120 BPM across all streams.");
        }
        ImGui::PopStyleColor(2);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Fires a 15ms woodblock click every 500ms through both channels with current delay offsets.\nIn sync: you hear 1 crisp click.\nOut of sync: you hear a split double click ('ka-klap').");
    }

    if (calibrating) {
        ImGui::EndDisabled();
    }

    // Capture real-time calibration progress messages into terminal log
    std::string currentCalibStatus = m_engine.GetCalibrationStatus();
    if (!currentCalibStatus.empty() && currentCalibStatus != m_lastCalibStatus) {
        m_lastCalibStatus = currentCalibStatus;
        AddTerminalLog(currentCalibStatus);
    }

    ImGui::Spacing();

    // Terminal-Style Status & Scan Console Window
    const float terminalHeight = 56.0f;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.06f, 0.08f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.18f, 0.24f, 0.34f, 0.75f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));

    ImGui::BeginChild("TerminalLogWindow", ImVec2(0, terminalHeight), true, ImGuiWindowFlags_None);

    for (size_t i = 0; i < m_terminalLogs.size(); ++i) {
        const auto& log = m_terminalLogs[i];
        if (log.find("[SYNC APPLIED]") != std::string::npos || log.find("Multi-Sync Applied") != std::string::npos) {
            ImGui::TextColored(ImVec4(0.25f, 0.92f, 0.45f, 1.0f), "> %s", log.c_str());
        } else if (log.find("[CALIBRATING") != std::string::npos || log.find("Calibrating") != std::string::npos || log.find("Auto-Sync:") != std::string::npos || log.find("Acoustic arrival") != std::string::npos) {
            ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.20f, 1.0f), "> %s", log.c_str());
        } else if (log.find("Error") != std::string::npos || log.find("failed") != std::string::npos || log.find("not detected") != std::string::npos || log.find("incomplete") != std::string::npos) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "> %s", log.c_str());
        } else {
            ImGui::TextColored(ImVec4(0.65f, 0.75f, 0.85f, 1.0f), "> %s", log.c_str());
        }
    }

    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20.0f) {
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);

    if (!streaming) {
        ImGui::EndDisabled();
    }

    // Auto-Sync Info / Educational Modal Popup (Item 4)
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Auto-Sync Guide##Help", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.0f, 1.0f), "How to Run Auto-Sync Calibration");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::BulletText("Hold both earbuds/headphones close to your microphone (5-10 cm).");
        ImGui::BulletText("Ensure room noise is relatively quiet.");
        ImGui::BulletText("Click '1-Click Auto-Sync' and hold still for ~2.5 seconds.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f), "How Dual-Chirp Cross-Correlation Works");
        ImGui::Spacing();
        ImGui::BulletText("Orthogonal Frequency Chirps:");
        ImGui::TextDisabled("   Stream A fires a low chirp (700 Hz - 1400 Hz).");
        ImGui::TextDisabled("   Stream B fires a high chirp (3200 Hz - 5200 Hz).");
        ImGui::TextDisabled("   Both chirps are fired at the exact same millisecond (T=0).");
        ImGui::Spacing();
        ImGui::BulletText("Hardware Latency Cancellation:");
        ImGui::TextDisabled("   Because both chirps travel into the SAME microphone and USB buffer,");
        ImGui::TextDisabled("   the microphone's internal latency cancels out mathematically!");
        ImGui::Spacing();
        ImGui::BulletText("Peak Delta Measurement (GCC-PHAT):");
        ImGui::TextDisabled("   Phase Transform cross-correlation finds the arrival difference.");
        ImGui::TextDisabled("   Auduo then delays the faster stream so both sound simultaneously.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (ImGui::Button("Got It!", ImVec2(100, 28))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // Startup Donation Modal Dialog
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(540, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Support AuDuo##DonationModal", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.35f, 1.0f), "Support AuDuo Development");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::PushTextWrapPos(510.0f);
        ImGui::TextUnformatted("this project was made open source because once upon a time i too was a broke student looking for a way to watch movies or listen to music with my friends or loved ones. I'm still broke but i've built the software. so please consider donating");
        ImGui::PopTextWrapPos();

        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        if (ImGui::Checkbox("Don't show this again", &m_dontShowDonationAgain)) {
            SaveConfig();
        }

        ImGui::Spacing();
        ImGui::Spacing();

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.55f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.65f, 0.25f, 1.0f));
        if (ImGui::Button("Support / Donate", ImVec2(150, 30))) {
            OpenDonationPage();
        }
        ImGui::PopStyleColor(2);

        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(100, 30))) {
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    ImGui::Separator();

    // Footer actions (Aligned with Column A and Column B)
    float colA_btnWidth = (colWidth - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;

    if (ImGui::Button("Test Beep", ImVec2(colA_btnWidth, 0))) {
        m_engine.PlayTestBeep();
        AddTerminalLog("Test Chime: Fired 250ms alignment pulse across active streams.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(colA_btnWidth, 0))) {
        m_engine.ResetAndRefresh();
        RefreshDeviceList();
        for (size_t i = 0; i < m_channels.size(); ++i) {
            auto& chan = m_channels[i];
            if (chan.deviceIdx >= 0 && chan.deviceIdx < (int)m_devices.size()) {
                chan.deviceId = m_devices[chan.deviceIdx].id;
                if (i == 0) m_engine.SelectDeviceA(chan.deviceId);
                else if (i == 1) m_engine.SelectDeviceB(chan.deviceId);
                else if (chan.pipeline) m_engine.SelectPipelineDevice(chan.pipeline, chan.deviceId);
            }
            if (chan.pipeline) {
                chan.pipeline->SetVolume(chan.volume);
                chan.pipeline->SetDelayMs(chan.syncDelayMs);
            }
        }
        if (m_audioRoutingEnabled) {
            RefreshAppSessionList();
            SyncAllAppRouting();
        }
        AddTerminalLog("Backend reset: pipelines flushed and audio devices refreshed.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Minimize to Tray", ImVec2(colA_btnWidth, 0))) {
        m_minimizeToTray = true;
    }

    // Column B footer controls: Checkbox and Support / Donate button (standard button styling)
    ImGui::SameLine(colB_X);
    ImGui::AlignTextToFramePadding();
    if (ImGui::Checkbox("Always minimize to tray", &m_alwaysMinimizeToTray)) {
        SaveConfig();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("When checked, clicking the window close [X] button hides Auduo to the system tray instead of exiting.");
    }

    const float donateBtnWidth = 140.0f;
    ImGui::SameLine(colB_X + colWidth - donateBtnWidth);
    if (ImGui::Button("Support / Donate", ImVec2(donateBtnWidth, 0))) {
        OpenDonationPage();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Support AuDuo open-source development (UPI, Cards & International)");
    }

    ImGui::End();

    ImGui::Render();
}

} // namespace Auduo
