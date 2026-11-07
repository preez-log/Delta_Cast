#include "DeltaCastDriver.h"
#include "DeltaCastGuids.h"
#include "timer.h"
#include "Logging.h"
#include "Version.h"
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <avrt.h>
#pragma comment(lib, "avrt.lib")

const IID kIID_IASIO = { 0x5B96C901, 0x7195, 0x11D2, { 0x9C, 0xB1, 0x00, 0x60, 0x08, 0x03, 0x92, 0x2C } };
extern HMODULE g_hModule;
extern LONG g_cDllRef;

static const char* SampleTypeName(ASIOSampleType t) {
    switch (t) {
    case ASIOSTInt16LSB:   return "Int16";
    case ASIOSTInt24LSB:   return "Int24";
    case ASIOSTInt32LSB:   return "Int32";
    case ASIOSTFloat32LSB: return "Float32";
    case ASIOSTFloat64LSB: return "Float64";
    default:               return "Unknown";
    }
}

// 전역 포인터 초기화
std::atomic<CDeltaCastDriver*> CDeltaCastDriver::g_pThis{ nullptr };

// ---------------------------------------------------------------------------
// 가상 백엔드 (Virtual)
// ---------------------------------------------------------------------------
VirtualBackend::VirtualBackend(CDeltaCastDriver* owner, double sampleRate)
    : m_owner(owner), m_sampleRate(sampleRate) {
    DebugLog("[VirtualBackend] Created with %.1f Hz", sampleRate);
}

VirtualBackend::~VirtualBackend() {
    Stop();
}

ASIOError VirtualBackend::Start() {
    if (!m_running) {
        m_running = true;
        m_thread = std::thread(&VirtualBackend::VirtualClockLoop, this);
        DebugLog("[VirtualBackend] Thread Started");
    }
    return ASE_OK;
}

ASIOError VirtualBackend::Stop() {
    if (m_running) {
        m_running = false;
        if (m_thread.joinable()) {
            m_thread.join();
        }
        DebugLog("[VirtualBackend] Thread Stopped");
    }
    return ASE_OK;
}

void VirtualBackend::VirtualClockLoop() {
    // 타이머 해상도, 스레드 우선순위 설정
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristics(L"Pro Audio", &taskIndex);
    if (hTask == NULL) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        DebugLog("[VirtualBackend] MMCSS Failed, using Time Critical fallback");
    }
    TimerResolutionSetter timerRes;

    // 블록당 시간 계산 (나노초)
    double idealSeconds = (double)m_bufferSize / m_sampleRate;
    auto idealDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::duration<double>(idealSeconds)
    );

    // 기준 시간
    auto wakeUpTime = std::chrono::steady_clock::now();
    long doubleBufferIndex = 0;

    // 목표 버퍼 유지량 0이면 제어 X
    const double target = (double)m_targetFill;

    // 비례 제어: 오차가 목표의 2배일 때 조정 최대
    const double K_P = 10.0;           // us / (상대 오차 1.0)
    const double MAX_ADJUST_US = 10.0; // 블록당 최대 ±10us

    DebugLog("[VirtualBackend] Simple Loop Started. Block Time: %.3f ms", idealSeconds * 1000.0);
    DebugLog("[VirtualBackend] Loop Running... Buffer: %d", m_bufferSize);

    while (m_running) {

        auto currentSleepTime = idealDuration;

        if (m_owner && target > 0.0) {
            double fill = (double)m_owner->m_loopbackBufferR.GetFillSize();
            double error = (fill - target) / target;     // +: 과다, -: 부족

            double adjustUs = K_P * error;
            if (adjustUs > MAX_ADJUST_US) adjustUs = MAX_ADJUST_US;
            if (adjustUs < -MAX_ADJUST_US) adjustUs = -MAX_ADJUST_US;

            // 과다 → 주기 + (느리게) 부족 → 주기 - (빠르게)
            currentSleepTime += std::chrono::nanoseconds((long long)(adjustUs * 1000.0));
        }

        wakeUpTime += currentSleepTime;

        if (wakeUpTime < std::chrono::steady_clock::now()) {
            wakeUpTime = std::chrono::steady_clock::now();
        }

        PrecisionClock::WaitUntil(wakeUpTime);

        if (m_owner && !m_owner->m_bufferInfos.empty()) {
            long numCh = m_owner->m_numChannels;
            // 채널별로 버퍼 클리어
            for (long i = 0; i < numCh; i++) {
                // 버퍼 포인터 가져오기
                void* pBuf = m_owner->m_bufferInfos[i].buffers[doubleBufferIndex];
                if (pBuf) memset(pBuf, 0, m_owner->m_bufferSize * sizeof(float));
            }
            m_owner->TriggerBufferSwitch(doubleBufferIndex);
        }
        // 샘플 위치 갱신
        m_samplePos += m_bufferSize;
        doubleBufferIndex = (doubleBufferIndex + 1) % 2;
    }
    if (hTask) AvRevertMmThreadCharacteristics(hTask);
}

