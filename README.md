# Delta_Cast ASIO

리듬게이머를 위한 저지연 ASIO 프록시 라우팅 드라이버
<br>
Developed by preez in Studio Delta Works

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Windows_x64_|_x86-lightgrey.svg)
![Build](https://img.shields.io/badge/build-Visual_Studio_2026-purple.svg)

[🇺🇸 English Version](README_EN.md) 👈 Click here for English

---

## 소개

Delta_Cast는 오디오 인터페이스(ASIO)를 사용하는 리듬게이머가 방송(OBS, Discord 등)을 할 때 겪는 소리 캡처 문제를 해결하기 위해 만들었습니다.

ASIO 드라이버는 Windows 오디오 믹서를 거치지 않기 때문에 방송 프로그램이 게임 소리를 잡지 못합니다. 이를 해결하려고 Voicemeeter 같은 믹서를 쓰면 게임 소리 자체가 늦어지는 문제가 있습니다.

Delta_Cast는 게임과 실제 ASIO 드라이버 사이에 들어가는 프록시 드라이버입니다. 게임 소리는 지연 없이 그대로 하드웨어로 보내고, 같은 소리를 복제해서 별도의 WASAPI 장치(가상 케이블 등)로 송출합니다.

```
게임 ──▶ Delta_Cast ──▶ 실제 ASIO 드라이버 ──▶ 헤드폰/스피커   (게임 경로: 지연 추가 없음)
              │
              └─ 복제 ──▶ WASAPI 장치 (VB-CABLE 등) ──▶ OBS / Discord   (송출 경로)
```

## 핵심 기능

* **게임 경로에 지연을 더하지 않음**
    - 게임 오디오는 실제 ASIO 드라이버의 버퍼를 그대로 사용합니다.
    - ASIO 콜백 안에서는 메모리 복사만 수행하며, 메모리 할당·락·대기가 없습니다.
* **락-프리 구조**
    - 콜백과 송출 스레드 사이는 락-프리 링 버퍼로 연결됩니다.
    - 송출 쪽이 느려져도 게임 오디오 경로는 영향을 받지 않습니다.
* **저지연 송출 (v1.3.0+)**
    - Windows 10 이상에서 WASAPI 저지연 공유 모드(`IAudioClient3`)를 자동으로 사용합니다.
    - 지원하지 않는 장치는 일반 공유 모드로 동작합니다.
    - 송출 장치의 샘플레이트가 게임과 다르면 내부 리샘플러로 변환합니다.
- **클럭 드리프트 보정 (v1.3.2+)** 
	- 장시간 방송에서도 송출 지연이 늘어나거나 끊기지 않습니다. 
	- 기본으로 켜져 있습니다. 끄려면 `Delta_Cast.ini`의 `[Settings]`에 `DriftCorrection=0`을 추가하세요.
* **다양한 포맷 지원**
    - ASIO 샘플 포맷: Int16 / Int24 / Int32 / Float32 / Float64
    - 64비트(`Delta_Cast.dll`)와 32비트(`Delta_Cast_x86.dll`) 게임 모두 지원
* **가상 ASIO 모드 (호환용)**
    - 오디오 인터페이스가 없는 환경에서 가상 ASIO 장치를 만들어 줍니다.
    - Windows 공유 모드를 거쳐 출력하므로 **수십 ms의 지연**이 있습니다. ASIO 장치가 있다면 프록시 모드를 권장합니다.

## 설치 방법

1. [Releases](../../releases) 페이지에서 최신 버전을 내려받습니다.
2. 압축을 풀고 `Delta_Cast_GUI.exe`를 실행합니다. (관리자 권한 필요)
3. **Select Real ASIO Hardware:** 사용 중인 오디오 인터페이스의 ASIO 드라이버(예: Steinberg UR22, Focusrite 등)를 선택합니다. 오디오 인터페이스가 없다면 맨 위의 `[Virtual]`을 선택합니다.
4. **Select Loopback Output:** 송출용 소리를 내보낼 장치를 선택합니다.
    - 프록시 모드: VB-CABLE 같은 가상 케이블을 권장합니다.
    - 가상 모드: 이 장치로 실제 소리가 나오므로 **헤드폰이나 스피커**를 선택합니다.
5. **Latency Mode**를 선택합니다. (아래 표 참고)
6. **[Save Config]** → **[Init Driver]** 순서로 눌러 드라이버를 등록합니다.

### 송출 지연 모드

송출 경로의 버퍼 크기입니다. 게임 소리의 지연과는 무관하며, 값이 작을수록 방송 소리가 빨라지는 대신 끊김에 민감해집니다.

| 모드 | 버퍼 | 48kHz / 32bit 기준 |
|---|---|---|
| 0 | 16384 bytes | 약 85 ms |
| 1 | 8192 bytes | 약 43 ms |
| 2 | 4096 bytes | 약 21 ms |
| 3 | 2048 bytes | 약 11 ms |
| 4 | 1024 bytes | 약 5 ms |

송출 소리가 끊기면 한 단계씩 큰 값으로 올려 보세요. 고정된 지연은 OBS의 오디오 싱크 오프셋으로 맞출 수 있습니다.

> **가상 케이블 설정 팁**
> VB-CABLE을 쓴다면 Windows 소리 설정에서 `CABLE Input`(재생)과 `CABLE Output`(녹음)의 형식을 모두 **48000 Hz**로 두세요.
> 다른 값으로 바꾸면 케이블 내부에서 샘플레이트 변환이 일어나 송출 소리의 고음역이 깎일 수 있습니다.
> 게임의 ASIO 샘플레이트가 48kHz가 아니어도 케이블은 48kHz로 두면 됩니다. Delta_Cast가 자동으로 변환합니다.

## 사용 방법

1. 리듬 게임을 실행합니다.
2. 게임의 오디오 설정에서 출력 장치로 **Delta_Cast ASIO**를 선택합니다.
3. OBS / Discord에서는 위에서 선택한 송출 장치(예: CABLE Output)를 오디오 입력으로 추가합니다.

## 업데이트 방법

1. 게임을 종료합니다. (실행 중인 게임은 종료 전 까지 이전 버전을 계속 사용합니다)
2. 기존 파일을 새 버전으로 덮어씁니다.
3. 설치 위치가 바뀌지 않았다면 다시 등록할 필요는 없습니다. 설정은 그대로 유지됩니다.

## 문제 해결

[DebugView](https://learn.microsoft.com/sysinternals/downloads/debugview)를 켠 상태로 게임을 실행하면 `[Delta_Cast]`로 시작하는 로그가 출력됩니다.

```
[Delta_Cast] [INFO] Mode: Proxy -> {...} | LatencyMode: 2 
[Delta_Cast] [INFO] Stream: 48000 Hz | Int32 | buffer 64 frames (1.33 ms) | out idx 2/3 (ch 0/1) 
[Delta_Cast] [INFO] WASAPI: CABLE Input (VB-Audio Virtual Cable) | 48000 Hz 2 ch 32-bit float | low-latency (IAudioClient3) | resample ON | drift correction ON 
[Delta_Cast] [INFO] Drift: -42.1 ppm (learned -40.2) | ring 21.3 ms (target 21.3 ms) | resync 0
```

* `[ERR]`가 포함된 줄이 있으면 원인이 함께 기록됩니다.
* `Virtual 모드로 대체합니다`가 보이면 선택한 ASIO 드라이버를 열지 못한 것입니다. 드라이버가 설치되어 있는지, 다른 프로그램이 장치를 사용 중인지 확인하세요.
* 문제를 제보할 때는 `[Delta_Cast]` 로그를 함께 첨부해 주세요.
* `Drift:` 줄은 1분마다 출력됩니다. `resync` 숫자가 계속 올라가면 송출 장치와의 클럭 차이가 보정 범위를 넘은 것이니 로그와 함께 제보해 주세요.

## 알려진 제한

* 클럭 드리프트 보정은 프록시 모드에서만 동작합니다.
* 가상 모드가 게임에 보고하는 지연값은 실제 지연보다 작습니다.
* 출력 채널은 앞의 2채널(스테레오)만 송출합니다.

## 빌드 방법

**요구 사항**
- Visual Studio 2026 
- Windows 10/11 SDK
- Steinberg ASIO SDK (라이선스 문제로 별도 다운로드)

**폴더 구성**
```
상위폴더\
  ├ ASIOSDK\common\...     ← ASIO SDK
  └ Delta_Cast\            ← 이 저장소
```

**빌드 순서**
1. 이 저장소를 Clone 합니다.
2. ASIO SDK를 저장소와 같은 위치의 `ASIOSDK` 폴더에 풉니다.
3. Visual Studio 2026에서 `Delta_Cast.slnx`를 엽니다.
4. **Release / x64**(64비트) 또는 **Release / Win32**(32비트)로 빌드합니다.

**코드 서명 (선택)**

환경 변수 `DC_SIGN_THUMBPRINT`에 코드 서명 인증서의 지문을 지정하면 빌드 후 자동으로 서명합니다. 지정하지 않으면 서명 단계를 건너뜁니다.

```powershell
[Environment]::SetEnvironmentVariable("DC_SIGN_THUMBPRINT", "<인증서 지문>", "User")
```

## 라이선스

이 프로젝트는 **MIT License**로 배포됩니다. 자유롭게 수정하고 배포할 수 있습니다. 자세한 내용은 [LICENSE](LICENSE) 파일을 참고하세요.

Copyright (c) 2025-2026 **Studio Delta Works**
