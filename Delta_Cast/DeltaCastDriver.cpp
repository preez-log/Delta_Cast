#include "DeltaCastDriver.h"
#include "DeltaCastGuids.h"
#include "timer.h"
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <avrt.h>
#pragma comment(lib, "avrt.lib")

const float INT32_TO_FLOAT = 4.65661287e-10f;  // 1 / 2^31
const float INT24_TO_FLOAT = 1.19209290e-7f;   // 1 / 2^23
const float INT16_TO_FLOAT = 3.05175781e-5f;   // 1 / 2^15

// 디버그 로그 
void DebugLog(const char* fmt, ...) {
#ifdef _DEBUG
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
#endif
}

const IID kIID_IASIO = { 0x5B96C901, 0x7195, 0x11D2, { 0x9C, 0xB1, 0x00, 0x60, 0x08, 0x03, 0x92, 0x2C } };
extern HMODULE g_hModule;

// 전역 포인터 초기화
CDeltaCastDriver* CDeltaCastDriver::g_pThis = nullptr;


// ---------------------------------------------------------------------------
// 가상 백엔드 (Virtual)
// ---------------------------------------------------------------------------
VirtualBackend::VirtualBackend(CDeltaCastDriver* owner, double sampleRate)
    : m_owner(owner), m_sampleRate(sampleRate) {
    DebugLog("[VirtualBackend] Created with %.1f Hz\n", sampleRate);
}

VirtualBackend::~VirtualBackend() {
    Stop();
}

ASIOError VirtualBackend::Start() {
    if (!m_running) {
        m_running = true;
        m_thread = std::thread(&VirtualBackend::VirtualClockLoop, this);
        DebugLog("[VirtualBackend] Thread Started\n");
    }
    return ASE_OK;
}

ASIOError VirtualBackend::Stop() {
    if (m_running) {
        m_running = false;
        if (m_thread.joinable()) {
            m_thread.join();
        }
        DebugLog("[VirtualBackend] Thread Stopped\n");
    }
    return ASE_OK;
}