ASIOError VirtualBackend::CreateBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) {
    m_bufferSize = bufferSize;

    try {
        m_buffers.resize(numChannels);
        for (long i = 0; i < numChannels; i++) {
            // 초기화
            m_buffers[i].assign(bufferSize * 2, 0.0f);

            // ASIO 더블 버퍼링 포인터 연결
            bufferInfos[i].buffers[0] = m_buffers[i].data();
            bufferInfos[i].buffers[1] = m_buffers[i].data() + bufferSize;
        }
        return ASE_OK;
    }
    catch (...) {
        return ASE_NoMemory;
    }
}

ASIOError VirtualBackend::DisposeBuffers() {
    m_buffers.clear();
    return ASE_OK;
}

ASIOError VirtualBackend::GetSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) {
    // 현재 샘플 위치
    int64_t pos = m_samplePos.load();
    sPos->lo = (unsigned long)(pos & 0xFFFFFFFF);
    sPos->hi = (unsigned long)(pos >> 32);

    // 시간 (나노초)
    auto now = std::chrono::high_resolution_clock::now();
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    tStamp->lo = (unsigned long)(nanos & 0xFFFFFFFF);
    tStamp->hi = (unsigned long)(nanos >> 32);

    return ASE_OK;
}

// ---------------------------------------------------------------------------
// 드라이버 본체
// ---------------------------------------------------------------------------
CDeltaCastDriver::CDeltaCastDriver() {
    InterlockedIncrement(&g_cDllRef);
    memset(&m_hostCallbacks, 0, sizeof(m_hostCallbacks));
    memset(&m_myCallbacks, 0, sizeof(m_myCallbacks));
    DebugLog("Driver Created");
}

CDeltaCastDriver::~CDeltaCastDriver() {
    stop();
    m_backendImpl.reset();
    ReleaseActiveStream();    // dispose 없이 해제되는 경우
    DebugLog("Driver Destroyed");
    InterlockedDecrement(&g_cDllRef);
}

void CDeltaCastDriver::ReleaseActiveStream() {
    CDeltaCastDriver* expected = this;
    g_pThis.compare_exchange_strong(expected, nullptr);
}

