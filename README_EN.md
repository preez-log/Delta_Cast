# Delta_Cast ASIO

Zero-Latency ASIO Routing Driver for Rhythm Gamers
<br>
Developed by preez in Studio Delta Works

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Windows_x64-lightgrey.svg)
![Build](https://img.shields.io/badge/build-Visual_Studio_2026-purple.svg)

[🇰🇷 한국어 버전](README.md) 👈 Click here for Korean

---

## Introduction
Delta_Cast was developed to solve audio capture issues experienced by rhythm gamers using Audio Interfaces (ASIO) during broadcasting (OBS, Discord, etc.).

Existing ASIO drivers operate in exclusive mode, making broadcasting difficult. Furthermore, using software mixers like Voicemeeter for capture often introduces input lag (latency), which negatively affects gameplay performance.

Delta_Cast is a proxy routing driver that passes audio data between the game and hardware with Zero-Latency (nanosecond scale) while simultaneously streaming internally cloned audio to WASAPI Loopback.

## Key Features

* **Zero-Latency (Pass-through):**
    - Directly passes the game's audio to the hardware driver immediately.
    - Designed with a structure where no memory allocation occurs inside the ASIO callback, achieving a theoretical latency of 0ms.
* **Lock-Free Architecture:**
    - Utilizes a Ring Buffer to eliminate synchronization costs between threads.
    - No Mutex or Critical Section is used, removing the risk of deadlocks.
* **Transmission Optimization:**
    - The internal Resampler converts audio to the standard 48kHz regardless of the input sample rate.
    - Includes logic for Clock Drift Correction and prevention.

## Installation

1.  Download the latest version from the [Releases](../../releases) page.
2.  Extract the files and run `DeltaCast_Config.exe`.
3.  **Select Real ASIO Hardware:** Choose the actual audio interface you are using (e.g., Steinberg UR22, Focusrite, FlexAsio, Asio4ALL, etc.).
4.  **Select Loopback Output:** Choose the Windows virtual device to output the sound to. (Installing VB Virtual Cable is recommended).
5.  Click the **[Save Config]** button, and then click **[Init Driver]** to register the driver.

## Usage

1.  Launch your Rhythm Game.
2.  Go to Sound Settings.
3.  Select Delta_Cast ASIO as the output device.

## Build from Source

Developers can modify the source code and build it themselves.

**Requirements:**
- Windows 10/11 SDK
- Visual Studio 2026 (C++ Desktop Development)
- Steinberg ASIO SDK 2.3 (Must be downloaded separately due to licensing)

**Build Steps:**
1.  Clone this repository.
2.  Open `Delta_Cast.sln` in Visual Studio 2022.
3.  Set the configuration to **Release / x64**.
4.  Build the solution.
5.  (Optional) Sign the DLL using `signtool`.

## License

This project is distributed under the **MIT License**. You are free to modify and distribute it. See the [LICENSE](LICENSE) file for details.

Copyright (c) 2025 **Studio Delta Works**