void VirtualBackend::VirtualClockLoop() {
	// 타이머 해상도, 스레드 우선순위 설정
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristics(L"Pro Audio", &taskIndex);
    if (hTask == NULL) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        DebugLog("[VirtualBackend] MMCSS Failed, using Time Critical fallback\n");
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

    size_t bufferCapacity = Config::RING_BUFFER_SIZE;

    DebugLog("[VirtualBackend] Simple Loop Started. Block Time: %.3f ms\n", idealSeconds * 1000.0);
    DebugLog("[VirtualBackend] Loop Running... Buffer: %d\n", m_bufferSize);

    while (m_running) {
        // 누적 오차 방지
        size_t currentFill = 0;
        if (m_owner) currentFill = m_owner->m_loopbackBufferR.GetFillSize();

        auto currentSleepTime = idealDuration;

        if (currentFill > bufferCapacity * 0.9) {
            // 오버런 임박 (90% 이상 참) -> 속도를 늦춤
            currentSleepTime += std::chrono::microseconds(10);
        }
        else if (currentFill < bufferCapacity * 0.1) {
            // 언더런 임박 (10% 미만 남음) -> 속도를 높임
            currentSleepTime -= std::chrono::microseconds(10);
        }
        wakeUpTime += currentSleepTime;
        
        if (wakeUpTime < std::chrono::steady_clock::now()) {
            wakeUpTime = std::chrono::steady_clock::now();
        }

        PrecisionClock::WaitUntil(wakeUpTime);

        if (m_owner && m_owner->m_bufferInfos) {
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
    g_pThis = this;
    memset(&m_hostCallbacks, 0, sizeof(m_hostCallbacks));
    memset(&m_myCallbacks, 0, sizeof(m_myCallbacks));
    DebugLog("[DeltaCast] Driver Created\n");
}

CDeltaCastDriver::~CDeltaCastDriver() {
    stop();
    m_backendImpl.reset();
    if (g_pThis == this) g_pThis = nullptr;
    DebugLog("[DeltaCast] Driver Destroyed\n");
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
        DebugLog("[DeltaCast] Mode: Virtual\n");
        m_backendImpl = std::make_unique<VirtualBackend>(this, 48000.0);
        m_isVirtualMode = true;
    }
    else if (wcslen(clsidStr) > 0) {
        CLSID targetClsid;
        if (SUCCEEDED(CLSIDFromString(clsidStr, &targetClsid))) {
            DebugLog("[DeltaCast] Mode: Proxy (%ls)\n", clsidStr);

            // 실제 드라이버 로드
            IASIO* realAsio = nullptr;
            if (SUCCEEDED(CoCreateInstance(targetClsid, nullptr, CLSCTX_INPROC_SERVER, targetClsid, (void**)&realAsio))) {
                m_backendImpl = std::make_unique<ProxyBackend>(realAsio);
            }
        }
        m_isVirtualMode = false;
    }

    if (!m_backendImpl) {
        DebugLog("[DeltaCast] Error: No valid backend found. Defaulting to Virtual.\n");
        m_backendImpl = std::make_unique<VirtualBackend>(this, 48000.0);
        m_isVirtualMode = true;
    }
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
    m_hostCallbacks = *callbacks;
    m_bufferInfos = bufferInfos;
    m_numChannels = numChannels;
    m_bufferSize = bufferSize;

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

    // 출력 채널 인덱스 찾기
    if (result == ASE_OK) {
        m_outIndexL = -1;
        m_outIndexR = -1;
        for (long i = 0; i < numChannels; i++) {
            if (bufferInfos[i].isInput == ASIOFalse) {
                if (m_outIndexL == -1) m_outIndexL = i;
                else if (m_outIndexR == -1) { m_outIndexR = i; break; }
            }
        }
        if (m_outIndexL == -1) { m_outIndexL = 0; m_outIndexR = 1; }
        if (m_outIndexR == -1) m_outIndexR = m_outIndexL;

        // 채널 타입 확인
        ASIOChannelInfo info = { 0 };
        info.channel = bufferInfos[m_outIndexL].channelNum;
        info.isInput = ASIOFalse;
        ASIOError infoResult = m_backendImpl->GetChannelInfo(&info);
        if (infoResult == ASE_OK) {
            m_sampleType = info.type;
        }
        else {
			// 기본값으로 Int32 사용
            m_sampleType = ASIOSTInt32LSB;
        }
    }
    return result;
}

ASIOError CDeltaCastDriver::start() {
    if (!m_backendImpl) {
        return ASE_NotPresent;
    }
    size_t threshold = 8192;
    switch (m_latencyMode) {
    case 0: threshold = 16384; break; // 42ms
    case 1: threshold = 8192;  break; // 21ms
    case 2: threshold = 4096;  break; // 10ms
    case 3: threshold = 2048;  break; // 5ms
    default: threshold = 8192; break;
    }
    m_renderer.Start(&m_loopbackBufferL, &m_loopbackBufferR, m_targetWasapiId, m_sampleType, m_sampleRate, threshold);
    return m_backendImpl->Start();
}

ASIOError CDeltaCastDriver::stop() {
    m_renderer.Stop();
    return m_backendImpl ? m_backendImpl->Stop() : ASE_OK;
}

ASIOError CDeltaCastDriver::disposeBuffers() {
    return m_backendImpl ? m_backendImpl->DisposeBuffers() : ASE_OK;
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
    if (g_pThis) g_pThis->TriggerBufferSwitch(index);
}
ASIOTime* CDeltaCastDriver::bufferSwitchTimeInfo(ASIOTime* timeInfo, long index, ASIOBool processNow) {
    ASIOTime* result = nullptr;
    if (g_pThis) {
        if (g_pThis->m_hostCallbacks.bufferSwitchTimeInfo)
            result = g_pThis->m_hostCallbacks.bufferSwitchTimeInfo(timeInfo, index, processNow);
        g_pThis->CopyAudioToRingBuffer(index);
    }
    return result;
}

void CDeltaCastDriver::CopyAudioToRingBuffer(long index) {
    if (m_outIndexL == -1 || m_lastProcessedBufferIndex == index) return;
    m_lastProcessedBufferIndex = index;

    static const size_t frameSize = 4;
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
long CDeltaCastDriver::getDriverVersion() { return 0x010201; }
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
    if (g_pThis && g_pThis->m_hostCallbacks.sampleRateDidChange) g_pThis->m_hostCallbacks.sampleRateDidChange(sRate);
}
long CDeltaCastDriver::asioMessage(long selector, long value, void* message, double* opt) {
    if (g_pThis && g_pThis->m_hostCallbacks.asioMessage) return g_pThis->m_hostCallbacks.asioMessage(selector, value, message, opt);
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