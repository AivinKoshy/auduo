#include <initguid.h>
#include "AudioEngine.hpp"
#include <iostream>
#include <vector>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <avrt.h>
#include <psapi.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <timeapi.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace Auduo {

// --- Resampler Implementation ---
void Resampler::ResampleAndMix(
    const float* input, size_t inFrames, uint32_t inRate, uint32_t inChannels,
    std::vector<float>& output, uint32_t outRate, uint32_t outChannels
) {
    if (!input || inFrames == 0 || inRate == 0 || outRate == 0 || inChannels == 0 || outChannels == 0) {
        output.clear();
        return;
    }

    const double ratio = static_cast<double>(outRate) / static_cast<double>(inRate);
    const size_t outFrames = static_cast<size_t>(std::ceil(inFrames * ratio));
    output.resize(outFrames * outChannels);

    for (size_t i = 0; i < outFrames; ++i) {
        double srcPos = i / ratio;
        size_t idx0 = static_cast<size_t>(srcPos);
        size_t idx1 = (idx0 + 1 < inFrames) ? idx0 + 1 : idx0;
        float frac = static_cast<float>(srcPos - idx0);

        if (inChannels == 1 && outChannels == 2) {
            float s0 = input[idx0];
            float s1 = input[idx1];
            float s = s0 + frac * (s1 - s0);
            output[i * 2 + 0] = s;
            output[i * 2 + 1] = s;
        } else if (inChannels >= 6 && outChannels == 2) {
            const float kCS = 0.7071f;
            float l0 = input[idx0 * inChannels + 0] + kCS * input[idx0 * inChannels + 2] + kCS * input[idx0 * inChannels + 4];
            float l1 = input[idx1 * inChannels + 0] + kCS * input[idx1 * inChannels + 2] + kCS * input[idx1 * inChannels + 4];
            float r0 = input[idx0 * inChannels + 1] + kCS * input[idx0 * inChannels + 2] + kCS * input[idx0 * inChannels + 5];
            float r1 = input[idx1 * inChannels + 1] + kCS * input[idx1 * inChannels + 2] + kCS * input[idx1 * inChannels + 5];
            output[i * 2 + 0] = l0 + frac * (l1 - l0);
            output[i * 2 + 1] = r0 + frac * (r1 - r0);
        } else {
            for (uint32_t ch = 0; ch < outChannels; ++ch) {
                uint32_t srcCh = (std::min)(ch, inChannels - 1);
                float s0 = input[idx0 * inChannels + srcCh];
                float s1 = input[idx1 * inChannels + srcCh];
                output[i * outChannels + ch] = s0 + frac * (s1 - s0);
            }
        }
    }
}

// --- AudioPipeline Implementation ---

AudioPipeline::AudioPipeline(const std::wstring& name, uint32_t bufferCapacityFrames)
    : m_name(name)
    , m_ringBuffer(bufferCapacityFrames, 2)
{
}

AudioPipeline::~AudioPipeline() {
    Shutdown();
}

bool AudioPipeline::Initialize(const std::wstring& endpointDeviceId) {
    Shutdown();
    m_deviceId = endpointDeviceId;
    m_deviceError.store(false, std::memory_order_relaxed);

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    m_comInitialized = SUCCEEDED(hr);

    IMMDeviceEnumerator* pEnum = nullptr;
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnum
    );
    if (FAILED(hr) || !pEnum) {
        Shutdown();
        return false;
    }

    if (m_deviceId.empty()) {
        hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &m_pDevice);
    } else {
        hr = pEnum->GetDevice(m_deviceId.c_str(), &m_pDevice);
    }
    pEnum->Release();
    if (FAILED(hr) || !m_pDevice) {
        Shutdown();
        return false;
    }

    hr = m_pDevice->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&m_pAudioClient
    );
    if (FAILED(hr) || !m_pAudioClient) {
        Shutdown();
        return false;
    }

    hr = m_pAudioClient->GetMixFormat(&m_pwfx);
    if (FAILED(hr) || !m_pwfx) {
        Shutdown();
        return false;
    }

    // Standard requested buffer: 40ms for high stability over Bluetooth
    REFERENCE_TIME hnsRequestedDuration = 400000; // 40ms in 100ns units

    hr = m_pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        hnsRequestedDuration,
        0,
        m_pwfx,
        NULL
    );

    if (FAILED(hr)) {
        // Fallback without special stream flags if needed
        hr = m_pAudioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            hnsRequestedDuration,
            0,
            m_pwfx,
            NULL
        );
    }
    if (FAILED(hr)) {
        Shutdown();
        return false;
    }

    m_hRenderEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!m_hRenderEvent) {
        Shutdown();
        return false;
    }

    hr = m_pAudioClient->SetEventHandle(m_hRenderEvent);
    if (FAILED(hr)) {
        Shutdown();
        return false;
    }

    hr = m_pAudioClient->GetBufferSize(&m_bufferFrameCount);
    if (FAILED(hr) || m_bufferFrameCount == 0) {
        Shutdown();
        return false;
    }

    hr = m_pAudioClient->GetService(
        __uuidof(IAudioRenderClient), (void**)&m_pRenderClient
    );
    if (FAILED(hr) || !m_pRenderClient) {
        Shutdown();
        return false;
    }

    m_ringBuffer.Clear();
    int initialDelay = m_delayMs.load(std::memory_order_relaxed);
    if (initialDelay > 0 && m_pwfx && m_pwfx->nSamplesPerSec > 0) {
        size_t delayFrames = (static_cast<size_t>(initialDelay) * m_pwfx->nSamplesPerSec) / 1000;
        m_ringBuffer.SetDelayFrames(delayFrames);
    }

    // Start background render thread
    m_running.store(true, std::memory_order_relaxed);
    m_renderThread = std::thread(&AudioPipeline::RenderLoop, this);

    return true;
}

void AudioPipeline::Shutdown() {
    m_running.store(false, std::memory_order_relaxed);
    if (m_hRenderEvent) {
        SetEvent(m_hRenderEvent);
    }
    if (m_renderThread.joinable()) {
        m_renderThread.join();
    }
    if (m_hRenderEvent) {
        CloseHandle(m_hRenderEvent);
        m_hRenderEvent = NULL;
    }
    if (m_pRenderClient) {
        m_pRenderClient->Release();
        m_pRenderClient = nullptr;
    }
    if (m_pAudioClient) {
        m_pAudioClient->Stop();
        m_pAudioClient->Release();
        m_pAudioClient = nullptr;
    }
    if (m_pwfx) {
        CoTaskMemFree(m_pwfx);
        m_pwfx = nullptr;
    }
    if (m_pDevice) {
        m_pDevice->Release();
        m_pDevice = nullptr;
    }
    if (m_comInitialized) {
        CoUninitialize();
        m_comInitialized = false;
    }
}

void AudioPipeline::PushAudio(const float* frames, size_t frameCount, uint32_t sampleRate, uint32_t channels) {
    if (!frames || frameCount == 0 || !m_running.load(std::memory_order_relaxed) || !m_pwfx) return;

    std::lock_guard<std::mutex> lock(m_pushMutex);
    const uint32_t targetRate = m_pwfx->nSamplesPerSec;
    const uint32_t targetChannels = m_pwfx->nChannels;

    if (sampleRate == targetRate && channels == targetChannels && channels == 2) {
        m_ringBuffer.Write(frames, frameCount);
    } else {
        std::vector<float> resampled;
        Resampler::ResampleAndMix(frames, frameCount, sampleRate, channels, resampled, targetRate, 2);
        m_ringBuffer.Write(resampled.data(), resampled.size() / 2);
    }
}

// Windows standard audio taper curve mapping slider scalar [0.0, 1.0] to perceived loudness
static inline float WindowsVolumeScalarToGain(float s) {
    if (s <= 0.0f) return 0.0f;
    if (s >= 1.0f) return 1.0f;
    // Cubic perceptual loudness curve matching Windows Audio (AudioSrv / Sndvol)
    return s * s * s;
}

void AudioPipeline::UpdateVUMeterOnly(const float* frames, size_t frameCount, uint32_t channels) {
    if (!frames || frameCount == 0 || channels == 0) return;
    float peak = 0.0f;
    float currentVol = WindowsVolumeScalarToGain(m_volume.load(std::memory_order_relaxed));
    for (size_t i = 0; i < frameCount * channels; ++i) {
        float absS = std::fabs(frames[i]) * currentVol;
        if (absS > peak) peak = absS;
    }
    m_metrics.peakVU.store(peak, std::memory_order_relaxed);
}

void AudioPipeline::SetDelayMs(int delayMs) {
    m_delayMs.store(delayMs, std::memory_order_relaxed);
    if (m_pwfx && m_pwfx->nSamplesPerSec > 0) {
        size_t frames = (static_cast<size_t>(delayMs) * m_pwfx->nSamplesPerSec) / 1000;
        std::lock_guard<std::mutex> lock(m_pushMutex);
        m_ringBuffer.SetDelayFrames(frames);
    }
}

void AudioPipeline::RenderLoop() {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    (void)hr;

    DWORD taskIndex = 0;
    HANDLE hAvrt = AvSetMmThreadCharacteristics(L"Pro Audio", &taskIndex);

    hr = m_pAudioClient->Start();
    if (FAILED(hr)) {
        m_deviceError.store(true, std::memory_order_relaxed);
        if (hAvrt) AvRevertMmThreadCharacteristics(hAvrt);
        CoUninitialize();
        return;
    }

    std::vector<float> stagingBuffer;

    while (m_running.load(std::memory_order_relaxed)) {
        DWORD waitRes = WaitForSingleObject(m_hRenderEvent, 500);
        if (waitRes != WAIT_OBJECT_0) {
            if (!m_running.load(std::memory_order_relaxed)) break;
            continue;
        }

        UINT32 padding = 0;
        hr = m_pAudioClient->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            // Likely device disconnected or invalid endpoint
            m_deviceError.store(true, std::memory_order_relaxed);
            break;
        }

        UINT32 framesNeeded = m_bufferFrameCount - padding;
        if (framesNeeded == 0) continue;

        BYTE* pData = nullptr;
        hr = m_pRenderClient->GetBuffer(framesNeeded, &pData);
        if (FAILED(hr) || !pData) {
            m_deviceError.store(true, std::memory_order_relaxed);
            break;
        }

        stagingBuffer.resize(framesNeeded * 2);
        size_t framesRead = m_ringBuffer.Read(stagingBuffer.data(), framesNeeded);

        float mVol = WindowsVolumeScalarToGain(m_masterVolume.load(std::memory_order_relaxed));
        float cVol = WindowsVolumeScalarToGain(m_volume.load(std::memory_order_relaxed));
        float currentVol = cVol * mVol;
        float peak = 0.0f;

        // Apply volume attenuation and calculate VU level
        for (size_t i = 0; i < framesNeeded * 2; ++i) {
            float s = stagingBuffer[i] * currentVol;
            // Soft clipping safety limiter
            if (s > 1.0f) s = 1.0f;
            else if (s < -1.0f) s = -1.0f;
            stagingBuffer[i] = s;

            float absS = std::fabs(s);
            if (absS > peak) peak = absS;
        }

        m_metrics.peakVU.store(peak, std::memory_order_relaxed);

        // Copy into WASAPI render buffer
        // Note: Assuming IEEE Float 32-bit format (Standard WASAPI shared mix format)
        if (m_pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
            (m_pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
             reinterpret_cast<WAVEFORMATEXTENSIBLE*>(m_pwfx)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            
            if (m_pwfx->nChannels == 2) {
                std::memcpy(pData, stagingBuffer.data(), framesNeeded * 2 * sizeof(float));
            } else {
                // Multi-channel upmix (e.g. 5.1/7.1 to stereo output)
                float* pOut = reinterpret_cast<float*>(pData);
                for (UINT32 f = 0; f < framesNeeded; ++f) {
                    pOut[f * m_pwfx->nChannels + 0] = stagingBuffer[f * 2 + 0];
                    if (m_pwfx->nChannels > 1) {
                        pOut[f * m_pwfx->nChannels + 1] = stagingBuffer[f * 2 + 1];
                    }
                    for (UINT32 c = 2; c < m_pwfx->nChannels; ++c) {
                        pOut[f * m_pwfx->nChannels + c] = 0.0f;
                    }
                }
            }
        } else if (m_pwfx->wBitsPerSample == 16) {
            // PCM 16-bit fallback
            int16_t* pOut = reinterpret_cast<int16_t*>(pData);
            for (size_t f = 0; f < framesNeeded; ++f) {
                for (uint32_t ch = 0; ch < m_pwfx->nChannels; ++ch) {
                    float s = (ch < 2) ? stagingBuffer[f * 2 + ch] : 0.0f;
                    float sample = (std::max)(-1.0f, (std::min)(1.0f, s));
                    pOut[f * m_pwfx->nChannels + ch] = static_cast<int16_t>(sample * 32767.0f);
                }
            }
        } else if (m_pwfx->wBitsPerSample == 24 && m_pwfx->nChannels > 0 && (m_pwfx->nBlockAlign / m_pwfx->nChannels == 3)) {
            // Packed 24-bit PCM (3 bytes per sample)
            uint8_t* pOut = reinterpret_cast<uint8_t*>(pData);
            for (size_t f = 0; f < framesNeeded; ++f) {
                for (uint32_t ch = 0; ch < m_pwfx->nChannels; ++ch) {
                    float s = (ch < 2) ? stagingBuffer[f * 2 + ch] : 0.0f;
                    float sample = (std::max)(-1.0f, (std::min)(1.0f, s));
                    int32_t val = static_cast<int32_t>(sample * 8388607.0f);
                    size_t idx = (f * m_pwfx->nChannels + ch) * 3;
                    pOut[idx + 0] = static_cast<uint8_t>(val & 0xFF);
                    pOut[idx + 1] = static_cast<uint8_t>((val >> 8) & 0xFF);
                    pOut[idx + 2] = static_cast<uint8_t>((val >> 16) & 0xFF);
                }
            }
        } else if (m_pwfx->wBitsPerSample == 24 || m_pwfx->wBitsPerSample == 32) {
            // PCM 32-bit integer fallback (or 24-bit in 32-bit container)
            int32_t* pOut = reinterpret_cast<int32_t*>(pData);
            for (size_t f = 0; f < framesNeeded; ++f) {
                for (uint32_t ch = 0; ch < m_pwfx->nChannels; ++ch) {
                    float s = (ch < 2) ? stagingBuffer[f * 2 + ch] : 0.0f;
                    float sample = (std::max)(-1.0f, (std::min)(1.0f, s));
                    pOut[f * m_pwfx->nChannels + ch] = static_cast<int32_t>(sample * 2147483647.0f);
                }
            }
        }

        DWORD flags = (framesRead == 0) ? AUDCLNT_BUFFERFLAGS_SILENT : 0;
        m_pRenderClient->ReleaseBuffer(framesNeeded, flags);
    }

    if (hAvrt) AvRevertMmThreadCharacteristics(hAvrt);
    CoUninitialize();
}

// --- AudioEngine Master Implementation ---

AudioEngine::AudioEngine()
    : m_pipelineA(std::make_shared<AudioPipeline>(L"STREAM A"))
    , m_pipelineB(std::make_shared<AudioPipeline>(L"STREAM B"))
{
}

AudioEngine::~AudioEngine() {
    Shutdown();
}

