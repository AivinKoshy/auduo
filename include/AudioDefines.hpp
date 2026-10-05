#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>

#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <atomic>

// GUIDs / COM Interface IDs needed for modern Windows Loopback
#ifndef __AUDCLNT_PROCESS_LOOPBACK_PARAMS_DEFINED__
#define __AUDCLNT_PROCESS_LOOPBACK_PARAMS_DEFINED__
typedef enum PROCESS_LOOPBACK_MODE {
    PROCESS_LOOPBACK_MODE_INCLUDE_PROCESS_TREE = 0,
    PROCESS_LOOPBACK_MODE_EXCLUDE_PROCESS_TREE = 1
} PROCESS_LOOPBACK_MODE;

typedef struct AUDCLNT_PROCESS_LOOPBACK_PARAMS {
    DWORD TargetProcessId;
    PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
} AUDCLNT_PROCESS_LOOPBACK_PARAMS;
#endif

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif

#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

namespace Auduo {

struct AudioDevice {
    std::wstring id;
    std::wstring name;
    bool isBluetooth = false;
    bool isDefault = false;
};

struct StreamMetrics {
    std::atomic<float> peakVU{0.0f};
    std::atomic<uint64_t> underrunCount{0};
    std::atomic<uint64_t> overflowCount{0};
    std::atomic<int> latencyMs{0};
};

enum class CaptureMode {
    SystemMirror,    // Captures all system audio, excluding Auduo itself
    ProcessSpecific  // Loopback targeting a specific process ID
};

} // namespace Auduo
