# AuDuo Project Roadmap & Milestone Tracker

## 📌 v1.0.0 — The Initial Stable Release (Current)
Focused on rock-solid, low-latency **Dual Audio Mirroring & Acoustic Auto-Sync** for Windows.

- [x] **Core WASAPI Dual Mirroring Engine:** Lock-free ring buffer architecture cloning system audio to two independent output endpoints simultaneously.
- [x] **Acoustic Auto-Sync (Differential Dual-Chirp):** Simultaneous orthogonal frequency chirps (700–1400 Hz on Stream A, 3200–5200 Hz on Stream B) with cross-correlation to automatically detect and apply latency delta in one click.
- [x] **Studio Metronome Sync Clapper:** Periodic woodblock clapper click for instant auditory verification of sync alignment.
- [x] **Hardware Master Volume Sync:** Bidirectional synchronization with laptop physical volume keys (Fn+Vol) and Windows taskbar volume via `IAudioEndpointVolumeCallback`.
- [x] **Hardware Calibration Mic Selector:** Dedicated dropdown to explicitly select the capture microphone (with auto-priority for built-in laptop mic array over dangling headphone cable mics).
- [x] **Device Hot-Plugging & Virtual Cable Exclusion:** Instant rescan (`⟳`) for newly connected Bluetooth/wired gear, filtering out virtual cable clutter from user-facing dropdowns.
- [x] **Windows Installer & Setup:** Inno Setup package (`AuDuo_Setup_v1.0.0.exe`) with on-demand VB-Audio Virtual Cable download, driver setup, and third-party acknowledgements.

---

## 🚀 v2.0.0 — Planned Features & Technical Backlog

### 1. Per-App Audio Routing (Split Audio / DJ Mode)
- **Architecture:** Implement Windows 10 (Build 19041+) and Windows 11 `ActivateAudioInterfaceAsync` process-loopback capture (`AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`).
- **Functionality:** 
  - Allow Stream A to isolate and play audio strictly from Process 1 (e.g., Spotify) through Device A.
  - Allow Stream B to isolate and play audio strictly from Process 2 (e.g., VLC or YouTube browser tab) through Device B.
- **Engine Requirements:**
  - Dual asynchronous COM activation handlers (`IActivateAudioInterfaceCompletionHandler`).
  - Independent capture loops per selected process with dynamic PID tracking and graceful re-attach on app restarts.

### 2. Multi-Channel Expansion (>2 Devices)
- Expand engine architecture to support $N$-channel playback (Stream A, B, C...) for sharing across 3+ simultaneous headphones/speakers.

### 3. Linux Support
- Native PipeWire / PulseAudio graph implementation using `module-null-sink` and `module-loopback` without requiring virtual audio drivers.

### 4. Bluetooth LE Audio / Auracast Exploration
- Direct integration with Bluetooth 5.2+ broadcast audio for native multi-device streaming where hardware controllers support it.