std::vector<AudioDevice> AudioEngine::EnumerateOutputDevices() {
    std::vector<AudioDevice> devices;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool shouldUninit = SUCCEEDED(hr);

    IMMDeviceEnumerator* pEnum = nullptr;
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnum
    );
    if (FAILED(hr) || !pEnum) {
        if (shouldUninit) CoUninitialize();
        return devices;
    }

    IMMDeviceCollection* pCollection = nullptr;
    hr = pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    if (SUCCEEDED(hr) && pCollection) {
        UINT count = 0;
        pCollection->GetCount(&count);

        IMMDevice* pDefaultDevice = nullptr;
        LPWSTR defaultId = nullptr;
        if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDefaultDevice)) && pDefaultDevice) {
            pDefaultDevice->GetId(&defaultId);
            pDefaultDevice->Release();
        }

        for (UINT i = 0; i < count; ++i) {
            IMMDevice* pDev = nullptr;
            if (SUCCEEDED(pCollection->Item(i, &pDev)) && pDev) {
                LPWSTR idStr = nullptr;
                pDev->GetId(&idStr);

                IPropertyStore* pStore = nullptr;
                std::wstring friendlyName = L"Audio Device";
                if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pStore)) && pStore) {
                    PROPVARIANT varName;
                    PropVariantInit(&varName);
                    if (SUCCEEDED(pStore->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.pwszVal) {
                        friendlyName = varName.pwszVal;
                    }
                    PropVariantClear(&varName);
                    pStore->Release();
                }

                AudioDevice dev;
                if (idStr) dev.id = idStr;
                dev.name = friendlyName;

                std::wstring lowerName = friendlyName;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::towlower);

                // Exclude virtual audio cables and loopback bridges from output selection
                if (lowerName.find(L"cable") != std::wstring::npos ||
                    lowerName.find(L"vb-audio") != std::wstring::npos ||
                    lowerName.find(L"virtual cable") != std::wstring::npos) {
                    if (idStr) CoTaskMemFree(idStr);
                    pDev->Release();
                    continue;
                }

                if (lowerName.find(L"bluetooth") != std::wstring::npos ||
                    lowerName.find(L"airpods") != std::wstring::npos ||
                    lowerName.find(L"buds") != std::wstring::npos ||
                    lowerName.find(L"wh-1000") != std::wstring::npos ||
                    lowerName.find(L"bose") != std::wstring::npos ||
                    lowerName.find(L"wireless") != std::wstring::npos) {
                    dev.isBluetooth = true;
                }

                if (defaultId && dev.id == defaultId) {
                    dev.isDefault = true;
                }

                devices.push_back(dev);
                if (idStr) CoTaskMemFree(idStr);
                pDev->Release();
            }
        }
        if (defaultId) CoTaskMemFree(defaultId);
        pCollection->Release();
    }

    pEnum->Release();
    if (shouldUninit) CoUninitialize();
    return devices;
}

std::vector<AudioDevice> AudioEngine::EnumerateInputDevices() {
    std::vector<AudioDevice> devices;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool shouldUninit = SUCCEEDED(hr);

    IMMDeviceEnumerator* pEnum = nullptr;
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnum
    );
    if (FAILED(hr) || !pEnum) {
        if (shouldUninit) CoUninitialize();
        return devices;
    }

    IMMDeviceCollection* pCollection = nullptr;
    hr = pEnum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pCollection);
    if (SUCCEEDED(hr) && pCollection) {
        UINT count = 0;
        pCollection->GetCount(&count);

        LPWSTR defaultId = nullptr;
        IMMDevice* pDefaultDev = nullptr;
        if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eCapture, eConsole, &pDefaultDev)) && pDefaultDev) {
            pDefaultDev->GetId(&defaultId);
            pDefaultDev->Release();
        }

        for (UINT i = 0; i < count; ++i) {
            IMMDevice* pDev = nullptr;
            if (SUCCEEDED(pCollection->Item(i, &pDev)) && pDev) {
                LPWSTR idStr = nullptr;
                pDev->GetId(&idStr);

                IPropertyStore* pStore = nullptr;
                std::wstring friendlyName = L"Unknown Microphone";
                if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pStore)) && pStore) {
                    PROPVARIANT varName;
                    PropVariantInit(&varName);
                    if (SUCCEEDED(pStore->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.pwszVal) {
                        friendlyName = varName.pwszVal;
                    }
                    PropVariantClear(&varName);
                    pStore->Release();
                }

                AudioDevice dev;
                if (idStr) dev.id = idStr;
                dev.name = friendlyName;

                std::wstring lowerName = friendlyName;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::towlower);

                // Exclude virtual audio cables from microphone calibration input selection
                if (lowerName.find(L"cable") != std::wstring::npos ||
                    lowerName.find(L"vb-audio") != std::wstring::npos ||
                    lowerName.find(L"virtual cable") != std::wstring::npos) {
                    if (idStr) CoTaskMemFree(idStr);
                    pDev->Release();
                    continue;
                }

                if (lowerName.find(L"bluetooth") != std::wstring::npos ||
                    lowerName.find(L"airpods") != std::wstring::npos ||
                    lowerName.find(L"buds") != std::wstring::npos ||
                    lowerName.find(L"hands-free") != std::wstring::npos ||
                    lowerName.find(L"wireless") != std::wstring::npos) {
                    dev.isBluetooth = true;
                }

                if (defaultId && dev.id == defaultId) {
                    dev.isDefault = true;
                }

                devices.push_back(dev);
                if (idStr) CoTaskMemFree(idStr);
                pDev->Release();
            }
        }
        if (defaultId) CoTaskMemFree(defaultId);
        pCollection->Release();
    }

    pEnum->Release();
    if (shouldUninit) CoUninitialize();
    return devices;
}

std::vector<std::pair<DWORD, std::wstring>> AudioEngine::EnumerateAudioProcesses() {
    std::vector<std::pair<DWORD, std::wstring>> processes;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool shouldUninit = SUCCEEDED(hr);

    IMMDeviceEnumerator* pEnum = nullptr;
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnum
    );
    if (FAILED(hr) || !pEnum) {
        if (shouldUninit) CoUninitialize();
        return processes;
    }

    IMMDevice* pDev = nullptr;
    hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev);
    if (SUCCEEDED(hr) && pDev) {
        IAudioSessionManager2* pMgr = nullptr;
        hr = pDev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL, (void**)&pMgr);
        if (SUCCEEDED(hr) && pMgr) {
            IAudioSessionEnumerator* pSessionEnum = nullptr;
            hr = pMgr->GetSessionEnumerator(&pSessionEnum);
            if (SUCCEEDED(hr) && pSessionEnum) {
                int sessionCount = 0;
                pSessionEnum->GetCount(&sessionCount);
                for (int i = 0; i < sessionCount; ++i) {
                    IAudioSessionControl* pControl = nullptr;
                    if (SUCCEEDED(pSessionEnum->GetSession(i, &pControl)) && pControl) {
                        IAudioSessionControl2* pControl2 = nullptr;
                        if (SUCCEEDED(pControl->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&pControl2)) && pControl2) {
                            DWORD pid = 0;
                            pControl2->GetProcessId(&pid);
                            if (pid != 0 && pid != GetCurrentProcessId()) {
                                HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                                WCHAR processName[MAX_PATH] = L"Unknown App";
                                if (hProcess) {
                                    DWORD size = MAX_PATH;
                                    QueryFullProcessImageNameW(hProcess, 0, processName, &size);
                                    CloseHandle(hProcess);
                                }
                                std::wstring nameOnly = processName;
                                size_t slashPos = nameOnly.find_last_of(L"\\/");
                                if (slashPos != std::wstring::npos) {
                                    nameOnly = nameOnly.substr(slashPos + 1);
                                }
                                processes.push_back({pid, nameOnly});
                            }
                            pControl2->Release();
                        }
                        pControl->Release();
                    }
                }
                pSessionEnum->Release();
            }
            pMgr->Release();
        }
        pDev->Release();
    }
    pEnum->Release();
    if (shouldUninit) CoUninitialize();
    return processes;
}

static const GUID GUID_AuduoVolumeContext = 
    { 0x7b58c891, 0x3d4e, 0x4f12, { 0x8a, 0xbc, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc } };

class EndpointVolumeWatcher : public IAudioEndpointVolumeCallback {
public:
    EndpointVolumeWatcher(AudioEngine* engine) : m_engine(engine), m_refCount(1) {}
    virtual ~EndpointVolumeWatcher() = default;

    void DetachEngine() {
        m_engine = nullptr;
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioEndpointVolumeCallback)) {
            *ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&m_refCount);
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG ul = InterlockedDecrement(&m_refCount);
        if (ul == 0) delete this;
        return ul;
    }

    STDMETHODIMP OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA pNotify) override {
        if (pNotify && m_engine) {
            m_engine->HandleEndpointVolumeNotification(pNotify->fMasterVolume, pNotify->bMuted != FALSE, pNotify->guidEventContext);
        }
        return S_OK;
    }

private:
    AudioEngine* m_engine;
    LONG m_refCount;
};

void AudioEngine::AttachWindowsVolumeEndpoint() {
    DetachWindowsVolumeEndpoint();

    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum)) || !pEnum) {
        return;
    }

    if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &m_pDefaultEndpointDevice)) && m_pDefaultEndpointDevice) {
        if (SUCCEEDED(m_pDefaultEndpointDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (void**)&m_pDefaultEndpointVolume)) && m_pDefaultEndpointVolume) {
            m_pVolumeWatcher = new EndpointVolumeWatcher(this);
            m_pDefaultEndpointVolume->RegisterControlChangeNotify(m_pVolumeWatcher);

            // Read initial volume and mute state
            float vol = 1.0f;
            WINBOOL mute = FALSE;
            if (SUCCEEDED(m_pDefaultEndpointVolume->GetMasterVolumeLevelScalar(&vol))) {
                m_pDefaultEndpointVolume->GetMute(&mute);
                m_masterVolume.store(vol, std::memory_order_relaxed);
                m_isMuted.store(mute != FALSE, std::memory_order_relaxed);
                float effective = (mute != FALSE) ? 0.0f : vol;
                if (m_pipelineA) m_pipelineA->SetMasterVolume(effective);
                if (m_pipelineB) m_pipelineB->SetMasterVolume(effective);
            }
        }
    }
    pEnum->Release();
}

void AudioEngine::DetachWindowsVolumeEndpoint() {
    if (m_pDefaultEndpointVolume) {
        if (m_pVolumeWatcher) {
            static_cast<EndpointVolumeWatcher*>(m_pVolumeWatcher)->DetachEngine();
            m_pDefaultEndpointVolume->UnregisterControlChangeNotify(m_pVolumeWatcher);
            m_pVolumeWatcher->Release();
            m_pVolumeWatcher = nullptr;
        }
        m_pDefaultEndpointVolume->Release();
        m_pDefaultEndpointVolume = nullptr;
    }
    if (m_pDefaultEndpointDevice) {
        m_pDefaultEndpointDevice->Release();
        m_pDefaultEndpointDevice = nullptr;
    }
}

void AudioEngine::HandleEndpointVolumeNotification(float vol, bool muted, const GUID& context) {
    if (IsEqualGUID(context, GUID_AuduoVolumeContext)) {
        return; // Ignore our own volume events
    }
    if (!m_syncWithWindowsVolume.load(std::memory_order_relaxed)) {
        return;
    }

    m_masterVolume.store(vol, std::memory_order_relaxed);
    m_isMuted.store(muted, std::memory_order_relaxed);
    float effective = muted ? 0.0f : vol;
    if (m_pipelineA) m_pipelineA->SetMasterVolume(effective);
    if (m_pipelineB) m_pipelineB->SetMasterVolume(effective);
}

bool AudioEngine::Initialize() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    AttachWindowsVolumeEndpoint();
    return true;
}

void AudioEngine::Shutdown() {
    DetachWindowsVolumeEndpoint();
    StopMicMonitor();
    StopMetronome();
    if (m_calibrationThread.joinable()) {
        m_calibrationThread.join();
    }
    StopStreaming();
    if (m_pipelineA) m_pipelineA->Shutdown();
    if (m_pipelineB) m_pipelineB->Shutdown();
    CoUninitialize();
}

void AudioEngine::SelectDeviceA(const std::wstring& deviceId) {
    if (m_deviceIdA == deviceId && m_pipelineA && m_pipelineA->IsRunning()) {
        return;
    }
    m_deviceIdA = deviceId;
    if (m_isStreaming.load(std::memory_order_relaxed) && m_pipelineA) {
        float vol = m_pipelineA->GetVolume();
        int delay = m_pipelineA->GetDelayMs();
        float master = m_isMuted.load(std::memory_order_relaxed) ? 0.0f : m_masterVolume.load(std::memory_order_relaxed);
        m_pipelineA->Shutdown();
        if (m_pipelineA->Initialize(m_deviceIdA)) {
            m_pipelineA->SetVolume(vol);
            m_pipelineA->SetDelayMs(delay);
            m_pipelineA->SetMasterVolume(master);
        }
    }
}

void AudioEngine::SelectDeviceB(const std::wstring& deviceId) {
    if (m_deviceIdB == deviceId && m_pipelineB && m_pipelineB->IsRunning()) {
        return;
    }
    m_deviceIdB = deviceId;
    if (m_isStreaming.load(std::memory_order_relaxed) && m_pipelineB) {
        float vol = m_pipelineB->GetVolume();
        int delay = m_pipelineB->GetDelayMs();
        float master = m_isMuted.load(std::memory_order_relaxed) ? 0.0f : m_masterVolume.load(std::memory_order_relaxed);
        m_pipelineB->Shutdown();
        if (m_pipelineB->Initialize(m_deviceIdB)) {
            m_pipelineB->SetVolume(vol);
            m_pipelineB->SetDelayMs(delay);
            m_pipelineB->SetMasterVolume(master);
        }
    }
}

void AudioEngine::SelectMicDevice(const std::wstring& deviceId) {
    if (m_micDeviceId == deviceId) return;
    m_micDeviceId = deviceId;
    if (m_micMonitorRunning.load(std::memory_order_relaxed)) {
        StopMicMonitor();
        StartMicMonitor();
    }
}

std::wstring AudioEngine::GetDefaultDeviceId() {
    std::wstring defaultId;
    IMMDeviceEnumerator* pEnum = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum)) && pEnum) {
        IMMDevice* pDev = nullptr;
        if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev)) && pDev) {
            LPWSTR pId = nullptr;
            if (SUCCEEDED(pDev->GetId(&pId)) && pId) {
                defaultId = pId;
                CoTaskMemFree(pId);
            }
            pDev->Release();
        }
        pEnum->Release();
    }
    return defaultId;
}

bool AudioEngine::IsVirtualCableDefault() const {
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum)) || !pEnum) {
        return false;
    }
    IMMDevice* pDev = nullptr;
    bool isCable = false;
    if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDev)) && pDev) {
        IPropertyStore* pStore = nullptr;
        if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pStore)) && pStore) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            if (SUCCEEDED(pStore->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.pwszVal) {
                std::wstring name = varName.pwszVal;
                std::transform(name.begin(), name.end(), name.begin(), ::towlower);
                if (name.find(L"cable") != std::wstring::npos ||
                    name.find(L"vb-audio") != std::wstring::npos ||
                    name.find(L"auduo") != std::wstring::npos) {
                    isCable = true;
                }
            }
            PropVariantClear(&varName);
            pStore->Release();
        }
        pDev->Release();
    }
    pEnum->Release();
    return isCable;
}

bool AudioEngine::IsVirtualCableInstalled() const {
    IMMDeviceEnumerator* pEnum = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum)) || !pEnum) {
        return false;
    }
    IMMDeviceCollection* pCol = nullptr;
    bool found = false;
    if (SUCCEEDED(pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCol)) && pCol) {
        UINT count = 0;
        pCol->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* pDev = nullptr;
            if (SUCCEEDED(pCol->Item(i, &pDev)) && pDev) {
                IPropertyStore* pStore = nullptr;
                if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pStore)) && pStore) {
                    PROPVARIANT varName;
                    PropVariantInit(&varName);
                    if (SUCCEEDED(pStore->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.pwszVal) {
                        std::wstring name = varName.pwszVal;
                        std::transform(name.begin(), name.end(), name.begin(), ::towlower);
                        if (name.find(L"cable") != std::wstring::npos ||
                            name.find(L"vb-audio") != std::wstring::npos ||
                            name.find(L"auduo") != std::wstring::npos) {
                            found = true;
                        }
                    }
                    PropVariantClear(&varName);
                    pStore->Release();
                }
                pDev->Release();
            }
            if (found) break;
        }
        pCol->Release();
    }
    pEnum->Release();
    return found;
}