// ---------------------------------------------------------------------------
// 초기화
// ---------------------------------------------------------------------------
void CDeltaCastDriver::LoadConfiguration() {
    WCHAR modulePath[MAX_PATH];
    if (GetModuleFileNameW(g_hModule, modulePath, MAX_PATH) == 0) return;

    std::wstring configPath = modulePath;
    size_t lastSlash = configPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) configPath = configPath.substr(0, lastSlash + 1);
    configPath += L"Delta_Cast.ini";

    // INI 에서 읽어옴
    WCHAR clsidStr[64] = { 0 };
    GetPrivateProfileStringW(L"Settings", L"TargetDriverCLSID", L"", clsidStr, 64, configPath.c_str());

    WCHAR wasapiIdBuf[256] = { 0 };
    GetPrivateProfileStringW(L"Settings", L"TargetWasapiID", L"", wasapiIdBuf, 256, configPath.c_str());
    m_latencyMode = GetPrivateProfileIntW(L"Settings", L"LatencyMode", 1, configPath.c_str());
    m_targetWasapiId = wasapiIdBuf;

    // 모드 선택
    if (wcscmp(clsidStr, L"Virtual") == 0) {
        DebugLog("Mode: Virtual");
        m_backendImpl = std::make_unique<VirtualBackend>(this, 48000.0);
        m_isVirtualMode = true;
    }
    else if (wcslen(clsidStr) > 0) {
        CLSID targetClsid;
        HRESULT hrClsid = CLSIDFromString(clsidStr, &targetClsid);
        if (SUCCEEDED(hrClsid)) {
            DebugLog("Mode: Proxy (%ls)", clsidStr);

            // 실제 드라이버 로드
            IASIO* realAsio = nullptr;
            HRESULT hrCreate = CoCreateInstance(targetClsid, nullptr, CLSCTX_INPROC_SERVER,targetClsid, (void**)&realAsio);

            if (SUCCEEDED(hrCreate)) {
                m_backendImpl = std::make_unique<ProxyBackend>(realAsio);
            }
            else {
                LogError("드라이버 생성 실패 (CLSID: %ls, HR: 0x%08X)", clsidStr, hrCreate);
            }
        }
        else {
            LogError("잘못된 CLSID 형식 (%ls, HR: 0x%08X)", clsidStr, hrClsid);
        }
        m_isVirtualMode = false;
    }

    if (!m_backendImpl) {
        LogError("Virtual 모드로 대체합니다.");
        m_backendImpl = std::make_unique<VirtualBackend>(this, 48000.0);
        m_isVirtualMode = true;
    }

    LogInfo("Mode: %s%s%ls | LatencyMode: %d",
        m_isVirtualMode ? "Virtual" : "Proxy",
        m_isVirtualMode ? "" : " -> ",
        m_isVirtualMode ? L"" : clsidStr,
        m_latencyMode);
}

ASIOBool CDeltaCastDriver::init(void* sysHandle) {
    LoadConfiguration();
    if (!m_backendImpl) return ASIOFalse;
    return (m_backendImpl->Init(sysHandle) == ASE_OK) ? ASIOTrue : ASIOFalse;
}

