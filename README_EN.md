# Delta_Cast ASIO

Low-latency ASIO proxy routing driver for rhythm gamers
<br>
Developed by preez in Studio Delta Works

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Windows_x64_|_x86-lightgrey.svg)
![Build](https://img.shields.io/badge/build-Visual_Studio_2026-purple.svg)

[🇰🇷 한국어](README.md) 👈 한국어 문서는 여기를 클릭하세요

---

## Introduction

Delta_Cast was created to solve the audio capture problem that rhythm gamers face when streaming (OBS, Discord, etc.) while using an audio interface over ASIO.

ASIO drivers bypass the Windows audio mixer, so streaming software cannot capture the game audio. Working around this with a mixer such as Voicemeeter adds latency to the game audio itself.

Delta_Cast is a proxy driver that sits between the game and your real ASIO driver. Game audio goes straight to the hardware with no added latency, while a copy of the same audio is sent to a separate WASAPI device (such as a virtual cable) for streaming.

```
Game ──▶ Delta_Cast ──▶ Real ASIO driver ──▶ Headphones/Speakers   (game path: no added latency)
              │
              └─ copy ──▶ WASAPI device (VB-CABLE, etc.) ──▶ OBS / Discord   (broadcast path)
```

## Key Features

* **No added latency on the game path**
    - Game audio uses the real ASIO driver's buffers directly.
    - The ASIO callback only performs a memory copy: no allocation, no locks, no waiting.
* **Lock-free design**
    - The callback and the broadcast thread are connected by a lock-free ring buffer.
    - A slow broadcast path never affects the game audio path.
* **Low-latency broadcast (v1.3.0+)**
    - On Windows 10 and later, the WASAPI low-latency shared mode (`IAudioClient3`) is used automatically.
    - Devices that do not support it fall back to the standard shared mode.
    - If the broadcast device's sample rate differs from the game's, the built-in resampler converts it.
* **Clock drift correction (v1.3.2+)**
    - Broadcast latency no longer grows and the audio no longer drops out, even during long streams.
    - Enabled by default. To turn it off, add `DriftCorrection=0` under `[Settings]` in `Delta_Cast.ini`.
* **Wide format support**
    - ASIO sample formats: Int16 / Int24 / Int32 / Float32 / Float64
    - Supports both 64-bit (`Delta_Cast.dll`) and 32-bit (`Delta_Cast_x86.dll`) games
* **Virtual ASIO mode (compatibility)**
    - Creates a virtual ASIO device for systems without an audio interface.
    - Output goes through the Windows shared mode, so expect **tens of milliseconds of latency**. If you have an ASIO device, proxy mode is recommended.

## Installation

1. Download the latest version from the [Releases](../../releases) page.
2. Extract the archive and run `Delta_Cast_GUI.exe` (administrator rights required).
3. **Select Real ASIO Hardware:** Choose the ASIO driver of your audio interface (e.g. Steinberg UR22, Focusrite). If you do not have an audio interface, choose `[Virtual]` at the top.
4. **Select Loopback Output:** Choose the device that will receive the broadcast audio.
    - Proxy mode: a virtual cable such as VB-CABLE is recommended.
    - Virtual mode: this device plays the actual sound, so choose your **headphones or speakers**.
5. Choose a **Latency Mode** (see the table below).
6. Click **[Save Config]**, then **[Init Driver]** to register the driver.

### Broadcast Latency Modes

This is the buffer size of the broadcast path. It does not affect game audio latency. Smaller values make the broadcast audio faster but more prone to dropouts.

| Mode | Buffer | At 48kHz / 32-bit |
|---|---|---|
| 0 | 16384 bytes | approx. 85 ms |
| 1 | 8192 bytes | approx. 43 ms |
| 2 | 4096 bytes | approx. 21 ms |
| 3 | 2048 bytes | approx. 11 ms |
| 4 | 1024 bytes | approx. 5 ms |

If the broadcast audio stutters, raise the mode one step at a time. A constant offset can be compensated with OBS's audio sync offset.

> **Virtual cable setup tip**
> If you use VB-CABLE, set the format of both `CABLE Input` (playback) and `CABLE Output` (recording) to **48000 Hz** in the Windows sound settings.
> Any other value makes the cable convert the sample rate internally, which can roll off the high frequencies of the broadcast audio.
> Keep the cable at 48 kHz even if the game's ASIO sample rate is not 48 kHz. Delta_Cast handles the conversion automatically.

## Usage

1. Launch your rhythm game.
2. In the game's audio settings, select **Delta_Cast ASIO** as the output device.
3. In OBS / Discord, add the broadcast device you selected above (e.g. CABLE Output) as an audio input.

## Updating

1. Close the game. (A running game keeps using the previous version until it is closed.)
2. Overwrite the existing files with the new version.
3. If the install location has not changed, there is no need to register again. Your settings are kept.

## Troubleshooting

Run the game with [DebugView](https://learn.microsoft.com/sysinternals/downloads/debugview) open, and you will see log lines starting with `[Delta_Cast]`.

```
[Delta_Cast] [INFO] Mode: Proxy -> {...} | LatencyMode: 2
[Delta_Cast] [INFO] Stream: 48000 Hz | Int32 | buffer 64 frames (1.33 ms) | out idx 2/3 (ch 0/1)
[Delta_Cast] [INFO] WASAPI: CABLE Input (VB-Audio Virtual Cable) | 48000 Hz 2 ch 32-bit float | low-latency (IAudioClient3) | resample ON | drift correction ON
[Delta_Cast] [INFO] Drift: -42.1 ppm (learned -40.2) | ring 21.3 ms (target 21.3 ms) | resync 0
```

* Lines containing `[ERR]` include the cause of the error.
* If you see the fallback-to-Virtual message, the selected ASIO driver could not be opened. Check that the driver is installed and that no other program is using the device.
* When reporting an issue, please attach the `[Delta_Cast]` log lines.
* The `Drift:` line is printed once a minute. If the `resync` count keeps rising, the clock difference to the broadcast device is beyond the correction range. Please report it together with the log.

## Known Limitations

* Clock drift correction works in proxy mode only.
* In virtual mode, the latency reported to the game is lower than the actual latency.
* Only the first two output channels (stereo) are broadcast.

## Building from Source

**Requirements**
- Visual Studio 2026
- Windows 10/11 SDK
- Steinberg ASIO SDK (must be downloaded separately due to licensing)

**Folder layout**
```
ParentFolder\
  ├ ASIOSDK\common\...     ← ASIO SDK
  └ Delta_Cast\            ← this repository
```

**Build steps**
1. Clone this repository.
2. Extract the ASIO SDK into an `ASIOSDK` folder next to the repository.
3. Open `Delta_Cast.slnx` in Visual Studio 2026.
4. Build **Release / x64** (64-bit) or **Release / Win32** (32-bit).

**Code signing (optional)**

If the environment variable `DC_SIGN_THUMBPRINT` is set to the thumbprint of a code-signing certificate, the output is signed automatically after the build. If it is not set, the signing step is skipped.

```powershell
[Environment]::SetEnvironmentVariable("DC_SIGN_THUMBPRINT", "<certificate thumbprint>", "User")
```

## License

This project is distributed under the **MIT License**. You are free to modify and redistribute it. See the [LICENSE](LICENSE) file for details.

Copyright (c) 2025-2026 **Studio Delta Works**