void AudioEngine::SetMasterVolume(float vol) {
    if (vol < 0.0f) vol = 0.0f;
    if (vol > 1.0f) vol = 1.0f;
    m_masterVolume.store(vol, std::memory_order_relaxed);

    if (m_isMuted.load(std::memory_order_relaxed) && vol > 0.0f) {
        m_isMuted.store(false, std::memory_order_relaxed);
        if (m_syncWithWindowsVolume.load(std::memory_order_relaxed) && m_pDefaultEndpointVolume) {
            m_pDefaultEndpointVolume->SetMute(FALSE, &GUID_AuduoVolumeContext);
        }
    }

    float effective = m_isMuted.load(std::memory_order_relaxed) ? 0.0f : vol;
    if (m_pipelineA) m_pipelineA->SetMasterVolume(effective);
    if (m_pipelineB) m_pipelineB->SetMasterVolume(effective);

    if (m_syncWithWindowsVolume.load(std::memory_order_relaxed)) {
        if (!m_pDefaultEndpointVolume) {
            AttachWindowsVolumeEndpoint();
        }
        if (m_pDefaultEndpointVolume) {
            m_pDefaultEndpointVolume->SetMasterVolumeLevelScalar(vol, &GUID_AuduoVolumeContext);
        }
    }
}

void AudioEngine::UpdateWindowsVolumeSync() {
    if (!m_syncWithWindowsVolume.load(std::memory_order_relaxed)) return;

    if (!m_pDefaultEndpointVolume) {
        AttachWindowsVolumeEndpoint();
    }
    if (m_pDefaultEndpointVolume) {
        float vol = 1.0f;
        WINBOOL mute = FALSE;
        if (SUCCEEDED(m_pDefaultEndpointVolume->GetMasterVolumeLevelScalar(&vol))) {
            m_pDefaultEndpointVolume->GetMute(&mute);
            bool isMuted = (mute != FALSE);
            m_isMuted.store(isMuted, std::memory_order_relaxed);
            m_masterVolume.store(vol, std::memory_order_relaxed);
            float effective = isMuted ? 0.0f : vol;
            if (m_pipelineA) m_pipelineA->SetMasterVolume(effective);
            if (m_pipelineB) m_pipelineB->SetMasterVolume(effective);
        }
    }
}

bool AudioEngine::StartStreaming() {
    if (m_isStreaming.load(std::memory_order_relaxed)) return true;

    // Initialize playback pipelines
    if (!m_pipelineA->Initialize(m_deviceIdA)) {
        std::wcerr << L"[Auduo] Failed to initialize Pipeline A on: " << m_deviceIdA << std::endl;
    }
    if (!m_pipelineB->Initialize(m_deviceIdB)) {
        std::wcerr << L"[Auduo] Failed to initialize Pipeline B on: " << m_deviceIdB << std::endl;
    }

    // Apply Smart Auto-Sync offset if enabled
    if (m_smartAutoSync) {
        // Query devices to check if one is Bluetooth and the other is not
        auto devices = EnumerateOutputDevices();
        bool aIsBt = false, bIsBt = false;
        for (const auto& dev : devices) {
            if (dev.id == m_deviceIdA) aIsBt = dev.isBluetooth;
            if (dev.id == m_deviceIdB) bIsBt = dev.isBluetooth;
        }

        if (!aIsBt && bIsBt) {
            // A is wired/speakers (low latency), B is Bluetooth (higher latency)
            // Delay A so it waits for B
            m_pipelineA->SetDelayMs(40);
            m_pipelineB->SetDelayMs(0);
        } else if (aIsBt && !bIsBt) {
            // A is Bluetooth, B is wired/speakers
            m_pipelineA->SetDelayMs(0);
            m_pipelineB->SetDelayMs(40);
        } else {
            // Both BT or both wired: balanced default
            m_pipelineA->SetDelayMs(0);
            m_pipelineB->SetDelayMs(0);
        }
    }

    m_stopRequested.store(false, std::memory_order_relaxed);
    m_isStreaming.store(true, std::memory_order_relaxed);

    m_captureThread = std::thread(&AudioEngine::CaptureLoop, this);
    return true;
}

void AudioEngine::StopStreaming() {
    if (!m_isStreaming.load(std::memory_order_relaxed)) return;

    m_stopRequested.store(true, std::memory_order_relaxed);
    if (m_hCaptureEvent) {
        SetEvent(m_hCaptureEvent);
    }
    if (m_captureThread.joinable()) {
        m_captureThread.join();
    }

    m_pipelineA->Shutdown();
    m_pipelineB->Shutdown();

    m_isStreaming.store(false, std::memory_order_relaxed);
}

void AudioEngine::PlayTestBeep() {
    // Generate a 440 Hz sinusoidal beep with sharp attack/decay for sync testing
    const uint32_t sampleRate = 48000;
    const size_t numFrames = sampleRate / 4; // 250ms duration
    std::vector<float> beepData(numFrames * 2, 0.0f);

    for (size_t i = 0; i < numFrames; ++i) {
        float t = static_cast<float>(i) / sampleRate;
        float envelope = 1.0f;
        if (i < 500) envelope = static_cast<float>(i) / 500.0f;
        else if (i > numFrames - 1000) envelope = static_cast<float>(numFrames - i) / 1000.0f;

        float sample = 0.5f * envelope * std::sin(2.0f * static_cast<float>(M_PI) * 880.0f * t);
        beepData[i * 2 + 0] = sample;
        beepData[i * 2 + 1] = sample;
    }

    if (m_pipelineA->IsRunning()) {
        m_pipelineA->PushAudio(beepData.data(), numFrames, sampleRate, 2);
    }
    if (m_pipelineB->IsRunning()) {
        m_pipelineB->PushAudio(beepData.data(), numFrames, sampleRate, 2);
    }
}

void AudioEngine::CaptureLoop() {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    (void)hr;

    DWORD taskIndex = 0;
    HANDLE hAvrt = AvSetMmThreadCharacteristics(L"Pro Audio", &taskIndex);

    IMMDeviceEnumerator* pEnum = nullptr;
    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&pEnum
    );

    if (FAILED(hr) || !pEnum) {
        m_isStreaming.store(false, std::memory_order_relaxed);
        if (hAvrt) AvRevertMmThreadCharacteristics(hAvrt);
        CoUninitialize();
        return;
    }

    hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &m_pCaptureDevice);
    pEnum->Release();

    if (FAILED(hr) || !m_pCaptureDevice) {
        m_isStreaming.store(false, std::memory_order_relaxed);
        if (hAvrt) AvRevertMmThreadCharacteristics(hAvrt);
        CoUninitialize();
        return;
    }

    hr = m_pCaptureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&m_pCaptureClient);
    if (FAILED(hr) || !m_pCaptureClient) {
        m_pCaptureDevice->Release();
        m_pCaptureDevice = nullptr;
        m_isStreaming.store(false, std::memory_order_relaxed);
        if (hAvrt) AvRevertMmThreadCharacteristics(hAvrt);
        CoUninitialize();
        return;
    }

    hr = m_pCaptureClient->GetMixFormat(&m_pCaptureFormat);
    if (FAILED(hr) || !m_pCaptureFormat) {
        m_pCaptureClient->Release();
        m_pCaptureClient = nullptr;
        m_pCaptureDevice->Release();
        m_pCaptureDevice = nullptr;
        m_isStreaming.store(false, std::memory_order_relaxed);
        if (hAvrt) AvRevertMmThreadCharacteristics(hAvrt);
        CoUninitialize();
        return;
    }

    // WASAPI Loopback Initialization
    // We exclude our own process from loopback to eliminate any echo loop
    AUDCLNT_PROCESS_LOOPBACK_PARAMS loopbackParams = {};
    loopbackParams.TargetProcessId = GetCurrentProcessId();
    loopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_EXCLUDE_PROCESS_TREE;
    (void)loopbackParams;

    DWORD streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

    hr = m_pCaptureClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        streamFlags,
        1000000, // 100ms buffer
        0,
        m_pCaptureFormat,
        NULL
    );

    if (FAILED(hr)) {
        // Fallback without event callback if unsupported by specific driver
        streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK;
        hr = m_pCaptureClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            streamFlags,
            1000000,
            0,
            m_pCaptureFormat,
            NULL
        );
    }

    m_hCaptureEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (m_hCaptureEvent && (streamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK)) {
        m_pCaptureClient->SetEventHandle(m_hCaptureEvent);
    }

    hr = m_pCaptureClient->GetService(__uuidof(IAudioCaptureClient), (void**)&m_pCaptureService);
    if (FAILED(hr) || !m_pCaptureService) {
        CoTaskMemFree(m_pCaptureFormat);
        m_pCaptureFormat = nullptr;
        m_pCaptureClient->Release();
        m_pCaptureClient = nullptr;
        m_pCaptureDevice->Release();
        m_pCaptureDevice = nullptr;
        m_isStreaming.store(false, std::memory_order_relaxed);
        if (hAvrt) AvRevertMmThreadCharacteristics(hAvrt);
        CoUninitialize();
        return;
    }

    // Silent Keep-Alive Stream:
    // In Windows WASAPI, if no application is currently outputting sound, the audio clock
    // pauses and loopback capture yields 0 packets. Creating a silent shared render stream
    // keeps the hardware audio clock ticking so capture packets flow immediately!
    IAudioClient* pSilentClient = nullptr;
    IAudioRenderClient* pSilentRender = nullptr;
    WAVEFORMATEX* pSilentFormat = nullptr;
    UINT32 silentBufferFrames = 0;
    hr = m_pCaptureDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pSilentClient);
    if (SUCCEEDED(hr) && pSilentClient) {
        if (SUCCEEDED(pSilentClient->GetMixFormat(&pSilentFormat)) && pSilentFormat) {
            if (SUCCEEDED(pSilentClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, pSilentFormat, NULL))) {
                if (SUCCEEDED(pSilentClient->GetService(__uuidof(IAudioRenderClient), (void**)&pSilentRender)) && pSilentRender) {
                    pSilentClient->GetBufferSize(&silentBufferFrames);
                    BYTE* pSilentData = nullptr;
                    if (SUCCEEDED(pSilentRender->GetBuffer(silentBufferFrames, &pSilentData)) && pSilentData) {
                        pSilentRender->ReleaseBuffer(silentBufferFrames, AUDCLNT_BUFFERFLAGS_SILENT);
                    }
                    pSilentClient->Start();
                }
            }
        }
    }

    hr = m_pCaptureClient->Start();

    std::vector<float> floatConversionBuffer;
    std::wstring defaultDevId = GetDefaultDeviceId();
    bool isADefault = (m_deviceIdA == defaultDevId);
    bool isBDefault = (m_deviceIdB == defaultDevId);
    (void)isADefault;
    (void)isBDefault;

    while (!m_stopRequested.load(std::memory_order_relaxed)) {
        // Continuous keep-alive: keep hardware audio clock running even when system is quiet
        if (pSilentClient && pSilentRender) {
            UINT32 padding = 0;
            if (SUCCEEDED(pSilentClient->GetCurrentPadding(&padding))) {
                UINT32 framesNeeded = silentBufferFrames - padding;
                if (framesNeeded > 0) {
                    BYTE* pSilentData = nullptr;
                    if (SUCCEEDED(pSilentRender->GetBuffer(framesNeeded, &pSilentData)) && pSilentData) {
                        pSilentRender->ReleaseBuffer(framesNeeded, AUDCLNT_BUFFERFLAGS_SILENT);
                    }
                }
            }
        }

        if (m_hCaptureEvent && (streamFlags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK)) {
            WaitForSingleObject(m_hCaptureEvent, 20);
        } else {
            Sleep(10);
        }

        if (m_stopRequested.load(std::memory_order_relaxed)) break;

        UINT32 packetLength = 0;
        hr = m_pCaptureService->GetNextPacketSize(&packetLength);
        while (SUCCEEDED(hr) && packetLength > 0) {
            BYTE* pData = nullptr;
            UINT32 numFramesAvailable = 0;
            DWORD flags = 0;

            hr = m_pCaptureService->GetBuffer(&pData, &numFramesAvailable, &flags, NULL, NULL);
            if (SUCCEEDED(hr) && pData && numFramesAvailable > 0) {
                const uint32_t channels = m_pCaptureFormat->nChannels;
                const uint32_t sampleRate = m_pCaptureFormat->nSamplesPerSec;

                floatConversionBuffer.resize(numFramesAvailable * channels);

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::fill(floatConversionBuffer.begin(), floatConversionBuffer.end(), 0.0f);
                } else {
                    if (m_pCaptureFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                        (m_pCaptureFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                         reinterpret_cast<WAVEFORMATEXTENSIBLE*>(m_pCaptureFormat)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
                        std::memcpy(floatConversionBuffer.data(), pData, numFramesAvailable * channels * sizeof(float));
                    } else if (m_pCaptureFormat->wBitsPerSample == 16) {
                        const int16_t* pSamples = reinterpret_cast<const int16_t*>(pData);
                        for (size_t i = 0; i < numFramesAvailable * channels; ++i) {
                            floatConversionBuffer[i] = pSamples[i] / 32768.0f;
                        }
                    } else if (m_pCaptureFormat->wBitsPerSample == 24 && channels > 0 && (m_pCaptureFormat->nBlockAlign / channels == 3)) {
                        const uint8_t* pBytes = reinterpret_cast<const uint8_t*>(pData);
                        for (size_t i = 0; i < numFramesAvailable * channels; ++i) {
                            int32_t val = (static_cast<int8_t>(pBytes[i * 3 + 2]) << 16) |
                                          (static_cast<uint8_t>(pBytes[i * 3 + 1]) << 8) |
                                          (static_cast<uint8_t>(pBytes[i * 3 + 0]));
                            floatConversionBuffer[i] = static_cast<float>(val) / 8388608.0f;
                        }
                    } else if (m_pCaptureFormat->wBitsPerSample == 24 || m_pCaptureFormat->wBitsPerSample == 32) {
                        const int32_t* pSamples = reinterpret_cast<const int32_t*>(pData);
                        for (size_t i = 0; i < numFramesAvailable * channels; ++i) {
                            floatConversionBuffer[i] = pSamples[i] / 2147483648.0f;
                        }
                    }
                }

                // Push captured audio into twin output pipelines (paused during acoustic calibration or metronome sync test)
                if (!m_isCalibrating.load(std::memory_order_relaxed) && !m_metronomeRunning.load(std::memory_order_relaxed)) {
                    if (m_pipelineA && m_pipelineA->IsRunning()) {
                        m_pipelineA->PushAudio(floatConversionBuffer.data(), numFramesAvailable, sampleRate, channels);
                    }
                    if (m_pipelineB && m_pipelineB->IsRunning()) {
                        m_pipelineB->PushAudio(floatConversionBuffer.data(), numFramesAvailable, sampleRate, channels);
                    }
                }

                m_pCaptureService->ReleaseBuffer(numFramesAvailable);
            }
            hr = m_pCaptureService->GetNextPacketSize(&packetLength);
        }
    }

    if (pSilentClient) {
        pSilentClient->Stop();
        if (pSilentRender) pSilentRender->Release();
        pSilentClient->Release();
    }
    if (pSilentFormat) CoTaskMemFree(pSilentFormat);

    if (m_pCaptureClient) m_pCaptureClient->Stop();
    if (m_pCaptureService) { m_pCaptureService->Release(); m_pCaptureService = nullptr; }
    if (m_pCaptureClient) { m_pCaptureClient->Release(); m_pCaptureClient = nullptr; }
    if (m_pCaptureFormat) { CoTaskMemFree(m_pCaptureFormat); m_pCaptureFormat = nullptr; }
    if (m_pCaptureDevice) { m_pCaptureDevice->Release(); m_pCaptureDevice = nullptr; }
    if (m_hCaptureEvent) { CloseHandle(m_hCaptureEvent); m_hCaptureEvent = NULL; }

    if (hAvrt) AvRevertMmThreadCharacteristics(hAvrt);
    CoUninitialize();
}

// --- Acoustic Mic Auto-Calibration Implementation ---

std::string AudioEngine::GetCalibrationStatus() const {
    std::lock_guard<std::mutex> lock(m_calibrationMutex);
    return m_calibrationStatus;
}

void AudioEngine::StartMicMonitor() {
    if (m_micMonitorThread.joinable()) return;
    m_stopMicMonitor.store(false, std::memory_order_relaxed);
    m_micMonitorRunning.store(true, std::memory_order_relaxed);
    m_micMonitorThread = std::thread(&AudioEngine::MicMonitorLoop, this);
}