// ---------------------------------------------------------------------------
// 오디오 제어
// ---------------------------------------------------------------------------
ASIOError CDeltaCastDriver::createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) {

    if (!m_backendImpl) return ASE_NotPresent;
    if (!bufferInfos || !callbacks || numChannels <= 0 || bufferSize <= 0)
        return ASE_InvalidParameter;

    // 활성 스트림 등록 (프로세스당 1개)
    CDeltaCastDriver* expected = nullptr;
    const bool newlyRegistered = g_pThis.compare_exchange_strong(expected, this);
    if (!newlyRegistered && expected != this) {
        LogError("다른 인스턴스가 이미 스트림을 사용 중입니다.");
        return ASE_InvalidMode;
    }

    m_hostCallbacks = *callbacks;
    m_numChannels = numChannels;
    m_bufferSize = bufferSize;

    // 버퍼 인덱스 초기화
    m_lastProcessedBufferIndex = -1;

    // 샘플 레이트 세팅
    ASIOSampleRate rate = 48000.0;
    m_backendImpl->GetSampleRate(&rate);
    m_sampleRate = rate;

    // 복제 콜백 연결
    m_myCallbacks.bufferSwitch = &CDeltaCastDriver::bufferSwitch;
    m_myCallbacks.bufferSwitchTimeInfo = &CDeltaCastDriver::bufferSwitchTimeInfo;
    m_myCallbacks.sampleRateDidChange = &CDeltaCastDriver::sampleRateChanged;
    m_myCallbacks.asioMessage = &CDeltaCastDriver::asioMessage;

    // 백엔드에서 버퍼 생성
    ASIOError result = m_backendImpl->CreateBuffers(bufferInfos, numChannels, bufferSize, &m_myCallbacks);
    if (result != ASE_OK) {
        memset(&m_hostCallbacks, 0, sizeof(m_hostCallbacks));
        m_numChannels = 0;
        if (newlyRegistered) ReleaseActiveStream();
        return result;
    }

    // bufferInfo 복사본 보관
    try {
        m_bufferInfos.assign(bufferInfos, bufferInfos + numChannels);
    }
    catch (...) {
        m_backendImpl->DisposeBuffers();
        memset(&m_hostCallbacks, 0, sizeof(m_hostCallbacks));
        m_numChannels = 0;
        if (newlyRegistered) ReleaseActiveStream();
        return ASE_NoMemory;
    }

    // 출력 채널 인덱스 찾기
    m_outIndexL = -1;
    m_outIndexR = -1;
    for (long i = 0; i < numChannels; i++) {
        if (m_bufferInfos[i].isInput == ASIOFalse) {
            if (m_outIndexL == -1) m_outIndexL = i;
            else if (m_outIndexR == -1) { m_outIndexR = i; break; }
        }
    }
    if (m_outIndexL == -1) { m_outIndexL = 0; m_outIndexR = (numChannels > 1) ? 1 : 0; }
    if (m_outIndexR == -1) m_outIndexR = m_outIndexL;

    // 채널 타입 확인
    ASIOChannelInfo info = { 0 };
    info.channel = m_bufferInfos[m_outIndexL].channelNum;
    info.isInput = ASIOFalse;
    m_sampleType = (m_backendImpl->GetChannelInfo(&info) == ASE_OK) ? info.type : ASIOSTInt32LSB;

    LogInfo("Stream: %.0f Hz | %s | buffer %ld frames (%.2f ms) | out idx %ld/%ld (ch %ld/%ld)",
        m_sampleRate, SampleTypeName(m_sampleType),
        m_bufferSize, m_bufferSize * 1000.0 / m_sampleRate,
        m_outIndexL, m_outIndexR,
        m_bufferInfos[m_outIndexL].channelNum, m_bufferInfos[m_outIndexR].channelNum);
    return ASE_OK;
}

size_t CDeltaCastDriver::GetLoopbackThreshold() const {
    switch (m_latencyMode) {
    case 0:  return 16384;
    case 1:  return 8192;
    case 2:  return 4096;
    case 3:  return 2048;
    case 4:  return 1024;
    default: return 8192;
    }
}

ASIOError CDeltaCastDriver::start() {
    if (!m_backendImpl) {
        return ASE_NotPresent;
    }
    const size_t threshold = GetLoopbackThreshold();

    // 가상 클럭 렌더러 버퍼 임계값을 기준으로 버퍼 채움 유지
    m_backendImpl->SetLoopbackTarget(threshold);
    const int sampleBytes = GetSampleSize(m_sampleType);

    // [임시] 가상 클럭 목표 하한: ASIO 블록 + WASAPI 공유 모드 기본 주기(10ms) 분량
    // 가상 엔진을 단일 클럭 구조로 교체할 때 함께 제거
    size_t target = threshold;
    if (sampleBytes > 0) {
        const size_t blockBytes = (size_t)m_bufferSize * sampleBytes;
        const size_t periodGuard = (size_t)(m_sampleRate * 0.010) * sampleBytes;
        target = (std::max)(threshold, blockBytes + periodGuard);
    }
    m_backendImpl->SetLoopbackTarget(target);

    const double thresholdMs = (sampleBytes > 0)
        ? (double)threshold / sampleBytes * 1000.0 / m_sampleRate : 0.0;
    const double targetMs = (sampleBytes > 0)
        ? (double)target / sampleBytes * 1000.0 / m_sampleRate : 0.0;

    if (m_isVirtualMode) {
        LogInfo("Loopback prebuffer: %zu bytes (%.1f ms) | virtual clock target: %zu bytes (%.1f ms)",
            threshold, thresholdMs, target, targetMs);
    }
    else {
        LogInfo("Loopback prebuffer: %zu bytes (%.1f ms)", threshold, thresholdMs);
    }

    m_renderer.Start(&m_loopbackBufferL, &m_loopbackBufferR, m_targetWasapiId, m_sampleType, m_sampleRate, threshold);
    return m_backendImpl->Start();
}

