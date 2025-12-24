#include "DeltaCastDriver.h"
#include "DeltaCastGuids.h"
#include "timer.h"
#include <windows.h>
#include <stdio.h>
#include <string>
#include <vector>

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
    // 스레드 우선순위
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    // 블록당 시간 계산 (나노초)
    double secondsPerBlock = (double)m_bufferSize / m_sampleRate;
    auto blockDuration = std::chrono::duration_cast<PrecisionClock::Duration>(
        std::chrono::duration<double>(secondsPerBlock)
    );

    // 기준 시간
    auto nextTriggerTime = PrecisionClock::Now();
    m_samplePos = 0;
    long doubleBufferIndex = 0;

    DebugLog("[VirtualBackend] Loop Running... Buffer: %d\n", m_bufferSize);

    while (m_running) {
        // 누적 오차 방지
        nextTriggerTime += blockDuration;

        // 버퍼 스위치
        if (m_owner) {
            m_owner->TriggerBufferSwitch(doubleBufferIndex);
        }

        // 샘플 위치 갱신
        m_samplePos += m_bufferSize;
        doubleBufferIndex = (doubleBufferIndex + 1) % 2;

        // 정밀 대기
        PrecisionClock::WaitUntil(nextTriggerTime);
    }
}

ASIOError VirtualBackend::CreateBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) {
    m_bufferSize = bufferSize;

    try {
        m_buffers.resize(numChannels);
        for (long i = 0; i < numChannels; i++) {
            // 초기화
            m_buffers[i].assign(bufferSize, 0.0f);

            // ASIO 더블 버퍼링 포인터 연결
            bufferInfos[i].buffers[0] = m_buffers[i].data();
            bufferInfos[i].buffers[1] = m_buffers[i].data();
        }
        DebugLog("[VirtualBackend] Buffers Allocated: %d ch, %d frames\n", numChannels, bufferSize);
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
    configPath += L"DeltaCast.ini";

    // INI 에서 읽어옴
    WCHAR clsidStr[64] = { 0 };
    GetPrivateProfileStringW(L"Settings", L"TargetDriverCLSID", L"", clsidStr, 64, configPath.c_str());

    WCHAR wasapiIdBuf[256] = { 0 };
    GetPrivateProfileStringW(L"Settings", L"TargetWasapiID", L"", wasapiIdBuf, 256, configPath.c_str());
    m_targetWasapiId = wasapiIdBuf;

    // 모드 선택
    if (wcscmp(clsidStr, L"Virtual") == 0) {
        DebugLog("[DeltaCast] Mode: Virtual\n");
        m_backendImpl = std::make_unique<VirtualBackend>(this, 44100.0);
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
    }

    if (!m_backendImpl) {
        DebugLog("[DeltaCast] Error: No valid backend found. Defaulting to Virtual.\n");
        m_backendImpl = std::make_unique<VirtualBackend>(this, 44100.0);
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

    // 리샘플러 초기화
    size_t maxResampledSize = (size_t)(bufferSize * (48000.0 / 44100.0) + 32);
    m_convertBufferL.resize(bufferSize);
    m_convertBufferR.resize(bufferSize);
    m_resampledDataL.resize(maxResampledSize);
    m_resampledDataR.resize(maxResampledSize);

    // 샘플 레이트 세팅
    ASIOSampleRate rate = 44100.0;
    m_backendImpl->GetSampleRate(&rate);
    m_sampleRate = rate;

    m_resamplerL.Setup(m_sampleRate, 48000.0);
    m_resamplerR.Setup(m_sampleRate, 48000.0);

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

        // 채널 타입 확인
        ASIOChannelInfo info = { 0 };
        info.channel = m_outIndexL;
        info.isInput = ASIOFalse;
        if (m_backendImpl->GetChannelInfo(&info) == ASE_OK) {
            m_sampleType = info.type;
        }
    }
    return result;
}

ASIOError CDeltaCastDriver::start() {
    if (!m_backendImpl) return ASE_NotPresent;
    m_renderer.Start(&m_loopbackBufferL, &m_loopbackBufferR, m_targetWasapiId);
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
    if (g_pThis) {
        if (g_pThis->m_hostCallbacks.bufferSwitchTimeInfo)
            return g_pThis->m_hostCallbacks.bufferSwitchTimeInfo(timeInfo, index, processNow);
        g_pThis->CopyAudioToRingBuffer(index);
    }
    return nullptr;
}

void CDeltaCastDriver::CopyAudioToRingBuffer(long index) {
    if (!m_bufferInfos || m_outIndexL == -1) return;
    if (m_lastProcessedBufferIndex == index) return;
    m_lastProcessedBufferIndex = index;

    // 원본 데이터 포인터 획득
    void* pRawL = m_bufferInfos[m_outIndexL].buffers[index];
    void* pRawR = (m_outIndexR != -1) ? m_bufferInfos[m_outIndexR].buffers[index] : nullptr;
    if (!pRawL) return;

    // 포맷 변환 (-> Float32)
    float* pDestL = m_convertBufferL.data();
    float* pDestR = m_convertBufferR.data();

    // 예시: Float32인 경우
    if (m_sampleType == ASIOSTFloat32LSB) {
        memcpy(pDestL, pRawL, m_bufferSize * sizeof(float));
        if (pRawR) memcpy(pDestR, pRawR, m_bufferSize * sizeof(float));
        else memcpy(pDestR, pDestL, m_bufferSize * sizeof(float));
    }
    switch (m_sampleType) {

    // 32비트 정수 (Int32)
    case ASIOSTInt32LSB: {
        int32_t* srcL = (int32_t*)pRawL;
        int32_t* srcR = (int32_t*)pRawR;
        for (long i = 0; i < m_bufferSize; i++) {
            pDestL[i] = (float)srcL[i] * INT32_TO_FLOAT;
            pDestR[i] = srcR ? ((float)srcR[i] * INT32_TO_FLOAT) : pDestL[i];
        }
        break;
    }

    // 32비트 부동소수점 (Float32)
    case ASIOSTFloat32LSB: {
        float* srcL = (float*)pRawL;
        float* srcR = (float*)pRawR;

        memcpy(pDestL, srcL, m_bufferSize * sizeof(float));

        if (srcR) {
            memcpy(pDestR, srcR, m_bufferSize * sizeof(float));
        }
        else {
            // Mono Source -> Stereo Copy
            memcpy(pDestR, srcL, m_bufferSize * sizeof(float));
        }
        break;
    }

    // 24비트 정수 (Int24 Packed)
    case ASIOSTInt24LSB: {
        uint8_t* srcL = (uint8_t*)pRawL;
        uint8_t* srcR = (uint8_t*)pRawR;
        for (long i = 0; i < m_bufferSize; i++) {

            // Left Channel
            int32_t sampleL = (int32_t)((srcL[i * 3 + 2] << 24) | (srcL[i * 3 + 1] << 16) | (srcL[i * 3] << 8));
            pDestL[i] = (float)(sampleL >> 8) * INT24_TO_FLOAT;

            // Right Channel
            if (srcR) {
                int32_t sampleR = (int32_t)((srcR[i * 3 + 2] << 24) | (srcR[i * 3 + 1] << 16) | (srcR[i * 3] << 8));
                pDestR[i] = (float)(sampleR >> 8) * INT24_TO_FLOAT;
            }
            else {
                pDestR[i] = pDestL[i];
            }
        }
        break;
    }
    // 16비트 정수 (Int16)
    case ASIOSTInt16LSB: {
        int16_t* srcL = (int16_t*)pRawL;
        int16_t* srcR = (int16_t*)pRawR;
        for (long i = 0; i < m_bufferSize; i++) {
            pDestL[i] = (float)srcL[i] * INT16_TO_FLOAT;
            pDestR[i] = srcR ? ((float)srcR[i] * INT16_TO_FLOAT) : pDestL[i];
        }
        break;
    }

    // 64비트 부동소수점 (Double)
    case ASIOSTFloat64LSB: {
        double* srcL = (double*)pRawL;
        double* srcR = (double*)pRawR;
        for (long i = 0; i < m_bufferSize; i++) {
            pDestL[i] = (float)srcL[i]; // downcast
            pDestR[i] = srcR ? (float)srcR[i] : pDestL[i];
        }
        break;
    }
    // 그 외 지원하지 않는 포맷 (Silence)
    default:
        memset(pDestL, 0, m_bufferSize * sizeof(float));
        memset(pDestR, 0, m_bufferSize * sizeof(float));
        break;
    }

    // 리샘플링 (-> 48000Hz)
    size_t outL = m_resamplerL.Process(pDestL, m_bufferSize, m_resampledDataL.data(), m_resampledDataL.size());
    size_t outR = m_resamplerR.Process(pDestR, m_bufferSize, m_resampledDataR.data(), m_resampledDataR.size());

    // 링버퍼 (-> WASAPI)
    if (outL > 0) m_loopbackBufferL.Push(m_resampledDataL.data(), outL);
    if (outR > 0) m_loopbackBufferR.Push(m_resampledDataR.data(), outR);
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
long CDeltaCastDriver::getDriverVersion() { return 0x010100; }
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