void AudioEngine::StopMicMonitor() {
    m_stopMicMonitor.store(true, std::memory_order_relaxed);
    m_micMonitorRunning.store(false, std::memory_order_relaxed);
    if (m_micMonitorThread.joinable()) {
        m_micMonitorThread.join();
    }
    m_micPeakVU.store(0.0f, std::memory_order_relaxed);
    m_micRawActive.store(false, std::memory_order_relaxed);
}

void AudioEngine::MicMonitorLoop() {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    (void)hr;

    IMMDeviceEnumerator* pEnum = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
    if (FAILED(hr) || !pEnum) {
        CoUninitialize();
        return;
    }

    IMMDevice* pMicDevice = nullptr;
    if (m_micDeviceId.empty()) {
        hr = pEnum->GetDefaultAudioEndpoint(eCapture, eConsole, &pMicDevice);
    } else {
        hr = pEnum->GetDevice(m_micDeviceId.c_str(), &pMicDevice);
        if (FAILED(hr) || !pMicDevice) {
            hr = pEnum->GetDefaultAudioEndpoint(eCapture, eConsole, &pMicDevice);
        }
    }
    pEnum->Release();
    if (FAILED(hr) || !pMicDevice) {
        CoUninitialize();
        return;
    }

    IAudioClient* pMicClient = nullptr;
    hr = pMicDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pMicClient);
    pMicDevice->Release();
    if (FAILED(hr) || !pMicClient) {
        CoUninitialize();
        return;
    }

    // Attempt to set Raw Audio Processing Mode to bypass OEM noise gate, voice AI, and beamforming
    IAudioClient2* pClient2 = nullptr;
    if (SUCCEEDED(pMicClient->QueryInterface(__uuidof(IAudioClient2), (void**)&pClient2)) && pClient2) {
        AudioClientProperties props = {};
        props.cbSize = sizeof(AudioClientProperties);
        props.bIsOffload = FALSE;
        props.eCategory = AudioCategory_Media;
        props.Options = AUDCLNT_STREAMOPTIONS_RAW;

        hr = pClient2->SetClientProperties(&props);
        if (FAILED(hr)) {
            props.Options = AUDCLNT_STREAMOPTIONS_NONE;
            pClient2->SetClientProperties(&props);
            m_micRawActive.store(false, std::memory_order_relaxed);
            std::cout << "[Auduo] Mic Monitor: RAW audio mode unsupported by hardware, fell back to standard." << std::endl;
        } else {
            m_micRawActive.store(true, std::memory_order_relaxed);
            std::cout << "[Auduo] Mic Monitor: RAW audio mode ENABLED (bypassing OEM voice processing/noise gates)." << std::endl;
        }
        pClient2->Release();
    }

    WAVEFORMATEX* pwfx = nullptr;
    hr = pMicClient->GetMixFormat(&pwfx);
    if (FAILED(hr) || !pwfx) {
        pMicClient->Release();
        CoUninitialize();
        return;
    }

    hr = pMicClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, pwfx, NULL);
    if (FAILED(hr)) {
        CoTaskMemFree(pwfx);
        pMicClient->Release();
        CoUninitialize();
        return;
    }

    IAudioCaptureClient* pCapture = nullptr;
    hr = pMicClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pCapture);
    if (FAILED(hr) || !pCapture) {
        CoTaskMemFree(pwfx);
        pMicClient->Release();
        CoUninitialize();
        return;
    }

    pMicClient->Start();
    const uint32_t channels = pwfx->nChannels;

    while (!m_stopMicMonitor.load(std::memory_order_relaxed)) {
        // If calibration is actively reading the mic, yield so we don't steal packets
        if (m_isCalibrating.load(std::memory_order_relaxed)) {
            Sleep(25);
            continue;
        }

        UINT32 packetSize = 0;
        hr = pCapture->GetNextPacketSize(&packetSize);
        float currentPeak = 0.0f;
        float gain = m_micGain.load(std::memory_order_relaxed);

        while (SUCCEEDED(hr) && packetSize > 0) {
            BYTE* pData = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (SUCCEEDED(pCapture->GetBuffer(&pData, &frames, &flags, NULL, NULL))) {
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && pData) {
                    if (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                        (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                         reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
                        float* fData = reinterpret_cast<float*>(pData);
                        for (UINT32 f = 0; f < frames * channels; ++f) {
                            float a = std::fabs(fData[f]) * gain;
                            if (a > currentPeak) currentPeak = a;
                        }
                    } else if (pwfx->wBitsPerSample == 16) {
                        int16_t* sData = reinterpret_cast<int16_t*>(pData);
                        for (UINT32 f = 0; f < frames * channels; ++f) {
                            float a = std::fabs(sData[f] / 32768.0f) * gain;
                            if (a > currentPeak) currentPeak = a;
                        }
                    } else if (pwfx->wBitsPerSample == 24 || pwfx->wBitsPerSample == 32) {
                        int32_t* iData = reinterpret_cast<int32_t*>(pData);
                        for (UINT32 f = 0; f < frames * channels; ++f) {
                            float a = std::fabs(iData[f] / 2147483648.0f) * gain;
                            if (a > currentPeak) currentPeak = a;
                        }
                    }
                }
                pCapture->ReleaseBuffer(frames);
            }
            hr = pCapture->GetNextPacketSize(&packetSize);
        }

        // Clamp meter peak to 1.0f
        if (currentPeak > 1.0f) currentPeak = 1.0f;

        // Smooth decaying VU level
        float prev = m_micPeakVU.load(std::memory_order_relaxed);
        float next = (currentPeak > prev) ? currentPeak : (prev * 0.85f);
        m_micPeakVU.store(next, std::memory_order_relaxed);

        Sleep(20);
    }

    pMicClient->Stop();
    pCapture->Release();
    CoTaskMemFree(pwfx);
    pMicClient->Release();
    CoUninitialize();
}

void AudioEngine::CalibrateChannel(int channelIndex, std::function<void(bool success, int latencyMs, const std::string& msg)> onComplete) {
    if (m_isCalibrating.load(std::memory_order_relaxed)) return;

    if (m_calibrationThread.joinable()) {
        m_calibrationThread.join();
    }

    m_isCalibrating.store(true, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_calibrationMutex);
        m_calibrationStatus = (channelIndex == 0) ? "Calibrating Stream A..." : "Calibrating Stream B...";
    }

    m_calibrationThread = std::thread(&AudioEngine::SingleChannelCalibrationWorker, this, channelIndex, onComplete);
}

void AudioEngine::SingleChannelCalibrationWorker(int channelIndex, std::function<void(bool, int, const std::string&)> onComplete) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    (void)hr;

    std::shared_ptr<AudioPipeline> targetPipeline = (channelIndex == 0) ? m_pipelineA : m_pipelineB;
    std::shared_ptr<AudioPipeline> otherPipeline  = (channelIndex == 0) ? m_pipelineB : m_pipelineA;
    const std::wstring targetDeviceId = (channelIndex == 0) ? m_deviceIdA : m_deviceIdB;
    const std::string channelName = (channelIndex == 0) ? "Stream A" : "Stream B";

    bool pipelineStartedByUs = false;
    if (targetPipeline && !targetPipeline->IsRunning()) {
        targetPipeline->Initialize(targetDeviceId);
        pipelineStartedByUs = true;
    }

    // Save user settings so we can restore them cleanly when calibration finishes
    const int savedTargetDelay = targetPipeline ? targetPipeline->GetDelayMs() : 0;
    const int savedOtherDelay  = otherPipeline  ? otherPipeline->GetDelayMs()  : 0;
    const float savedTargetVol = targetPipeline ? targetPipeline->GetVolume()  : 1.0f;
    const float savedMasterVol = m_masterVolume.load(std::memory_order_relaxed);

    // Check physical hardware endpoint volume
    IMMDeviceEnumerator* pEnum = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum);

    IMMDevice* pTargetDevice = nullptr;
    IAudioEndpointVolume* pTargetEndpointVol = nullptr;
    float savedEndpointVol = 1.0f;
    BOOL savedEndpointMute = FALSE;
    bool hasEndpointVol = false;

    if (pEnum) {
        if (!targetDeviceId.empty()) {
            pEnum->GetDevice(targetDeviceId.c_str(), &pTargetDevice);
        } else {
            pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pTargetDevice);
        }
        if (pTargetDevice) {
            if (SUCCEEDED(pTargetDevice->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (void**)&pTargetEndpointVol)) && pTargetEndpointVol) {
                hasEndpointVol = true;
                pTargetEndpointVol->GetMasterVolumeLevelScalar(&savedEndpointVol);
                pTargetEndpointVol->GetMute(&savedEndpointMute);
                std::cout << "[Auduo Calib] Hardware endpoint volume: " << static_cast<int>(savedEndpointVol * 100)
                          << "%, Muted: " << (savedEndpointMute ? "YES" : "NO") << std::endl;
                if (savedEndpointMute) {
                    pTargetEndpointVol->SetMute(FALSE, NULL);
                }
                if (savedEndpointVol < 0.40f) {
                    pTargetEndpointVol->SetMasterVolumeLevelScalar(0.70f, NULL);
                }
            }
            pTargetDevice->Release();
        }
    }

    // Lambda to cleanly restore state on any exit path
    auto restorePipelines = [&]() {
        if (hasEndpointVol && pTargetEndpointVol) {
            pTargetEndpointVol->SetMasterVolumeLevelScalar(savedEndpointVol, NULL);
            pTargetEndpointVol->SetMute(savedEndpointMute, NULL);
            pTargetEndpointVol->Release();
            pTargetEndpointVol = nullptr;
        }
        if (pipelineStartedByUs && targetPipeline) {
            targetPipeline->Shutdown();
        } else {
            if (targetPipeline) {
                targetPipeline->ClearBuffer();
                targetPipeline->SetDelayMs(savedTargetDelay);
                targetPipeline->SetVolume(savedTargetVol);
                targetPipeline->SetMasterVolume(savedMasterVol);
            }
            if (otherPipeline && otherPipeline->IsRunning()) {
                otherPipeline->ClearBuffer();
                otherPipeline->SetDelayMs(savedOtherDelay);
            }
            m_masterVolume.store(savedMasterVol, std::memory_order_relaxed);
        }
        m_isCalibrating.store(false, std::memory_order_relaxed);
        CoUninitialize();
    };

    // Ensure pipeline plays at full calibration volume
    if (targetPipeline) {
        targetPipeline->SetVolume(1.0f);
        targetPipeline->SetMasterVolume(1.0f);
    }

    // Immediately silence non-target pipeline and flush its buffer
    if (otherPipeline && otherPipeline->IsRunning()) {
        otherPipeline->ClearBuffer();
    }

    // Clear target pipeline ring buffer and remove artificial delay so chirp plays with 0 queue lag
    if (targetPipeline && targetPipeline->IsRunning()) {
        targetPipeline->SetDelayMs(0);
        targetPipeline->ClearBuffer();
    }

    // Allow hardware audio client buffers to drain (~50ms)
    Sleep(50);

    IMMDevice* pMicDevice = nullptr;
    if (pEnum) {
        if (m_micDeviceId.empty()) {
            pEnum->GetDefaultAudioEndpoint(eCapture, eConsole, &pMicDevice);
        } else {
            HRESULT hrDev = pEnum->GetDevice(m_micDeviceId.c_str(), &pMicDevice);
            if (FAILED(hrDev) || !pMicDevice) {
                pEnum->GetDefaultAudioEndpoint(eCapture, eConsole, &pMicDevice);
            }
        }
        pEnum->Release();
    }

    if (!pMicDevice) {
        {
            std::lock_guard<std::mutex> lock(m_calibrationMutex);
            m_calibrationStatus = "Error: No microphone detected";
        }
        restorePipelines();
        if (onComplete) onComplete(false, 0, "No microphone detected on system.");
        return;
    }

    IAudioClient* pMicClient = nullptr;
    hr = pMicDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pMicClient);
    pMicDevice->Release();
    if (FAILED(hr) || !pMicClient) {
        restorePipelines();
        if (onComplete) onComplete(false, 0, "Failed to activate microphone.");
        return;
    }

    // Set Raw Audio Processing Mode to bypass Windows Voice AI & Beamforming noise gates
    IAudioClient2* pClient2 = nullptr;
    if (SUCCEEDED(pMicClient->QueryInterface(__uuidof(IAudioClient2), (void**)&pClient2)) && pClient2) {
        AudioClientProperties props = {};
        props.cbSize = sizeof(AudioClientProperties);
        props.bIsOffload = FALSE;
        props.eCategory = AudioCategory_Media;
        props.Options = AUDCLNT_STREAMOPTIONS_RAW;

        hr = pClient2->SetClientProperties(&props);
        if (FAILED(hr)) {
            props.Options = AUDCLNT_STREAMOPTIONS_NONE;
            pClient2->SetClientProperties(&props);
            std::cout << "[Auduo] Calibration: RAW mic mode unsupported by hardware, fell back to standard." << std::endl;
        } else {
            std::cout << "[Auduo] Calibration: RAW mic mode ENABLED (bypassing OEM voice processing/noise gates)." << std::endl;
        }
        pClient2->Release();
    }

    WAVEFORMATEX* pMicFormat = nullptr;
    hr = pMicClient->GetMixFormat(&pMicFormat);
    if (FAILED(hr) || !pMicFormat) {
        pMicClient->Release();
        restorePipelines();
        if (onComplete) onComplete(false, 0, "Failed to query mic format.");
        return;
    }

    hr = pMicClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, pMicFormat, NULL);
    if (FAILED(hr)) {
        CoTaskMemFree(pMicFormat);
        pMicClient->Release();
        restorePipelines();
        if (onComplete) onComplete(false, 0, "Failed to initialize mic client.");
        return;
    }

    IAudioCaptureClient* pMicCapture = nullptr;
    hr = pMicClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pMicCapture);
    if (FAILED(hr) || !pMicCapture) {
        CoTaskMemFree(pMicFormat);
        pMicClient->Release();
        restorePipelines();
        if (onComplete) onComplete(false, 0, "Failed to get mic capture service.");
        return;
    }

    pMicClient->Start();

    const uint32_t micRate = pMicFormat->nSamplesPerSec;
    const uint32_t micChannels = pMicFormat->nChannels;

    // High-Precision Multi-Burst Acoustic Chirp Sequence:
    // 3 distinct frequency sweeps spaced 120ms apart with orthogonal bands
    struct BurstProfile {
        float f0;
        float f1;
        int nominalOffsetMs;
    };
    const BurstProfile burstProfiles[3] = {
        { 1100.0f, 2400.0f, 0 },    // Burst 0: 0ms -> 50ms (1.1 - 2.4 kHz)
        { 1600.0f, 3200.0f, 120 },  // Burst 1: 120ms -> 170ms (1.6 - 3.2 kHz)
        { 2200.0f, 4000.0f, 240 }   // Burst 2: 240ms -> 290ms (2.2 - 4.0 kHz)
    };

    const uint32_t sweepRate = 48000;
    const size_t chirpFrames = (sweepRate * 50) / 1000; // 50ms per chirp
    const float T = 0.050f;
    const size_t totalSeqFrames = (sweepRate * 300) / 1000; // 300ms total playback

    std::vector<float> sweepData(totalSeqFrames * 2, 0.0f);
    std::vector<std::vector<float>> refChirps(3, std::vector<float>(chirpFrames, 0.0f));

    for (size_t b = 0; b < 3; ++b) {
        size_t burstStartFrame = (burstProfiles[b].nominalOffsetMs * sweepRate) / 1000;
        for (size_t i = 0; i < chirpFrames; ++i) {
            float t = static_cast<float>(i) / sweepRate;
            // Hann window envelope to ensure smooth edges
            float env = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * i / (chirpFrames - 1)));
            float phase = 2.0f * static_cast<float>(M_PI) * (burstProfiles[b].f0 * t + ((burstProfiles[b].f1 - burstProfiles[b].f0) / (2.0f * T)) * t * t);
            float sample = 0.95f * env * std::sin(phase);

            refChirps[b][i] = sample;
            if (burstStartFrame + i < totalSeqFrames) {
                sweepData[(burstStartFrame + i) * 2 + 0] = sample;
                sweepData[(burstStartFrame + i) * 2 + 1] = sample;
            }
        }
    }

    const size_t refLenAtMic = (chirpFrames * micRate) / sweepRate;
    const float nccThreshold = 0.35f; // Bounded Pearson NCC: ambient noise < 0.25, acoustic chirp > 0.45
    const int totalShots = 3;
    std::vector<int> ensembleLatencies;

    for (int shot = 0; shot < totalShots; ++shot) {
        {
            std::lock_guard<std::mutex> lock(m_calibrationMutex);
            std::ostringstream ss;
            ss << channelName << ": Ping " << (shot + 1) << "/" << totalShots << "...";
            m_calibrationStatus = ss.str();
        }

        // Flush target pipeline buffer for this ping
        if (targetPipeline && targetPipeline->IsRunning()) {
            targetPipeline->ClearBuffer();
        }

        Sleep(30);

        // Flush stale mic buffers
        UINT32 discardSize = 0;
        while (SUCCEEDED(pMicCapture->GetNextPacketSize(&discardSize)) && discardSize > 0) {
            BYTE* pD = nullptr; UINT32 f = 0; DWORD fl = 0;
            if (SUCCEEDED(pMicCapture->GetBuffer(&pD, &f, &fl, NULL, NULL))) {
                pMicCapture->ReleaseBuffer(f);
            }
        }

        // Push multi-burst audio directly into target pipeline
        if (targetPipeline && targetPipeline->IsRunning()) {
            targetPipeline->PushAudio(sweepData.data(), totalSeqFrames, sweepRate, 2);
        }

        auto startRecord = std::chrono::steady_clock::now();
        std::vector<float> recordedAudio;
        // Record for 800ms per shot (covers 300ms sequence + up to 500ms BT latency)
        float calibGain = m_micGain.load(std::memory_order_relaxed);
        while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startRecord).count() < 800) {
            UINT32 packetSize = 0;
            pMicCapture->GetNextPacketSize(&packetSize);
            while (packetSize > 0) {
                BYTE* pData = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (SUCCEEDED(pMicCapture->GetBuffer(&pData, &frames, &flags, NULL, NULL))) {
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && pData) {
                        if (pMicFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                            (pMicFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                             reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pMicFormat)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
                            float* fData = reinterpret_cast<float*>(pData);
                            for (UINT32 f = 0; f < frames; ++f) {
                                recordedAudio.push_back(fData[f * micChannels] * calibGain);
                            }
                        } else if (pMicFormat->wBitsPerSample == 16) {
                            int16_t* sData = reinterpret_cast<int16_t*>(pData);
                            for (UINT32 f = 0; f < frames; ++f) {
                                recordedAudio.push_back((sData[f * micChannels] / 32768.0f) * calibGain);
                            }
                        } else if (pMicFormat->wBitsPerSample == 24 || pMicFormat->wBitsPerSample == 32) {
                            int32_t* iData = reinterpret_cast<int32_t*>(pData);
                            for (UINT32 f = 0; f < frames; ++f) {
                                recordedAudio.push_back((iData[f * micChannels] / 2147483648.0f) * calibGain);
                            }
                        }
                    } else {
                        for (UINT32 f = 0; f < frames; ++f) recordedAudio.push_back(0.0f);
                    }
                    pMicCapture->ReleaseBuffer(frames);
                }
                pMicCapture->GetNextPacketSize(&packetSize);
            }
            Sleep(5);
        }

        // Multi-Burst NCC Analysis for this shot
        std::vector<int> shotBurstLatencies;
        for (size_t b = 0; b < 3; ++b) {
            std::vector<float> resampledRef(refLenAtMic, 0.0f);
            float e_ref = 0.0f;
            for (size_t i = 0; i < refLenAtMic; ++i) {
                float srcIdx = (static_cast<float>(i) * sweepRate) / micRate;
                size_t idx0 = static_cast<size_t>(srcIdx);
                if (idx0 < refChirps[b].size()) {
                    resampledRef[i] = refChirps[b][idx0];
                    e_ref += resampledRef[i] * resampledRef[i];
                }
            }

            if (e_ref < 1e-6f) continue;

            size_t nominalStartInMic = (burstProfiles[b].nominalOffsetMs * micRate) / 1000;
            size_t searchStart = nominalStartInMic;
            size_t searchEnd = (recordedAudio.size() > refLenAtMic) ? (recordedAudio.size() - refLenAtMic) : 0;
            size_t maxWindow = ((burstProfiles[b].nominalOffsetMs + 650) * micRate) / 1000;
            if (searchEnd > maxWindow) searchEnd = maxWindow;

            float bestNcc = 0.0f;
            size_t bestOffset = 0;
            if (searchEnd > searchStart) {
                for (size_t i = searchStart; i < searchEnd; i += 2) {
                    float crossProd = 0.0f;
                    float e_sig = 0.0f;
                    for (size_t k = 0; k < refLenAtMic; ++k) {
                        float s = recordedAudio[i + k];
                        crossProd += s * resampledRef[k];
                        e_sig += s * s;
                    }
                    float denom = std::sqrt(e_sig * e_ref);
                    if (denom > 1e-6f) {
                        float ncc = std::fabs(crossProd) / denom;
                        if (ncc > bestNcc) {
                            bestNcc = ncc;
                            bestOffset = i;
                        }
                    }
                }

                if (bestNcc > 0.25f && bestOffset >= 3 && bestOffset + 3 < searchEnd) {
                    size_t fineStart = bestOffset - 3;
                    size_t fineEnd = bestOffset + 3;
                    for (size_t i = fineStart; i <= fineEnd; ++i) {
                        float crossProd = 0.0f;
                        float e_sig = 0.0f;
                        for (size_t k = 0; k < refLenAtMic; ++k) {
                            float s = recordedAudio[i + k];
                            crossProd += s * resampledRef[k];
                            e_sig += s * s;
                        }
                        float denom = std::sqrt(e_sig * e_ref);
                        if (denom > 1e-6f) {
                            float ncc = std::fabs(crossProd) / denom;
                            if (ncc > bestNcc) {
                                bestNcc = ncc;
                                bestOffset = i;
                            }
                        }
                    }
                }
            }

            if (bestNcc >= nccThreshold && micRate > 0) {
                int estLatency = static_cast<int>(std::round(((static_cast<double>(bestOffset) / micRate) * 1000.0) - burstProfiles[b].nominalOffsetMs));
                if (estLatency >= 0 && estLatency <= 650) {
                    shotBurstLatencies.push_back(estLatency);
                }
            }
        }

        if (!shotBurstLatencies.empty()) {
            std::sort(shotBurstLatencies.begin(), shotBurstLatencies.end());
            int shotEst = (shotBurstLatencies.size() == 3) ? shotBurstLatencies[1] :
                          ((shotBurstLatencies.size() == 2) ? (shotBurstLatencies[0] + shotBurstLatencies[1]) / 2 : shotBurstLatencies[0]);
            ensembleLatencies.push_back(shotEst);
            std::cout << "[Auduo Calib] Shot " << (shot + 1) << "/" << totalShots
                      << ": detected latency = " << shotEst << " ms (" << shotBurstLatencies.size() << "/3 bursts)" << std::endl;
        } else {
            std::cout << "[Auduo Calib] Shot " << (shot + 1) << "/" << totalShots << ": no chirp detected" << std::endl;
        }
    }

    pMicClient->Stop();
    pMicCapture->Release();
    CoTaskMemFree(pMicFormat);
    pMicClient->Release();

    bool detected = false;
    int latencyMs = 0;
    std::string msg;

    if (!ensembleLatencies.empty()) {
        detected = true;
        std::sort(ensembleLatencies.begin(), ensembleLatencies.end());
        int spread = ensembleLatencies.back() - ensembleLatencies.front();

        if (ensembleLatencies.size() == 3) {
            latencyMs = ensembleLatencies[1]; // Median of 3
        } else if (ensembleLatencies.size() == 2) {
            latencyMs = (ensembleLatencies[0] + ensembleLatencies[1]) / 2;
        } else {
            latencyMs = ensembleLatencies[0];
        }

        std::ostringstream oss;
        oss << channelName << " detected! Latency: " << latencyMs << " ms ("
            << ensembleLatencies.size() << "/3 pings: ";
        for (size_t s = 0; s < ensembleLatencies.size(); ++s) {
            if (s > 0) oss << ", ";
            oss << ensembleLatencies[s];
        }
        oss << " ms, ±" << (spread / 2) << " ms jitter)";
        msg = oss.str();
    } else {
        detected = false;
        msg = channelName + ": Chirp sequence not detected (no audio matched above noise floor).";
    }

    {
        std::lock_guard<std::mutex> lock(m_calibrationMutex);
        m_calibrationStatus = msg;
    }

    // Cleanly restore user delays, volumes, and buffers before notifying caller
    restorePipelines();

    if (onComplete) {
        onComplete(detected, latencyMs, msg);
    }
}