ASIOError CDeltaCastDriver::stop() {
    m_renderer.Stop();
    return m_backendImpl ? m_backendImpl->Stop() : ASE_OK;
}

ASIOError CDeltaCastDriver::disposeBuffers() {
    if (!m_backendImpl) return ASE_OK;

    // 호스트가 stop() 없이 dispose를 호출시 렌더러 -> 백엔드 종료
    m_renderer.Stop();
    m_backendImpl->Stop();

    ASIOError err = m_backendImpl->DisposeBuffers();

    m_bufferInfos.clear();
    m_numChannels = 0;
    m_outIndexL = -1;
    m_outIndexR = -1;
    m_lastProcessedBufferIndex = -1;

    // 링버퍼 초기화
    m_loopbackBufferL.Reset();
    m_loopbackBufferR.Reset();

    // 스트림 해제
    memset(&m_hostCallbacks, 0, sizeof(m_hostCallbacks));
    ReleaseActiveStream();
    return err;
}
// ---------------------------------------------------------------------------
// 오디오 처리
// ---------------------------------------------------------------------------
void CDeltaCastDriver::TriggerBufferSwitch(long index) {
    // 호스트 콜백
    if (m_hostCallbacks.bufferSwitch) {
        m_hostCallbacks.bufferSwitch(index, ASIOFalse);
    }
    // 오디오 복제 및 변환
    CopyAudioToRingBuffer(index);
}

// 정적 래퍼들
void CDeltaCastDriver::bufferSwitch(long index, ASIOBool directProcess) {
    if (auto* p = g_pThis.load()) p->TriggerBufferSwitch(index);
}
ASIOTime* CDeltaCastDriver::bufferSwitchTimeInfo(ASIOTime* timeInfo, long index, ASIOBool processNow) {
    ASIOTime* result = nullptr;
    if (auto* p = g_pThis.load()) {
        if (p->m_hostCallbacks.bufferSwitchTimeInfo)
            result = p->m_hostCallbacks.bufferSwitchTimeInfo(timeInfo, index, processNow);
        p->CopyAudioToRingBuffer(index);
    }
    return result;
}

void CDeltaCastDriver::CopyAudioToRingBuffer(long index) {
    if (m_bufferInfos.empty() || m_outIndexL == -1 || m_lastProcessedBufferIndex == index) return;
    m_lastProcessedBufferIndex = index;

    const size_t frameSize = GetSampleSize(m_sampleType);
    if (frameSize == 0) return; // 알 수 없는 샘플 포맷 보호
    size_t bytesToCopy = m_bufferSize * frameSize;

    // 링버퍼 여유 공간 확인
    size_t available = m_loopbackBufferL.GetAvailableWrite();
    if (available < bytesToCopy) {
        // 오버런
        return;
    }

    // 원본 데이터 포인터 획득
    void* pRawL = m_bufferInfos[m_outIndexL].buffers[index];
    void* pRawR = (m_outIndexR != -1) ? m_bufferInfos[m_outIndexR].buffers[index] : nullptr;

    // 링버퍼 (-> WASAPI)
    m_loopbackBufferL.Push(pRawL, bytesToCopy);
    if (pRawR) { m_loopbackBufferR.Push(pRawR, bytesToCopy); }
    else { m_loopbackBufferR.Push(pRawL, bytesToCopy); }
}

