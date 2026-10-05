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
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], sizeNeeded, NULL, NULL);
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
    LoadConfig();

    if (!m_devices.empty()) {
        if (m_selectedDeviceA >= 0 && m_selectedDeviceA < (int)m_devices.size()) {
            m_engine.SelectDeviceA(m_devices[m_selectedDeviceA].id);
        }
        if (m_selectedDeviceB >= 0 && m_selectedDeviceB < (int)m_devices.size()) {
            m_engine.SelectDeviceB(m_devices[m_selectedDeviceB].id);
        }
    }
    if (!m_inputDevices.empty()) {
        if (m_selectedMicDevice >= 0 && m_selectedMicDevice < (int)m_inputDevices.size()) {
            m_engine.SelectMicDevice(m_inputDevices[m_selectedMicDevice].id);
        }
    }
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

void UI::Shutdown() {
    if (!m_initialized) return;
    m_engine.StopMicMonitor();
    SaveConfig();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    m_initialized = false;
}

void UI::RefreshDeviceList() {
    m_devices = AudioEngine::EnumerateOutputDevices();
    if (m_devices.empty()) return;

    if (m_selectedDeviceA >= (int)m_devices.size()) {
        m_selectedDeviceA = 0;
    }
    if (m_selectedDeviceB >= (int)m_devices.size() || m_selectedDeviceB == m_selectedDeviceA) {
        m_selectedDeviceB = (m_devices.size() > 1) ? ((m_selectedDeviceA == 0) ? 1 : 0) : 0;
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
            else if (key == "sync_a") m_syncOffsetA = std::clamp(std::stoi(val), 0, 500);
            else if (key == "sync_b") m_syncOffsetB = std::clamp(std::stoi(val), 0, 500);
            else if (key == "device_a_idx") m_selectedDeviceA = std::stoi(val);
            else if (key == "device_b_idx") m_selectedDeviceB = std::stoi(val);
            else if (key == "device_a_id") {
                int sizeNeeded = MultiByteToWideChar(CP_UTF8, 0, val.c_str(), (int)val.size(), NULL, 0);
                if (sizeNeeded > 0) {
                    std::wstring wId(sizeNeeded, 0);
                    MultiByteToWideChar(CP_UTF8, 0, val.c_str(), (int)val.size(), &wId[0], sizeNeeded);
                    for (int i = 0; i < (int)m_devices.size(); ++i) {
                        if (m_devices[i].id == wId) {
                            m_selectedDeviceA = i;
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
                    for (int i = 0; i < (int)m_devices.size(); ++i) {
                        if (m_devices[i].id == wId) {
                            m_selectedDeviceB = i;
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
    float lineHeight = ImGui::GetTextLineHeight();
    ImVec2 center(p.x + radius + 1.0f, p.y + lineHeight * 0.5f);

    ImVec4 chosenColor = active ? onColor : offColor;
    ImU32 col = ImGui::ColorConvertFloat4ToU32(chosenColor);

    if (active) {
        ImVec4 glowCol = chosenColor;
        glowCol.w = 0.35f;
        drawList->AddCircleFilled(center, radius + 3.0f, ImGui::ColorConvertFloat4ToU32(glowCol));
    }
    drawList->AddCircleFilled(center, radius, col);

    ImGui::Dummy(ImVec2(radius * 2.0f + 2.0f, lineHeight));
    ImGui::SameLine(0.0f, 6.0f);
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

    // Row 1: Button on the left (Column A), Status on the right (Column B)
    const float streamBtnHeight = 32.0f;
    bool streaming = m_engine.IsStreaming();
    if (!streaming) {
        if (!cableDefault) {
            ImGui::BeginDisabled();
        }
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.60f, 0.25f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.75f, 0.32f, 1.0f));
        if (ImGui::Button("  START STREAMING  ", ImVec2(colWidth, streamBtnHeight))) {
            if (!m_devices.empty()) {
                if (m_selectedDeviceA >= 0 && m_selectedDeviceA < (int)m_devices.size()) {
                    m_engine.SelectDeviceA(m_devices[m_selectedDeviceA].id);
                }
                if (m_selectedDeviceB >= 0 && m_selectedDeviceB < (int)m_devices.size()) {
                    m_engine.SelectDeviceB(m_devices[m_selectedDeviceB].id);
                }
            }
            m_engine.SetMirrorSystemAudio(true);
            m_engine.GetPipelineA()->SetVolume(m_volA);
            m_engine.GetPipelineB()->SetVolume(m_volB);
            m_engine.GetPipelineA()->SetDelayMs(m_syncOffsetA);
            m_engine.GetPipelineB()->SetDelayMs(m_syncOffsetB);
            m_engine.StartStreaming();
        }
        ImGui::PopStyleColor(2);
        if (!cableDefault) {
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Set 'CABLE Input' as your Windows Default Playback Device to enable streaming.");
            }
        }
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.20f, 0.20f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.25f, 0.25f, 1.0f));
        if (ImGui::Button("  STOP STREAMING  ", ImVec2(colWidth, streamBtnHeight))) {
            m_engine.StopStreaming();
        }
        ImGui::PopStyleColor(2);
    }

    // Column B: Status LED aligned with Column B (starting at colB_X)
    ImGui::SameLine(colB_X);
    float ledYOffset = (streamBtnHeight - ImGui::GetTextLineHeight()) * 0.5f;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ledYOffset);
    if (cableDefault) {
        DrawLED(true, "Virtual Cable Ready",
                "VB-Audio Virtual Cable is connected and set as the Windows Default Playback Device.",
                ImVec4(0.25f, 0.90f, 0.45f, 1.0f),
                ImVec4(0.95f, 0.25f, 0.25f, 1.0f));
    } else {
        DrawLED(false, "Virtual Cable Not Connected",
                "VB-Audio Virtual Cable is not set as your Windows Default Playback Device.\nPlease set 'CABLE Input' as default in Windows Sound settings to enable streaming.",
                ImVec4(0.25f, 0.90f, 0.45f, 1.0f),
                ImVec4(0.95f, 0.25f, 0.25f, 1.0f));
    }
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ledYOffset);

    ImGui::Spacing();
    ImGui::Separator();

    // Grey out the rest of the application controls if streaming hasn't been started
    if (!streaming) {
        ImGui::BeginDisabled();
    }

    // Periodic Windows Volume Sync check (every ~30ms for instant Fn key responsiveness)
    static auto lastVolCheck = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastVolCheck).count() > 30) {
        lastVolCheck = now;
        m_engine.UpdateWindowsVolumeSync();
    }

    // Master Volume Bar aligned with Column A
    ImGui::Text("Master Volume:");
    ImGui::SameLine();
    float mvSliderStart = ImGui::GetCursorPosX();
    float mvSliderWidth = colWidth - (mvSliderStart - colStartX) - 52.0f;
    if (mvSliderWidth < 120.0f) mvSliderWidth = 120.0f;
    ImGui::SetNextItemWidth(mvSliderWidth);
    int masterPercent = static_cast<int>(std::round(m_engine.GetMasterVolume() * 100.0f));
    if (ImGui::SliderInt("##MasterVol", &masterPercent, 0, 100, "%d%%")) {
        m_masterVol = masterPercent / 100.0f;
        m_engine.SetMasterVolume(m_masterVol);
    }
    ImGui::SameLine();
    if (m_engine.IsMuted()) {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "[MUTED]");
    } else {
        ImGui::TextDisabled("(%d%%)", masterPercent);
    }

    // Checkbox aligned directly with Column B
    ImGui::SameLine(colB_X);
    if (ImGui::Checkbox("Sync with PC Volume Keys", &m_syncWindowsVol)) {
        m_engine.SetSyncWithWindowsVolume(m_syncWindowsVol);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("When checked, laptop physical volume keys (Fn+Vol) and Windows taskbar slider automatically scale both audio streams.");
    }

    ImGui::Separator();

    // Twin Channel Panels (Stream A & Stream B)
    const float panelHeight = 260.0f;
    ImGuiWindowFlags childFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

    // Stream A Panel
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
    ImGui::BeginChild("StreamA_Panel", ImVec2(colWidth, panelHeight), true, childFlags);
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Stream A");
    if (m_engine.GetPipelineA() && m_engine.GetPipelineA()->HasDeviceError()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.25f, 1.0f), " [DISCONNECTED]");
    } else if (!m_devices.empty() && m_selectedDeviceA >= 0 && m_selectedDeviceA < (int)m_devices.size() && m_devices[m_selectedDeviceA].isDefault) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.0f, 0.85f), " [Win Default Passthrough]");
    }
    ImGui::Separator();

    ImGui::Text("Output Device (OUT):");
    ImGui::SetNextItemWidth(-1.0f);
    std::string curDevA = m_devices.empty() ? "None" : (WStringToUTF8(m_devices[m_selectedDeviceA].name) + (m_devices[m_selectedDeviceA].isDefault ? " [WIN DEFAULT]" : ""));
    if (ImGui::BeginCombo("##DeviceA", curDevA.c_str())) {
        for (int i = 0; i < (int)m_devices.size(); ++i) {
            bool isSelected = (m_selectedDeviceA == i);
            std::string dName = WStringToUTF8(m_devices[i].name) + (m_devices[i].isDefault ? " [WIN DEFAULT]" : "") + (m_devices[i].isBluetooth ? " [BT]" : "");
            if (ImGui::Selectable(dName.c_str(), isSelected)) {
                m_selectedDeviceA = i;
                m_engine.SelectDeviceA(m_devices[i].id);
                SaveConfig();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Text("Volume: %.0f%%", m_volA * 100.0f);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("##VolA", &m_volA, 0.0f, 1.0f, "")) {
        m_engine.GetPipelineA()->SetVolume(m_volA);
        SaveConfig();
    }

    ImGui::Spacing();
    float vuA = m_engine.GetPipelineA()->GetPeakVU();
    ImGui::Text("Live VU:");
    ImGui::SameLine();
    ImGui::ProgressBar(vuA, ImVec2(-1.0f, 14), "");

    ImGui::Spacing();
    ImGui::Text("Sync Delay: +%d ms", m_syncOffsetA);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderInt("##SyncA", &m_syncOffsetA, 0, 500, "%d ms")) {
        m_engine.GetPipelineA()->SetDelayMs(m_syncOffsetA);
        SaveConfig();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::SameLine(colB_X);

    // Stream B Panel
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
    ImGui::BeginChild("StreamB_Panel", ImVec2(colWidth, panelHeight), true, childFlags);
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Stream B");
    if (m_engine.GetPipelineB() && m_engine.GetPipelineB()->HasDeviceError()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.25f, 1.0f), " [DISCONNECTED]");
    } else if (!m_devices.empty() && m_selectedDeviceB >= 0 && m_selectedDeviceB < (int)m_devices.size() && m_devices[m_selectedDeviceB].isDefault) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.0f, 0.85f), " [Win Default Passthrough]");
    }
    ImGui::Separator();

    ImGui::Text("Output Device (OUT):");
    ImGui::SetNextItemWidth(-1.0f);
    std::string curDevB = m_devices.empty() ? "None" : (WStringToUTF8(m_devices[m_selectedDeviceB].name) + (m_devices[m_selectedDeviceB].isDefault ? " [WIN DEFAULT]" : ""));
    if (ImGui::BeginCombo("##DeviceB", curDevB.c_str())) {
        for (int i = 0; i < (int)m_devices.size(); ++i) {
            bool isSelected = (m_selectedDeviceB == i);
            std::string dName = WStringToUTF8(m_devices[i].name) + (m_devices[i].isDefault ? " [WIN DEFAULT]" : "") + (m_devices[i].isBluetooth ? " [BT]" : "");
            if (ImGui::Selectable(dName.c_str(), isSelected)) {
                m_selectedDeviceB = i;
                m_engine.SelectDeviceB(m_devices[i].id);
                SaveConfig();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::Text("Volume: %.0f%%", m_volB * 100.0f);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("##VolB", &m_volB, 0.0f, 1.0f, "")) {
        m_engine.GetPipelineB()->SetVolume(m_volB);
        SaveConfig();
    }

    ImGui::Spacing();
    float vuB = m_engine.GetPipelineB()->GetPeakVU();
    ImGui::Text("Live VU:");
    ImGui::SameLine();
    ImGui::ProgressBar(vuB, ImVec2(-1.0f, 14), "");

    ImGui::Spacing();
    ImGui::Text("Sync Delay: +%d ms", m_syncOffsetB);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderInt("##SyncB", &m_syncOffsetB, 0, 500, "%d ms")) {
        m_engine.GetPipelineB()->SetDelayMs(m_syncOffsetB);
        SaveConfig();
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::Spacing();
    ImGui::Separator();

    // Latency Calibration Section (Item 7)
    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f), "Latency Calibration");

    // Row: Input Device (Mic) & RAW Mode indicator on the same row (Item 8)
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

    // RAW Audio Indicator aligned with Column B (Item 8)
    ImGui::SameLine(colB_X);
    bool rawActive = m_engine.IsMicRawActive();
    DrawLED(rawActive,
            rawActive ? "RAW Audio Mode" : "Standard Audio Mode",
            rawActive ? "Direct unsuppressed hardware capture. Windows/OEM noise gates and voice filters are bypassed." : "Standard audio capture mode.",
            ImVec4(0.25f, 0.90f, 0.45f, 1.0f),
            ImVec4(0.60f, 0.60f, 0.60f, 1.0f));

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
        if (!m_devices.empty()) {
            if (m_selectedDeviceA >= 0 && m_selectedDeviceA < (int)m_devices.size()) {
                m_engine.SelectDeviceA(m_devices[m_selectedDeviceA].id);
            }
            if (m_selectedDeviceB >= 0 && m_selectedDeviceB < (int)m_devices.size()) {
                m_engine.SelectDeviceB(m_devices[m_selectedDeviceB].id);
            }
        }
        m_autoSyncDone = false;
        m_autoSyncDelta = 0;
        m_calibMsg = "Firing simultaneous orthogonal chirps (Stream A: 700-1400Hz, Stream B: 3200-5200Hz)...";
        m_engine.AutoSyncBothChannels([this](bool success, int deltaMs, const std::string& msg) {
            m_autoSyncDone = success;
            m_autoSyncDelta = deltaMs;
            m_calibMsg = msg;
            if (success) {
                if (deltaMs > 0) {
                    m_syncOffsetA = 0;
                    m_syncOffsetB = (std::min)(deltaMs, 500);
                } else if (deltaMs < 0) {
                    m_syncOffsetA = (std::min)(-deltaMs, 500);
                    m_syncOffsetB = 0;
                } else {
                    m_syncOffsetA = 0;
                    m_syncOffsetB = 0;
                }
                m_engine.GetPipelineA()->SetDelayMs(m_syncOffsetA);
                m_engine.GetPipelineB()->SetDelayMs(m_syncOffsetB);
                SaveConfig();
            }
        });
    }
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Fires orthogonal frequency chirps across Stream A and Stream B at the exact same instant (T=0).\nCancels out microphone latency and measures exact acoustic time-difference.");
    }

    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.28f, 0.40f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.40f, 0.58f, 1.0f));
    if (ImGui::Button("?##AutoSyncHelp", ImVec2(infoBtnWidth, 32))) {
        ImGui::OpenPopup("Auto-Sync Guide##Help");
    }
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Click for instructions on how to run Auto-Sync and how the dual-chirp calibration works.");
    }

    ImGui::SameLine(colB_X);

    bool metRunning = m_engine.IsMetronomeRunning();
    if (metRunning) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.35f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.45f, 0.20f, 1.0f));
        if (ImGui::Button("  Stop Metronome Clapper  ", ImVec2(colWidth, 32))) {
            m_engine.StopMetronome();
            m_metronomeEnabled = false;
        }
        ImGui::PopStyleColor(2);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.50f, 0.35f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.62f, 0.42f, 1.0f));
        if (ImGui::Button("  Metronome Sync Clapper  ", ImVec2(colWidth, 32))) {
            if (!m_devices.empty()) {
                if (m_selectedDeviceA >= 0 && m_selectedDeviceA < (int)m_devices.size()) {
                    m_engine.SelectDeviceA(m_devices[m_selectedDeviceA].id);
                }
                if (m_selectedDeviceB >= 0 && m_selectedDeviceB < (int)m_devices.size()) {
                    m_engine.SelectDeviceB(m_devices[m_selectedDeviceB].id);
                }
            }
            m_engine.GetPipelineA()->SetVolume(m_volA);
            m_engine.GetPipelineB()->SetVolume(m_volB);
            m_engine.GetPipelineA()->SetDelayMs(m_syncOffsetA);
            m_engine.GetPipelineB()->SetDelayMs(m_syncOffsetB);
            float master = m_engine.IsMuted() ? 0.0f : m_engine.GetMasterVolume();
            m_engine.GetPipelineA()->SetMasterVolume(master);
            m_engine.GetPipelineB()->SetMasterVolume(master);

            m_engine.StartMetronome(500);
            m_metronomeEnabled = true;
        }
        ImGui::PopStyleColor(2);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Fires a 15ms woodblock click every 500ms through both channels with current delay offsets.\nIn sync: you hear 1 crisp click.\nOut of sync: you hear a split double click ('ka-klap').");
    }

    if (calibrating) {
        ImGui::EndDisabled();
    }

    // Status / Helper text line
    if (calibrating) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "[CALIBRATING...] %s", m_engine.GetCalibrationStatus().c_str());
    } else if (m_autoSyncDone) {
        ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.4f, 1.0f), "[SYNC APPLIED] %s", m_calibMsg.c_str());
    } else if (!m_calibMsg.empty()) {
        ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1.0f), "Status: %s", m_calibMsg.c_str());
    } else {
        ImGui::TextDisabled("Status: Hold earbuds near mic, click '1-Click Auto-Sync', or use Metronome to verify.");
    }

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
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(colA_btnWidth, 0))) {
        RefreshDeviceList();
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