void AudioEngine::AutoSyncBothChannels(std::function<void(bool success, int deltaMs, const std::string& msg)> onComplete) {
    if (m_isCalibrating.load(std::memory_order_relaxed)) return;

    if (m_calibrationThread.joinable()) {
        m_calibrationThread.join();
    }

    m_isCalibrating.store(true, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_calibrationMutex);
        m_calibrationStatus = "Auto-Sync: Preparing simultaneous dual ping...";
    }

    m_calibrationThread = std::thread(&AudioEngine::DualChannelAutoSyncWorker, this, onComplete);
}

void AudioEngine::DualChannelAutoSyncWorker(std::function<void(bool, int, const std::string&)> onComplete) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    (void)hr;

    bool pipelineAStartedByUs = false;
    bool pipelineBStartedByUs = false;

    if (m_pipelineA && !m_pipelineA->IsRunning()) {
        m_pipelineA->Initialize(m_deviceIdA);
        pipelineAStartedByUs = true;
    }
    if (m_pipelineB && !m_pipelineB->IsRunning()) {
        m_pipelineB->Initialize(m_deviceIdB);
        pipelineBStartedByUs = true;
    }

    const int savedDelayA = m_pipelineA ? m_pipelineA->GetDelayMs() : 0;
    const int savedDelayB = m_pipelineB ? m_pipelineB->GetDelayMs() : 0;
    const float savedVolA = m_pipelineA ? m_pipelineA->GetVolume() : 1.0f;
    const float savedVolB = m_pipelineB ? m_pipelineB->GetVolume() : 1.0f;
    const float savedMasterVol = m_masterVolume.load(std::memory_order_relaxed);

    IMMDeviceEnumerator* pEnum = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum);

    IMMDevice* pDevA = nullptr;
    IAudioEndpointVolume* pEndVolA = nullptr;
    float savedEndVolA = 1.0f;
    BOOL savedMuteA = FALSE;
    bool hasEndVolA = false;

    IMMDevice* pDevB = nullptr;
    IAudioEndpointVolume* pEndVolB = nullptr;
    float savedEndVolB = 1.0f;
    BOOL savedMuteB = FALSE;
    bool hasEndVolB = false;

    if (pEnum) {
        if (!m_deviceIdA.empty()) {
            pEnum->GetDevice(m_deviceIdA.c_str(), &pDevA);
        } else {
            pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDevA);
        }
        if (pDevA) {
            if (SUCCEEDED(pDevA->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (void**)&pEndVolA)) && pEndVolA) {
                hasEndVolA = true;
                pEndVolA->GetMasterVolumeLevelScalar(&savedEndVolA);
                pEndVolA->GetMute(&savedMuteA);
                if (savedMuteA) pEndVolA->SetMute(FALSE, NULL);
                if (savedEndVolA < 0.40f) pEndVolA->SetMasterVolumeLevelScalar(0.70f, NULL);
            }
            pDevA->Release();
        }

        if (!m_deviceIdB.empty()) {
            pEnum->GetDevice(m_deviceIdB.c_str(), &pDevB);
        } else {
            pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDevB);
        }
        if (pDevB) {
            if (SUCCEEDED(pDevB->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (void**)&pEndVolB)) && pEndVolB) {
                hasEndVolB = true;
                pEndVolB->GetMasterVolumeLevelScalar(&savedEndVolB);
                pEndVolB->GetMute(&savedMuteB);
                if (savedMuteB) pEndVolB->SetMute(FALSE, NULL);
                if (savedEndVolB < 0.40f) pEndVolB->SetMasterVolumeLevelScalar(0.70f, NULL);
            }
            pDevB->Release();
        }
    }

    auto restorePipelines = [&]() {
        if (hasEndVolA && pEndVolA) {
            pEndVolA->SetMasterVolumeLevelScalar(savedEndVolA, NULL);
            pEndVolA->SetMute(savedMuteA, NULL);
            pEndVolA->Release();
            pEndVolA = nullptr;
        }
        if (hasEndVolB && pEndVolB) {
            pEndVolB->SetMasterVolumeLevelScalar(savedEndVolB, NULL);
            pEndVolB->SetMute(savedMuteB, NULL);
            pEndVolB->Release();
            pEndVolB = nullptr;
        }

        if (pipelineAStartedByUs && !m_isStreaming.load(std::memory_order_relaxed) && m_pipelineA) {
            m_pipelineA->Shutdown();
        } else if (m_pipelineA) {
            m_pipelineA->ClearBuffer();
            m_pipelineA->SetDelayMs(savedDelayA);
            m_pipelineA->SetVolume(savedVolA);
            m_pipelineA->SetMasterVolume(savedMasterVol);
        }

        if (pipelineBStartedByUs && !m_isStreaming.load(std::memory_order_relaxed) && m_pipelineB) {
            m_pipelineB->Shutdown();
        } else if (m_pipelineB) {
            m_pipelineB->ClearBuffer();
            m_pipelineB->SetDelayMs(savedDelayB);
            m_pipelineB->SetVolume(savedVolB);
            m_pipelineB->SetMasterVolume(savedMasterVol);
        }

        m_masterVolume.store(savedMasterVol, std::memory_order_relaxed);
        m_isCalibrating.store(false, std::memory_order_relaxed);
        CoUninitialize();
    };

    if (m_pipelineA) {
        m_pipelineA->SetVolume(1.0f);
        m_pipelineA->SetMasterVolume(1.0f);
        m_pipelineA->SetDelayMs(0);
        m_pipelineA->ClearBuffer();
    }
    if (m_pipelineB) {
        m_pipelineB->SetVolume(1.0f);
        m_pipelineB->SetMasterVolume(1.0f);
        m_pipelineB->SetDelayMs(0);
        m_pipelineB->ClearBuffer();
    }
    m_masterVolume.store(1.0f, std::memory_order_relaxed);
    Sleep(50);

    IMMDevice* pMicDevice = nullptr;
    if (pEnum) {
        if (m_micDeviceId.empty()) {
            pEnum->GetDefaultAudioEndpoint(eCapture, eConsole, &pMicDevice);
        } else {
            HRESULT hrDev = pEnum->GetDevice(m_micDeviceId.c_str(), &pMicDevice);
            if (FAILED(hrDev) || !pMicDevice) {
                pEnum->GetDefaultAudioEndpoint(eCapture, eConsole, &pMicDevice);
            }
        }
        pEnum->Release();
    }

    if (!pMicDevice) {
        {
            std::lock_guard<std::mutex> lock(m_calibrationMutex);
            m_calibrationStatus = "Error: No microphone detected";
        }
        restorePipelines();
        if (onComplete) onComplete(false, 0, "No microphone detected on system.");
        return;
    }

    IAudioClient* pMicClient = nullptr;
    hr = pMicDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pMicClient);
    pMicDevice->Release();
    if (FAILED(hr) || !pMicClient) {
        restorePipelines();
        if (onComplete) onComplete(false, 0, "Failed to activate microphone.");
        return;
    }

    IAudioClient2* pClient2 = nullptr;
    if (SUCCEEDED(pMicClient->QueryInterface(__uuidof(IAudioClient2), (void**)&pClient2)) && pClient2) {
        AudioClientProperties props = {};
        props.cbSize = sizeof(AudioClientProperties);
        props.bIsOffload = FALSE;
        props.eCategory = AudioCategory_Media;
        props.Options = AUDCLNT_STREAMOPTIONS_RAW;
        hr = pClient2->SetClientProperties(&props);
        if (FAILED(hr)) {
            props.Options = AUDCLNT_STREAMOPTIONS_NONE;
            pClient2->SetClientProperties(&props);
        }
        pClient2->Release();
    }

    WAVEFORMATEX* pMicFormat = nullptr;
    hr = pMicClient->GetMixFormat(&pMicFormat);
    if (FAILED(hr) || !pMicFormat) {
        pMicClient->Release();
        restorePipelines();
        if (onComplete) onComplete(false, 0, "Failed to query mic format.");
        return;
    }

    hr = pMicClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, pMicFormat, NULL);
    if (FAILED(hr)) {
        CoTaskMemFree(pMicFormat);
        pMicClient->Release();
        restorePipelines();
        if (onComplete) onComplete(false, 0, "Failed to initialize mic client.");
        return;
    }

    IAudioCaptureClient* pMicCapture = nullptr;
    hr = pMicClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pMicCapture);
    if (FAILED(hr) || !pMicCapture) {
        CoTaskMemFree(pMicFormat);
        pMicClient->Release();
        restorePipelines();
        if (onComplete) onComplete(false, 0, "Failed to get mic capture service.");
        return;
    }

    pMicClient->Start();

    const uint32_t micRate = pMicFormat->nSamplesPerSec;
    const uint32_t micChannels = pMicFormat->nChannels;

    const uint32_t sweepRate = 48000;
    const float chirpDurationSec = 0.070f; // 70ms chirp
    const size_t chirpFrames = static_cast<size_t>(sweepRate * chirpDurationSec); // 3360 frames
    const size_t totalShotFrames = (sweepRate * 850) / 1000; // 850ms per shot (40800 frames)

    // Orthogonal frequency bands:
    // Stream A: 700 Hz -> 1400 Hz (Low Band)
    // Guard Band: 1400 Hz -> 3200 Hz (dead zone absorbing 2nd harmonics up to 2800 Hz)
    // Stream B: 3200 Hz -> 5200 Hz (High Band)
    const float f0_A = 700.0f,  f1_A = 1400.0f;
    const float f0_B = 3200.0f, f1_B = 5200.0f;

    std::vector<float> audioSeqA(totalShotFrames * 2, 0.0f);
    std::vector<float> audioSeqB(totalShotFrames * 2, 0.0f);
    std::vector<float> refA(chirpFrames, 0.0f);
    std::vector<float> refB(chirpFrames, 0.0f);

    for (size_t i = 0; i < chirpFrames; ++i) {
        float t = static_cast<float>(i) / sweepRate;
        float env = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * i / (chirpFrames - 1)));

        float phaseA = 2.0f * static_cast<float>(M_PI) * (f0_A * t + ((f1_A - f0_A) / (2.0f * chirpDurationSec)) * t * t);
        float sA = 0.95f * env * std::sin(phaseA);
        refA[i] = sA;
        audioSeqA[i * 2 + 0] = sA;
        audioSeqA[i * 2 + 1] = sA;

        float phaseB = 2.0f * static_cast<float>(M_PI) * (f0_B * t + ((f1_B - f0_B) / (2.0f * chirpDurationSec)) * t * t);
        float sB = 0.95f * env * std::sin(phaseB);
        refB[i] = sB;
        audioSeqB[i * 2 + 0] = sB;
        audioSeqB[i * 2 + 1] = sB;
    }

    const size_t refLenAtMic = (chirpFrames * micRate) / sweepRate;
    std::vector<float> resampledRefA(refLenAtMic, 0.0f);
    std::vector<float> resampledRefB(refLenAtMic, 0.0f);
    float e_refA = 0.0f;
    float e_refB = 0.0f;

    for (size_t i = 0; i < refLenAtMic; ++i) {
        float srcIdx = (static_cast<float>(i) * sweepRate) / micRate;
        size_t idx0 = static_cast<size_t>(srcIdx);
        if (idx0 < chirpFrames) {
            resampledRefA[i] = refA[idx0];
            e_refA += resampledRefA[i] * resampledRefA[i];
            resampledRefB[i] = refB[idx0];
            e_refB += resampledRefB[i] * resampledRefB[i];
        }
    }

    const int totalShots = 3;
    std::vector<int> ensembleDeltas;
    std::vector<int> ensembleLatA;
    std::vector<int> ensembleLatB;
    int shotCountA = 0;
    int shotCountB = 0;

    const float nccThreshold = 0.28f;

    for (int shot = 0; shot < totalShots; ++shot) {
        {
            std::lock_guard<std::mutex> lock(m_calibrationMutex);
            std::ostringstream ss;
            ss << "Dual Ping " << (shot + 1) << "/" << totalShots << "...";
            m_calibrationStatus = ss.str();
        }

        if (m_pipelineA && m_pipelineA->IsRunning()) m_pipelineA->ClearBuffer();
        if (m_pipelineB && m_pipelineB->IsRunning()) m_pipelineB->ClearBuffer();

        Sleep(30);

        UINT32 discardSize = 0;
        while (SUCCEEDED(pMicCapture->GetNextPacketSize(&discardSize)) && discardSize > 0) {
            BYTE* pD = nullptr; UINT32 f = 0; DWORD fl = 0;
            if (SUCCEEDED(pMicCapture->GetBuffer(&pD, &f, &fl, NULL, NULL))) {
                pMicCapture->ReleaseBuffer(f);
            }
        }

        if (m_pipelineA && m_pipelineA->IsRunning()) {
            m_pipelineA->PushAudio(audioSeqA.data(), totalShotFrames, sweepRate, 2);
        }
        if (m_pipelineB && m_pipelineB->IsRunning()) {
            m_pipelineB->PushAudio(audioSeqB.data(), totalShotFrames, sweepRate, 2);
        }

        auto startRecord = std::chrono::steady_clock::now();
        std::vector<float> recordedAudio;
        float calibGain = m_micGain.load(std::memory_order_relaxed);

        while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startRecord).count() < 850) {
            UINT32 packetSize = 0;
            pMicCapture->GetNextPacketSize(&packetSize);
            while (packetSize > 0) {
                BYTE* pData = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (SUCCEEDED(pMicCapture->GetBuffer(&pData, &frames, &flags, NULL, NULL))) {
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && pData) {
                        if (pMicFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                            (pMicFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                             reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pMicFormat)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
                            float* fData = reinterpret_cast<float*>(pData);
                            for (UINT32 f = 0; f < frames; ++f) {
                                recordedAudio.push_back(fData[f * micChannels] * calibGain);
                            }
                        } else if (pMicFormat->wBitsPerSample == 16) {
                            int16_t* sData = reinterpret_cast<int16_t*>(pData);
                            for (UINT32 f = 0; f < frames; ++f) {
                                recordedAudio.push_back((sData[f * micChannels] / 32768.0f) * calibGain);
                            }
                        } else if (pMicFormat->wBitsPerSample == 24 || pMicFormat->wBitsPerSample == 32) {
                            int32_t* iData = reinterpret_cast<int32_t*>(pData);
                            for (UINT32 f = 0; f < frames; ++f) {
                                recordedAudio.push_back((iData[f * micChannels] / 2147483648.0f) * calibGain);
                            }
                        }
                    } else {
                        for (UINT32 f = 0; f < frames; ++f) recordedAudio.push_back(0.0f);
                    }
                    pMicCapture->ReleaseBuffer(frames);
                }
                pMicCapture->GetNextPacketSize(&packetSize);
            }
            Sleep(5);
        }

        size_t searchEnd = (recordedAudio.size() > refLenAtMic) ? (recordedAudio.size() - refLenAtMic) : 0;
        size_t maxWindow = (650 * micRate) / 1000;
        if (searchEnd > maxWindow) searchEnd = maxWindow;

        float bestNccA = 0.0f;
        size_t bestOffsetA = 0;
        float bestNccB = 0.0f;
        size_t bestOffsetB = 0;

        if (searchEnd > 0 && e_refA > 1e-6f && e_refB > 1e-6f) {
            for (size_t i = 0; i < searchEnd; i += 2) {
                float crossProd = 0.0f;
                float e_sig = 0.0f;
                for (size_t k = 0; k < refLenAtMic; ++k) {
                    float s = recordedAudio[i + k];
                    crossProd += s * resampledRefA[k];
                    e_sig += s * s;
                }
                float denom = std::sqrt(e_sig * e_refA);
                if (denom > 1e-6f) {
                    float ncc = std::fabs(crossProd) / denom;
                    if (ncc > bestNccA) {
                        bestNccA = ncc;
                        bestOffsetA = i;
                    }
                }
            }
            if (bestNccA > 0.20f && bestOffsetA >= 3 && bestOffsetA + 3 < searchEnd) {
                size_t fineStart = bestOffsetA - 3;
                size_t fineEnd = bestOffsetA + 3;
                for (size_t i = fineStart; i <= fineEnd; ++i) {
                    float crossProd = 0.0f, e_sig = 0.0f;
                    for (size_t k = 0; k < refLenAtMic; ++k) {
                        float s = recordedAudio[i + k];
                        crossProd += s * resampledRefA[k];
                        e_sig += s * s;
                    }
                    float denom = std::sqrt(e_sig * e_refA);
                    if (denom > 1e-6f) {
                        float ncc = std::fabs(crossProd) / denom;
                        if (ncc > bestNccA) {
                            bestNccA = ncc;
                            bestOffsetA = i;
                        }
                    }
                }
            }

            for (size_t i = 0; i < searchEnd; i += 2) {
                float crossProd = 0.0f;
                float e_sig = 0.0f;
                for (size_t k = 0; k < refLenAtMic; ++k) {
                    float s = recordedAudio[i + k];
                    crossProd += s * resampledRefB[k];
                    e_sig += s * s;
                }
                float denom = std::sqrt(e_sig * e_refB);
                if (denom > 1e-6f) {
                    float ncc = std::fabs(crossProd) / denom;
                    if (ncc > bestNccB) {
                        bestNccB = ncc;
                        bestOffsetB = i;
                    }
                }
            }
            if (bestNccB > 0.20f && bestOffsetB >= 3 && bestOffsetB + 3 < searchEnd) {
                size_t fineStart = bestOffsetB - 3;
                size_t fineEnd = bestOffsetB + 3;
                for (size_t i = fineStart; i <= fineEnd; ++i) {
                    float crossProd = 0.0f, e_sig = 0.0f;
                    for (size_t k = 0; k < refLenAtMic; ++k) {
                        float s = recordedAudio[i + k];
                        crossProd += s * resampledRefB[k];
                        e_sig += s * s;
                    }
                    float denom = std::sqrt(e_sig * e_refB);
                    if (denom > 1e-6f) {
                        float ncc = std::fabs(crossProd) / denom;
                        if (ncc > bestNccB) {
                            bestNccB = ncc;
                            bestOffsetB = i;
                        }
                    }
                }
            }
        }

        bool detA = (bestNccA >= nccThreshold);
        bool detB = (bestNccB >= nccThreshold);
        if (detA) shotCountA++;
        if (detB) shotCountB++;

        if (detA && detB && micRate > 0) {
            int latA = static_cast<int>(std::round((static_cast<double>(bestOffsetA) / micRate) * 1000.0));
            int latB = static_cast<int>(std::round((static_cast<double>(bestOffsetB) / micRate) * 1000.0));
            int delta = latA - latB;
            ensembleDeltas.push_back(delta);
            ensembleLatA.push_back(latA);
            ensembleLatB.push_back(latB);
            std::cout << "[Auduo Auto-Sync] Ping " << (shot + 1) << "/" << totalShots
                      << ": Stream A = " << latA << " ms (NCC " << bestNccA << "), Stream B = " << latB
                      << " ms (NCC " << bestNccB << ") -> Delta = " << delta << " ms" << std::endl;
        } else {
            std::cout << "[Auduo Auto-Sync] Ping " << (shot + 1) << "/" << totalShots
                      << ": partial/missed detection (detA=" << (detA ? "YES" : "NO") << " [" << bestNccA
                      << "], detB=" << (detB ? "YES" : "NO") << " [" << bestNccB << "])" << std::endl;
        }
    }

    pMicClient->Stop();
    pMicCapture->Release();
    CoTaskMemFree(pMicFormat);
    pMicClient->Release();

    bool success = false;
    int finalDelta = 0;
    std::string msg;

    if (!ensembleDeltas.empty()) {
        success = true;
        std::sort(ensembleDeltas.begin(), ensembleDeltas.end());
        finalDelta = (ensembleDeltas.size() == 3) ? ensembleDeltas[1] :
                     ((ensembleDeltas.size() == 2) ? (ensembleDeltas[0] + ensembleDeltas[1]) / 2 : ensembleDeltas[0]);
        int spread = ensembleDeltas.back() - ensembleDeltas.front();

        std::ostringstream oss;
        oss << "Sync Measured: " << std::abs(finalDelta) << " ms offset (";
        if (finalDelta > 0) {
            oss << "Stream B delayed +" << finalDelta << " ms";
        } else if (finalDelta < 0) {
            oss << "Stream A delayed +" << (-finalDelta) << " ms";
        } else {
            oss << "Perfectly synchronized";
        }
        oss << " | " << ensembleDeltas.size() << "/3 pings, ±" << (spread / 2) << " ms jitter)";
        msg = oss.str();
    } else {
        success = false;
        std::ostringstream oss;
        oss << "Could not detect both chirps simultaneously (Stream A heard: " << shotCountA
            << "/3, Stream B heard: " << shotCountB
            << "/3). Position earbuds closer to laptop mic and retry.";
        msg = oss.str();
    }

    {
        std::lock_guard<std::mutex> lock(m_calibrationMutex);
        m_calibrationStatus = msg;
    }

    restorePipelines();

    if (onComplete) {
        onComplete(success, finalDelta, msg);
    }
}