ASIOError CDeltaCastDriver::getBufferSize(long* min, long* max, long* pref, long* gran) {
    return m_backendImpl ? m_backendImpl->GetBufferSize(min, max, pref, gran) : ASE_NotPresent;
}
ASIOError CDeltaCastDriver::getSampleRate(ASIOSampleRate* rate) {
    return m_backendImpl ? m_backendImpl->GetSampleRate(rate) : ASE_NotPresent;
}
ASIOError CDeltaCastDriver::setSampleRate(ASIOSampleRate rate) {
    ASIOError err = m_backendImpl ? m_backendImpl->SetSampleRate(rate) : ASE_NotPresent;
    if (err == ASE_OK) m_sampleRate = rate;
    return err;
}
ASIOError CDeltaCastDriver::getChannels(long* in, long* out) {
    return m_backendImpl ? m_backendImpl->GetChannels(in, out) : ASE_NotPresent;
}
ASIOError CDeltaCastDriver::getChannelInfo(ASIOChannelInfo* info) {
    return m_backendImpl ? m_backendImpl->GetChannelInfo(info) : ASE_NotPresent;
}
ASIOError CDeltaCastDriver::getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) {
    return m_backendImpl ? m_backendImpl->GetSamplePosition(sPos, tStamp) : ASE_NotPresent;
}
ASIOError CDeltaCastDriver::outputReady() {
    return m_backendImpl ? m_backendImpl->OutputReady() : ASE_NotPresent;
}
ASIOError CDeltaCastDriver::controlPanel() {
    return m_backendImpl ? m_backendImpl->ControlPanel() : ASE_NotPresent;
}
ASIOError CDeltaCastDriver::setClockSource(long reference) {
    return m_backendImpl ? m_backendImpl->SetClockSource(reference) : ASE_NotPresent;
}
ASIOError CDeltaCastDriver::getClockSources(ASIOClockSource* clocks, long* numSources) {
    return m_backendImpl ? m_backendImpl->GetClockSources(clocks, numSources) : ASE_NotPresent;
}
// 기타 함수
void CDeltaCastDriver::getDriverName(char* name) { strcpy_s(name, 32, "Delta_Cast ASIO"); }
long CDeltaCastDriver::getDriverVersion() { return DC_VER_HEX; }
void CDeltaCastDriver::getErrorMessage(char* string) {
    if (m_backendImpl) m_backendImpl->GetErrorMessage(string);
    else strcpy_s(string, 32, "No Backend");
}
ASIOError CDeltaCastDriver::getLatencies(long* inputLatency, long* outputLatency) {
    return m_backendImpl ? m_backendImpl->GetLatencies(inputLatency, outputLatency) : ASE_NotPresent;
}

ASIOError CDeltaCastDriver::canSampleRate(ASIOSampleRate sampleRate) {
    return m_backendImpl ? m_backendImpl->CanSampleRate(sampleRate) : ASE_NotPresent;
}

ASIOError CDeltaCastDriver::future(long selector, void* opt) {
    return m_backendImpl ? m_backendImpl->Future(selector, opt) : ASE_NotPresent;
}

// ---------------------------------------------------------------------------
// COM 구현
// ---------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE CDeltaCastDriver::QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == kIID_IASIO || riid == CLSID_Delta_Cast) {
        *ppv = static_cast<IASIO*>(this); AddRef(); return S_OK;
    }
    *ppv = nullptr; return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE CDeltaCastDriver::AddRef() { return ++m_refCount; }
ULONG STDMETHODCALLTYPE CDeltaCastDriver::Release() {
    ULONG ref = --m_refCount;
    if (ref == 0) delete this;
    return ref;
}


// 정적 콜백들
void CDeltaCastDriver::sampleRateChanged(ASIOSampleRate sRate) {
    if (auto* p = g_pThis.load(); p && p->m_hostCallbacks.sampleRateDidChange) p->m_hostCallbacks.sampleRateDidChange(sRate);
}
long CDeltaCastDriver::asioMessage(long selector, long value, void* message, double* opt) {
    if (auto* p = g_pThis.load(); p && p->m_hostCallbacks.asioMessage) return p->m_hostCallbacks.asioMessage(selector, value, message, opt);
    return 0;
}

// 샘플 크기 반환
int CDeltaCastDriver::GetSampleSize(ASIOSampleType type) {
    switch (type) {
    case ASIOSTInt32LSB:   return 4;
    case ASIOSTFloat32LSB: return 4;
    case ASIOSTInt24LSB:   return 3;
    case ASIOSTInt16LSB:   return 2;
    case ASIOSTFloat64LSB: return 8;
    default: return 0;
    }
}