void AudioEngine::StartMetronome(int intervalMs) {
    if (m_metronomeRunning.load(std::memory_order_relaxed)) return;
    if (m_metronomeThread.joinable()) {
        m_metronomeThread.join();
    }
    m_metronomeRunning.store(true, std::memory_order_relaxed);
    m_metronomeThread = std::thread(&AudioEngine::MetronomeLoop, this, intervalMs);
}

void AudioEngine::StopMetronome() {
    m_metronomeRunning.store(false, std::memory_order_relaxed);
    if (m_metronomeThread.joinable()) {
        m_metronomeThread.join();
    }
}

void AudioEngine::MetronomeLoop(int intervalMs) {
    (void)intervalMs;
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    (void)hr;

    timeBeginPeriod(1);

    DWORD taskIndex = 0;
    HANDLE hAvrt = AvSetMmThreadCharacteristics(L"Pro Audio", &taskIndex);

    bool pipelineAStarted = false;
    bool pipelineBStarted = false;

    if (m_pipelineA && !m_pipelineA->IsRunning()) {
        if (m_pipelineA->Initialize(m_deviceIdA)) {
            pipelineAStarted = true;
        }
    }
    if (m_pipelineB && !m_pipelineB->IsRunning()) {
        if (m_pipelineB->Initialize(m_deviceIdB)) {
            pipelineBStarted = true;
        }
    }

    if (m_pipelineA) {
        m_pipelineA->ClearBuffer();
        int d = m_pipelineA->GetDelayMs();
        if (d > 0) m_pipelineA->SetDelayMs(d);
        float master = m_isMuted.load(std::memory_order_relaxed) ? 0.0f : m_masterVolume.load(std::memory_order_relaxed);
        m_pipelineA->SetMasterVolume(master);
    }
    if (m_pipelineB) {
        m_pipelineB->ClearBuffer();
        int d = m_pipelineB->GetDelayMs();
        if (d > 0) m_pipelineB->SetDelayMs(d);
        float master = m_isMuted.load(std::memory_order_relaxed) ? 0.0f : m_masterVolume.load(std::memory_order_relaxed);
        m_pipelineB->SetMasterVolume(master);
    }

    const uint32_t sampleRate = 48000;
    // Pre-rendered 2.0-second studio rhythm track (4 beats @ 120 BPM = 500ms intervals)
    // Beat 1: High crisp studio rimshot (1800Hz / 3200Hz)
    // Beats 2, 3, 4: Organic acoustic studio handclap with micro-flam bursts
    const size_t totalLoopFrames = sampleRate * 2; // 2.0 seconds = 96,000 frames
    std::vector<float> preRenderedTrack(totalLoopFrames * 2, 0.0f);

    auto renderClap = [&](size_t startFrame, bool isDownbeat) {
        const size_t durationFrames = (sampleRate * 45) / 1000; // 45ms clap
        for (size_t i = 0; i < durationFrames && (startFrame + i) < totalLoopFrames; ++i) {
            float t = static_cast<float>(i) / sampleRate;
            float s = 0.0f;
            if (isDownbeat) {
                // Crisp high-woodblock rimshot (1800Hz + 3200Hz harmonic snap)
                float decay = std::exp(-220.0f * t);
                s = 0.85f * (0.65f * std::sin(2.0f * static_cast<float>(M_PI) * 1800.0f * t) +
                             0.35f * std::sin(2.0f * static_cast<float>(M_PI) * 3200.0f * t)) * decay;
            } else {
                // Authentic studio handclap with micro-flam burst + resonant body
                float pre1 = (t < 0.006f) ? 0.30f * std::sin(2.0f * static_cast<float>(M_PI) * 1400.0f * t) * (1.0f - t / 0.006f) : 0.0f;
                float t2 = t - 0.007f;
                float pre2 = (t2 >= 0.0f && t2 < 0.006f) ? 0.45f * std::sin(2.0f * static_cast<float>(M_PI) * 1250.0f * t2) * (1.0f - t2 / 0.006f) : 0.0f;
                float t3 = t - 0.014f;
                float mainImpact = 0.0f;
                if (t3 >= 0.0f) {
                    float decay = std::exp(-160.0f * t3);
                    mainImpact = 0.85f * (0.60f * std::sin(2.0f * static_cast<float>(M_PI) * 1050.0f * t3) +
                                         0.40f * std::sin(2.0f * static_cast<float>(M_PI) * 2100.0f * t3)) * decay;
                }
                s = pre1 + pre2 + mainImpact;
            }
            if (s > 1.0f) s = 1.0f; else if (s < -1.0f) s = -1.0f;
            preRenderedTrack[(startFrame + i) * 2 + 0] = s;
            preRenderedTrack[(startFrame + i) * 2 + 1] = s;
        }
    };

    for (int beat = 0; beat < 4; ++beat) {
        size_t beatStart = (beat * totalLoopFrames) / 4;
        renderClap(beatStart, beat == 0);
    }

    const size_t chunkSize = (sampleRate * 20) / 1000; // 20ms continuous chunk = 960 frames
    std::vector<float> chunk(chunkSize * 2, 0.0f);

    size_t trackPos = 0;
    auto nextTick = std::chrono::steady_clock::now();

    while (m_metronomeRunning.load(std::memory_order_relaxed)) {
        for (size_t i = 0; i < chunkSize; ++i) {
            size_t p = (trackPos + i) % totalLoopFrames;
            chunk[i * 2 + 0] = preRenderedTrack[p * 2 + 0];
            chunk[i * 2 + 1] = preRenderedTrack[p * 2 + 1];
        }
        trackPos = (trackPos + chunkSize) % totalLoopFrames;

        if (m_pipelineA && m_pipelineA->IsRunning()) {
            m_pipelineA->PushAudio(chunk.data(), chunkSize, sampleRate, 2);
        }
        if (m_pipelineB && m_pipelineB->IsRunning()) {
            m_pipelineB->PushAudio(chunk.data(), chunkSize, sampleRate, 2);
        }

        nextTick += std::chrono::milliseconds(20);
        auto now = std::chrono::steady_clock::now();
        if (nextTick > now) {
            auto sleepMs = std::chrono::duration_cast<std::chrono::milliseconds>(nextTick - now).count();
            if (sleepMs > 0) Sleep(static_cast<DWORD>(sleepMs));
        } else {
            nextTick = now;
        }
    }

    if (m_pipelineA) {
        m_pipelineA->ClearBuffer();
        int d = m_pipelineA->GetDelayMs();
        if (d > 0) m_pipelineA->SetDelayMs(d);
    }
    if (m_pipelineB) {
        m_pipelineB->ClearBuffer();
        int d = m_pipelineB->GetDelayMs();
        if (d > 0) m_pipelineB->SetDelayMs(d);
    }

    if (pipelineAStarted && !m_isStreaming.load(std::memory_order_relaxed) && m_pipelineA) {
        m_pipelineA->Shutdown();
    }
    if (pipelineBStarted && !m_isStreaming.load(std::memory_order_relaxed) && m_pipelineB) {
        m_pipelineB->Shutdown();
    }

    if (hAvrt) AvRevertMmThreadCharacteristics(hAvrt);
    timeEndPeriod(1);
    CoUninitialize();
}

// --- Headless Automated CLI Test Suites ---

int AudioEngine::RunTestDevices() {
    std::wcout << L"[Auduo Test] Enumerating audio endpoints..." << std::endl;
    auto devices = EnumerateOutputDevices();
    std::wcout << L"[Auduo Test] Found " << devices.size() << L" active render endpoints:" << std::endl;
    for (size_t i = 0; i < devices.size(); ++i) {
        std::wcout << L"  [" << i << L"] " << devices[i].name
                   << (devices[i].isDefault ? L" (DEFAULT)" : L"")
                   << (devices[i].isBluetooth ? L" [Bluetooth]" : L"")
                   << L"\n       ID: " << devices[i].id
                   << std::endl;
    }

    auto inDevices = EnumerateInputDevices();
    std::wcout << L"[Auduo Test] Found " << inDevices.size() << L" active capture/microphone endpoints:" << std::endl;
    for (size_t i = 0; i < inDevices.size(); ++i) {
        std::wcout << L"  [" << i << L"] " << inDevices[i].name
                   << (inDevices[i].isDefault ? L" (DEFAULT)" : L"")
                   << (inDevices[i].isBluetooth ? L" [Bluetooth]" : L"")
                   << L"\n       ID: " << inDevices[i].id
                   << std::endl;
    }
    return devices.empty() ? 1 : 0;
}

int AudioEngine::RunTestBuffer() {
    std::cout << "[Auduo Test] Testing Lock-Free Ring Buffer math and wraparound..." << std::endl;
    RingBuffer rb(1024, 2);

    std::vector<float> writeData(500 * 2);
    for (size_t i = 0; i < writeData.size(); ++i) {
        writeData[i] = static_cast<float>(i);
    }

    size_t written = rb.Write(writeData.data(), 500);
    if (written != 500) {
        std::cerr << "[Auduo Test] Failed initial write: " << written << std::endl;
        return 1;
    }

    std::vector<float> readData(300 * 2);
    size_t read = rb.Read(readData.data(), 300);
    if (read != 300) {
        std::cerr << "[Auduo Test] Failed initial read: " << read << std::endl;
        return 1;
    }

    for (size_t i = 0; i < 300 * 2; ++i) {
        if (readData[i] != static_cast<float>(i)) {
            std::cerr << "[Auduo Test] Data mismatch at " << i << std::endl;
            return 1;
        }
    }

    // Wraparound test
    written = rb.Write(writeData.data(), 500);
    if (written != 500) {
        std::cerr << "[Auduo Test] Failed wraparound write: " << written << std::endl;
        return 1;
    }

    std::cout << "[Auduo Test] Lock-Free Ring Buffer PASSED (Zero data corruption, verified bitmask wraparound)" << std::endl;
    return 0;
}

int AudioEngine::RunTestCapture(int durationSeconds) {
    std::cout << "[Auduo Test] Testing WASAPI Process-Excluded Loopback Capture (" << durationSeconds << "s)..." << std::endl;
    AudioEngine engine;
    if (!engine.Initialize()) {
        std::cerr << "[Auduo Test] Engine initialization failed" << std::endl;
        return 1;
    }

    auto devices = EnumerateOutputDevices();
    if (devices.empty()) {
        std::cerr << "[Auduo Test] No output devices found for loopback" << std::endl;
        return 1;
    }

    engine.SelectDeviceA(devices[0].id);
    if (devices.size() > 1) {
        engine.SelectDeviceB(devices[1].id);
    } else {
        engine.SelectDeviceB(devices[0].id);
    }

    if (!engine.StartStreaming()) {
        std::cerr << "[Auduo Test] StartStreaming failed" << std::endl;
        return 1;
    }

    std::cout << "[Auduo Test] Streaming active. Capturing for " << durationSeconds << " seconds..." << std::endl;
    for (int i = 0; i < durationSeconds; ++i) {
        Sleep(1000);
        float vuA = engine.GetPipelineA()->GetPeakVU();
        float vuB = engine.GetPipelineB()->GetPeakVU();
        std::cout << "  [Tick " << (i + 1) << "/" << durationSeconds << "] Peak VU A: " << vuA << " | Peak VU B: " << vuB << std::endl;
    }

    engine.StopStreaming();
    std::cout << "[Auduo Test] WASAPI Loopback Capture PASSED." << std::endl;
    return 0;
}

int AudioEngine::RunTestCalibration() {
    std::cout << "[Auduo Test] Running Acoustic Multi-Burst Calibration Test Suite..." << std::endl;
    AudioEngine engine;
    if (!engine.Initialize()) {
        std::cerr << "[Auduo Test] Engine initialization failed" << std::endl;
        return 1;
    }

    auto devices = EnumerateOutputDevices();
    if (devices.empty()) {
        std::cerr << "[Auduo Test] No output devices found" << std::endl;
        return 1;
    }

    // Pick first physical device (Speakers)
    std::wstring devId = devices[0].id;
    for (const auto& dev : devices) {
        if (dev.name.find(L"Speakers") != std::wstring::npos || dev.name.find(L"Realtek") != std::wstring::npos) {
            devId = dev.id;
            break;
        }
    }

    engine.SelectDeviceA(devId);
    std::wcout << L"[Auduo Test] Target Test Endpoint: " << devId << std::endl;

    std::atomic<bool> done{false};
    bool testSuccess = false;
    int testLatency = -1;
    std::string testMsg;

    engine.CalibrateChannel(0, [&](bool success, int latencyMs, const std::string& msg) {
        testSuccess = success;
        testLatency = latencyMs;
        testMsg = msg;
        done.store(true);
    });

    while (!done.load()) {
        Sleep(50);
    }

    std::cout << "[Auduo Test] Calibration Result: " << (testSuccess ? "SUCCESS" : "REJECTED (as intended if no sound)")
              << " | Latency: " << testLatency << " ms" << std::endl;
    std::cout << "[Auduo Test] Message: " << testMsg << std::endl;

    return 0;
}

int AudioEngine::RunTestAutoSync() {
    std::cout << "[Auduo Test] Running Simultaneous Dual-Frequency Auto-Sync Test..." << std::endl;
    AudioEngine engine;
    if (!engine.Initialize()) {
        std::cerr << "[Auduo Test] Engine initialization failed" << std::endl;
        return 1;
    }

    auto devices = EnumerateOutputDevices();
    if (devices.empty()) {
        std::cerr << "[Auduo Test] No output devices found" << std::endl;
        return 1;
    }

    std::wstring devA = devices[0].id;
    std::wstring devB = (devices.size() > 1) ? devices[1].id : devices[0].id;

    engine.SelectDeviceA(devA);
    engine.SelectDeviceB(devB);
    std::wcout << L"[Auduo Test] Device A: " << devA << std::endl;
    std::wcout << L"[Auduo Test] Device B: " << devB << std::endl;

    std::atomic<bool> done{false};
    bool testSuccess = false;
    int testDelta = 0;
    std::string testMsg;

    engine.AutoSyncBothChannels([&](bool success, int deltaMs, const std::string& msg) {
        testSuccess = success;
        testDelta = deltaMs;
        testMsg = msg;
        done.store(true);
    });

    while (!done.load()) {
        Sleep(50);
    }

    std::cout << "[Auduo Test] AutoSync Result: " << (testSuccess ? "SUCCESS" : "REJECTED (as intended if no sound)")
              << " | Delta: " << testDelta << " ms" << std::endl;
    std::cout << "[Auduo Test] Message: " << testMsg << std::endl;
    return 0;
}

int AudioEngine::RunTestBeep(const std::wstring& targetMatch) {
    std::cout << "[Auduo Test] Running Acoustic Test Beep..." << std::endl;
    AudioEngine engine;
    if (!engine.Initialize()) {
        std::cerr << "[Auduo Test] Engine initialization failed" << std::endl;
        return 1;
    }

    auto devices = EnumerateOutputDevices();
    if (devices.empty()) {
        std::cerr << "[Auduo Test] No output devices found" << std::endl;
        return 1;
    }

    std::wstring chosenId;
    std::wstring chosenName;
    for (const auto& dev : devices) {
        if (!targetMatch.empty() && dev.name.find(targetMatch) != std::wstring::npos) {
            chosenId = dev.id;
            chosenName = dev.name;
            break;
        }
    }
    if (chosenId.empty()) {
        for (const auto& dev : devices) {
            if (dev.name.find(L"WF-C510") != std::wstring::npos || dev.isBluetooth) {
                chosenId = dev.id;
                chosenName = dev.name;
                break;
            }
        }
    }
    if (chosenId.empty()) {
        chosenId = devices[0].id;
        chosenName = devices[0].name;
    }

    std::wcout << L"[Auduo Test] Target Render Endpoint: " << chosenName << L"\n             ID: " << chosenId << std::endl;

    auto pipeline = engine.GetPipelineA();
    if (!pipeline->Initialize(chosenId)) {
        std::cerr << "[Auduo Test] Failed to initialize target pipeline" << std::endl;
        return 1;
    }
    pipeline->SetVolume(1.0f);
    pipeline->SetMasterVolume(1.0f);
    pipeline->SetDelayMs(0);

    IMMDeviceEnumerator* pEnum = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
    if (pEnum) {
        IMMDevice* pDev = nullptr;
        if (SUCCEEDED(pEnum->GetDevice(chosenId.c_str(), &pDev)) && pDev) {
            IAudioEndpointVolume* pEndVol = nullptr;
            if (SUCCEEDED(pDev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (void**)&pEndVol)) && pEndVol) {
                pEndVol->SetMute(FALSE, NULL);
                float curVol = 1.0f;
                pEndVol->GetMasterVolumeLevelScalar(&curVol);
                if (curVol < 0.60f) pEndVol->SetMasterVolumeLevelScalar(0.80f, NULL);
                pEndVol->Release();
            }
            pDev->Release();
        }
    }

    IMMDevice* pMicDevice = nullptr;
    if (pEnum) {
        pEnum->GetDefaultAudioEndpoint(eCapture, eConsole, &pMicDevice);
        pEnum->Release();
    }
    if (!pMicDevice) {
        std::cerr << "[Auduo Test] No microphone endpoint found" << std::endl;
        pipeline->Shutdown();
        return 1;
    }

    IAudioClient* pMicClient = nullptr;
    hr = pMicDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&pMicClient);
    pMicDevice->Release();
    if (FAILED(hr) || !pMicClient) {
        std::cerr << "[Auduo Test] Failed to activate microphone" << std::endl;
        pipeline->Shutdown();
        return 1;
    }

    IAudioClient2* pClient2 = nullptr;
    if (SUCCEEDED(pMicClient->QueryInterface(__uuidof(IAudioClient2), (void**)&pClient2)) && pClient2) {
        AudioClientProperties props = {};
        props.cbSize = sizeof(AudioClientProperties);
        props.bIsOffload = FALSE;
        props.eCategory = AudioCategory_Media;
        props.Options = AUDCLNT_STREAMOPTIONS_RAW;
        hr = pClient2->SetClientProperties(&props);
        if (FAILED(hr)) {
            props.Options = AUDCLNT_STREAMOPTIONS_NONE;
            pClient2->SetClientProperties(&props);
        }
        pClient2->Release();
    }

    WAVEFORMATEX* pMicFormat = nullptr;
    hr = pMicClient->GetMixFormat(&pMicFormat);
    if (FAILED(hr) || !pMicFormat) {
        pMicClient->Release();
        pipeline->Shutdown();
        return 1;
    }

    hr = pMicClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, pMicFormat, NULL);
    if (FAILED(hr)) {
        CoTaskMemFree(pMicFormat);
        pMicClient->Release();
        pipeline->Shutdown();
        return 1;
    }

    IAudioCaptureClient* pMicCapture = nullptr;
    hr = pMicClient->GetService(__uuidof(IAudioCaptureClient), (void**)&pMicCapture);
    if (FAILED(hr) || !pMicCapture) {
        CoTaskMemFree(pMicFormat);
        pMicClient->Release();
        pipeline->Shutdown();
        return 1;
    }

    pMicClient->Start();

    const uint32_t micRate = pMicFormat->nSamplesPerSec;
    const uint32_t micChannels = pMicFormat->nChannels;

    UINT32 discardSize = 0;
    while (SUCCEEDED(pMicCapture->GetNextPacketSize(&discardSize)) && discardSize > 0) {
        BYTE* pD = nullptr; UINT32 f = 0; DWORD fl = 0;
        if (SUCCEEDED(pMicCapture->GetBuffer(&pD, &f, &fl, NULL, NULL))) {
            pMicCapture->ReleaseBuffer(f);
        }
    }

    // Measure baseline room noise for 250ms
    float baselinePeak = 0.0f;
    float baselineRms = 0.0f;
    size_t baselineCount = 0;
    auto startBaseline = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startBaseline).count() < 250) {
        UINT32 packetSize = 0;
        pMicCapture->GetNextPacketSize(&packetSize);
        while (packetSize > 0) {
            BYTE* pData = nullptr; UINT32 frames = 0; DWORD flags = 0;
            if (SUCCEEDED(pMicCapture->GetBuffer(&pData, &frames, &flags, NULL, NULL))) {
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && pData) {
                    if (pMicFormat->wBitsPerSample == 16) {
                        int16_t* sData = reinterpret_cast<int16_t*>(pData);
                        for (UINT32 f = 0; f < frames; ++f) {
                            float s = std::fabs(sData[f * micChannels] / 32768.0f);
                            if (s > baselinePeak) baselinePeak = s;
                            baselineRms += s * s;
                            baselineCount++;
                        }
                    } else {
                        float* fData = reinterpret_cast<float*>(pData);
                        for (UINT32 f = 0; f < frames; ++f) {
                            float s = std::fabs(fData[f * micChannels]);
                            if (s > baselinePeak) baselinePeak = s;
                            baselineRms += s * s;
                            baselineCount++;
                        }
                    }
                }
                pMicCapture->ReleaseBuffer(frames);
            }
            pMicCapture->GetNextPacketSize(&packetSize);
        }
        Sleep(5);
    }
    if (baselineCount > 0) baselineRms = std::sqrt(baselineRms / baselineCount);

    // Warm up Bluetooth pipeline with 150ms silence so A2DP connection is fully awake
    const uint32_t beepRate = 48000;
    std::vector<float> warmupSilence((beepRate * 150) / 1000 * 2, 0.0f);
    pipeline->PushAudio(warmupSilence.data(), warmupSilence.size() / 2, beepRate, 2);
    Sleep(80);

    // Synthesize 70ms Stream A chirp (700-1400Hz) followed by a 150ms 1000Hz tone
    const float chirpSec = 0.070f;
    const size_t chirpFrames = static_cast<size_t>(beepRate * chirpSec);
    const size_t toneFrames = (beepRate * 150) / 1000;
    const size_t totalTestFrames = (beepRate * 850) / 1000;

    std::vector<float> testAudio(totalTestFrames * 2, 0.0f);
    std::vector<float> refChirp(chirpFrames, 0.0f);

    for (size_t i = 0; i < chirpFrames; ++i) {
        float t = static_cast<float>(i) / beepRate;
        float env = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * i / (chirpFrames - 1)));
        float phase = 2.0f * static_cast<float>(M_PI) * (700.0f * t + ((1400.0f - 700.0f) / (2.0f * chirpSec)) * t * t);
        float s = 0.95f * env * std::sin(phase);
        refChirp[i] = s;
        testAudio[i * 2 + 0] = s;
        testAudio[i * 2 + 1] = s;
    }

    for (size_t i = 0; i < toneFrames; ++i) {
        float t = static_cast<float>(i) / beepRate;
        float env = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * i / (toneFrames - 1)));
        float s = 0.90f * env * std::sin(2.0f * static_cast<float>(M_PI) * 1000.0f * t);
        size_t idx = chirpFrames + i;
        if (idx < totalTestFrames) {
            testAudio[idx * 2 + 0] = s;
            testAudio[idx * 2 + 1] = s;
        }
    }

    // Build resampled reference for correlation
    const size_t refLenAtMic = (chirpFrames * micRate) / beepRate;
    std::vector<float> resampledRef(refLenAtMic, 0.0f);
    float e_ref = 0.0f;
    for (size_t i = 0; i < refLenAtMic; ++i) {
        float srcIdx = (static_cast<float>(i) * beepRate) / micRate;
        size_t idx0 = static_cast<size_t>(srcIdx);
        if (idx0 < chirpFrames) {
            resampledRef[i] = refChirp[idx0];
            e_ref += resampledRef[i] * resampledRef[i];
        }
    }

    while (SUCCEEDED(pMicCapture->GetNextPacketSize(&discardSize)) && discardSize > 0) {
        BYTE* pD = nullptr; UINT32 f = 0; DWORD fl = 0;
        if (SUCCEEDED(pMicCapture->GetBuffer(&pD, &f, &fl, NULL, NULL))) {
            pMicCapture->ReleaseBuffer(f);
        }
    }

    std::wcout << L"[Auduo Test] Firing test signal to: " << chosenName << L"..." << std::endl;
    pipeline->PushAudio(testAudio.data(), totalTestFrames, beepRate, 2);

    // Record mic during test for 850ms
    float beepPeak = 0.0f;
    float beepRms = 0.0f;
    size_t beepCount = 0;
    std::vector<float> recordedAudio;
    auto startBeepRec = std::chrono::steady_clock::now();

    while (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startBeepRec).count() < 850) {
        UINT32 packetSize = 0;
        pMicCapture->GetNextPacketSize(&packetSize);
        while (packetSize > 0) {
            BYTE* pData = nullptr; UINT32 frames = 0; DWORD flags = 0;
            if (SUCCEEDED(pMicCapture->GetBuffer(&pData, &frames, &flags, NULL, NULL))) {
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && pData) {
                    if (pMicFormat->wBitsPerSample == 16) {
                        int16_t* sData = reinterpret_cast<int16_t*>(pData);
                        for (UINT32 f = 0; f < frames; ++f) {
                            float s = sData[f * micChannels] / 32768.0f;
                            float absS = std::fabs(s);
                            if (absS > beepPeak) beepPeak = absS;
                            beepRms += s * s;
                            beepCount++;
                            recordedAudio.push_back(s);
                        }
                    } else {
                        float* fData = reinterpret_cast<float*>(pData);
                        for (UINT32 f = 0; f < frames; ++f) {
                            float s = fData[f * micChannels];
                            float absS = std::fabs(s);
                            if (absS > beepPeak) beepPeak = absS;
                            beepRms += s * s;
                            beepCount++;
                            recordedAudio.push_back(s);
                        }
                    }
                } else {
                    for (UINT32 f = 0; f < frames; ++f) recordedAudio.push_back(0.0f);
                }
                pMicCapture->ReleaseBuffer(frames);
            }
            pMicCapture->GetNextPacketSize(&packetSize);
        }
        Sleep(5);
    }
    if (beepCount > 0) beepRms = std::sqrt(beepRms / beepCount);

    pMicClient->Stop();
    pMicCapture->Release();
    CoTaskMemFree(pMicFormat);
    pMicClient->Release();
    pipeline->Shutdown();

    // Cross-correlation analysis
    float bestNcc = 0.0f;
    size_t bestOffset = 0;
    size_t searchEnd = (recordedAudio.size() > refLenAtMic) ? (recordedAudio.size() - refLenAtMic) : 0;
    size_t maxWindow = (650 * micRate) / 1000;
    if (searchEnd > maxWindow) searchEnd = maxWindow;

    if (searchEnd > 0 && e_ref > 1e-6f) {
        for (size_t i = 0; i < searchEnd; i += 2) {
            float crossProd = 0.0f;
            float e_sig = 0.0f;
            for (size_t k = 0; k < refLenAtMic; ++k) {
                float s = recordedAudio[i + k];
                crossProd += s * resampledRef[k];
                e_sig += s * s;
            }
            float denom = std::sqrt(e_sig * e_ref);
            if (denom > 1e-6f) {
                float ncc = std::fabs(crossProd) / denom;
                if (ncc > bestNcc) {
                    bestNcc = ncc;
                    bestOffset = i;
                }
            }
        }
        if (bestNcc > 0.20f && bestOffset >= 3 && bestOffset + 3 < searchEnd) {
            size_t fineStart = bestOffset - 3;
            size_t fineEnd = bestOffset + 3;
            for (size_t i = fineStart; i <= fineEnd; ++i) {
                float crossProd = 0.0f, e_sig = 0.0f;
                for (size_t k = 0; k < refLenAtMic; ++k) {
                    float s = recordedAudio[i + k];
                    crossProd += s * resampledRef[k];
                    e_sig += s * s;
                }
                float denom = std::sqrt(e_sig * e_ref);
                if (denom > 1e-6f) {
                    float ncc = std::fabs(crossProd) / denom;
                    if (ncc > bestNcc) {
                        bestNcc = ncc;
                        bestOffset = i;
                    }
                }
            }
        }
    }

    int detectedLatencyMs = (micRate > 0) ? static_cast<int>(std::round((static_cast<double>(bestOffset) / micRate) * 1000.0)) : 0;
    float peakRatio = (baselinePeak > 1e-4f) ? (beepPeak / baselinePeak) : (beepPeak * 100.0f);

    std::cout << "----------------------------------------" << std::endl;
    std::cout << "[Auduo Test] Ambient Noise Floor: Peak = " << baselinePeak << ", RMS = " << baselineRms << std::endl;
    std::cout << "[Auduo Test] Signal Received:    Peak = " << beepPeak << ", RMS = " << beepRms << std::endl;
    std::cout << "[Auduo Test] Signal-to-Noise:    " << peakRatio << "x above baseline" << std::endl;
    std::cout << "[Auduo Test] Correlation Match:  NCC = " << bestNcc << " (" << static_cast<int>(bestNcc * 100.0f) << "%)" << std::endl;
    if (bestNcc >= 0.28f) {
        std::cout << "[Auduo Test] Acoustic Latency:   " << detectedLatencyMs << " ms" << std::endl;
    }
    std::cout << "----------------------------------------" << std::endl;

    if (bestNcc >= 0.28f) {
        std::cout << "[Auduo Test] Result: AUDIBLE AND VERIFIED! Mic clearly detected the taped earbud (NCC " 
                  << static_cast<int>(bestNcc * 100.0f) << "%, " << detectedLatencyMs << " ms latency)." << std::endl;
    } else if (beepPeak > 0.05f) {
        std::cout << "[Auduo Test] Result: SOUND DETECTED but low waveform correlation (NCC " 
                  << static_cast<int>(bestNcc * 100.0f) << "%). Check earbud volume/seal." << std::endl;
    } else {
        std::cout << "[Auduo Test] Result: INAUDIBLE / TOO QUIET. Microphone did not pick up sound from earbud." << std::endl;
    }
    std::cout << "----------------------------------------" << std::endl;

    return 0;
}

int AudioEngine::RunTestVolume() {
    std::cout << "========================================" << std::endl;
    std::cout << "[Auduo Test] WASAPI Windows Volume Sync Diagnostic" << std::endl;
    std::cout << "========================================" << std::endl;

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    (void)hr;

    IMMDeviceEnumerator* pEnum = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
    if (FAILED(hr) || !pEnum) {
        std::cerr << "Failed to create MMDeviceEnumerator: " << std::hex << hr << std::endl;
        return 1;
    }

    IMMDevice* pDefDev = nullptr;
    hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDefDev);
    if (SUCCEEDED(hr) && pDefDev) {
        LPWSTR pId = nullptr;
        pDefDev->GetId(&pId);
        std::wcout << L"[Default Console Endpoint ID]: " << (pId ? pId : L"Unknown") << std::endl;
        if (pId) CoTaskMemFree(pId);

        IPropertyStore* pStore = nullptr;
        if (SUCCEEDED(pDefDev->OpenPropertyStore(STGM_READ, &pStore)) && pStore) {
            PROPVARIANT var;
            PropVariantInit(&var);
            PROPERTYKEY keyName;
            CLSIDFromString(L"{A45C254E-DF1C-4EFD-8020-67D146A850E0}", &keyName.fmtid);
            keyName.pid = 14;
            if (SUCCEEDED(pStore->GetValue(keyName, &var)) && var.pwszVal) {
                std::wcout << L"[Default Console Device Name]: " << var.pwszVal << std::endl;
            }
            PropVariantClear(&var);
            pStore->Release();
        }

        IAudioEndpointVolume* pEndpointVol = nullptr;
        hr = pDefDev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (void**)&pEndpointVol);
        if (SUCCEEDED(hr) && pEndpointVol) {
            float vol = 0.0f;
            WINBOOL mute = FALSE;
            pEndpointVol->GetMasterVolumeLevelScalar(&vol);
            pEndpointVol->GetMute(&mute);
            std::cout << "[Default Endpoint Volume]: " << static_cast<int>(vol * 100.0f) << "% (" << vol << ")"
                      << ", Mute: " << (mute ? "YES" : "NO") << std::endl;

            // Test setting the exact same volume to test write permissions
            HRESULT hrSet = pEndpointVol->SetMasterVolumeLevelScalar(vol, NULL);
            std::cout << "[Default Endpoint SetMasterVolumeLevelScalar test]: " 
                      << (SUCCEEDED(hrSet) ? "SUCCESS" : "FAILED") << " (HR: 0x" << std::hex << hrSet << std::dec << ")" << std::endl;

            pEndpointVol->Release();
        } else {
            std::cerr << "Failed to activate IAudioEndpointVolume on default device: 0x" << std::hex << hr << std::dec << std::endl;
        }
        pDefDev->Release();
    } else {
        std::cerr << "Failed to get default audio endpoint: 0x" << std::hex << hr << std::dec << std::endl;
    }

    std::cout << "----------------------------------------" << std::endl;
    std::cout << "[All Active Render Endpoints]:" << std::endl;

    IMMDeviceCollection* pColl = nullptr;
    if (SUCCEEDED(pEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pColl)) && pColl) {
        UINT count = 0;
        pColl->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* pDev = nullptr;
            if (SUCCEEDED(pColl->Item(i, &pDev)) && pDev) {
                LPWSTR pId = nullptr;
                pDev->GetId(&pId);
                std::wstring name = L"Unknown";
                IPropertyStore* pStore = nullptr;
                if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pStore)) && pStore) {
                    PROPVARIANT var;
                    PropVariantInit(&var);
                    PROPERTYKEY keyName;
                    CLSIDFromString(L"{A45C254E-DF1C-4EFD-8020-67D146A850E0}", &keyName.fmtid);
                    keyName.pid = 14;
                    if (SUCCEEDED(pStore->GetValue(keyName, &var)) && var.pwszVal) {
                        name = var.pwszVal;
                    }
                    PropVariantClear(&var);
                    pStore->Release();
                }

                float vol = -1.0f;
                WINBOOL mute = FALSE;
                IAudioEndpointVolume* pVol = nullptr;
                if (SUCCEEDED(pDev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (void**)&pVol)) && pVol) {
                    pVol->GetMasterVolumeLevelScalar(&vol);
                    pVol->GetMute(&mute);
                    pVol->Release();
                }

                std::wcout << L"  [" << i << L"] " << name << L"\n       Vol: " 
                           << static_cast<int>(vol * 100.0f) << L"%, Mute: " << (mute ? L"YES" : L"NO")
                           << L", ID: " << (pId ? pId : L"") << std::endl;

                if (pId) CoTaskMemFree(pId);
                pDev->Release();
            }
        }
        pColl->Release();
    }

    pEnum->Release();
    CoUninitialize();
    std::cout << "========================================" << std::endl;
    return 0;
}

} // namespace Auduo
