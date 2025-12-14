#include "DeltaCastDriver.h"
#include "DeltaCastGuids.h"
#include <windows.h>
#include <stdio.h>
#include <string>

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
void LogGUID(const char* prefix, REFIID riid) {
#ifdef _DEBUG
    LPOLESTR guidStr = nullptr;
    if (SUCCEEDED(StringFromCLSID(riid, &guidStr))) {
        DebugLog("%s %ls\n", prefix, guidStr);
        CoTaskMemFree(guidStr);
    }
    else {
        DebugLog("%s (Failed to convert GUID)\n", prefix);
    }
#endif
}

const IID kIID_IASIO = { 0x5B96C901, 0x7195, 0x11D2, { 0x9C, 0xB1, 0x00, 0x60, 0x08, 0x03, 0x92, 0x2C } };
extern HMODULE g_hModule;

// 전역 포인터 초기화
CDeltaCastDriver* CDeltaCastDriver::g_pThis = nullptr;

CDeltaCastDriver::CDeltaCastDriver() { 
    g_pThis = this; // 전역 포인터 연결
    memset(&m_hostCallbacks, 0, sizeof(m_hostCallbacks));
    memset(&m_myCallbacks, 0, sizeof(m_myCallbacks));
    LoadConfiguration();
    DebugLog("[DeltaCast] Created\n"); 
}
CDeltaCastDriver::~CDeltaCastDriver() { 
    if (m_backend) {
        m_backend->Release();
        m_backend = nullptr;
    }
    if (g_pThis == this) g_pThis = nullptr;
    DebugLog("[DeltaCast] Destroyed\n");
}

ASIOError CDeltaCastDriver::createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) {
    DebugLog("[DeltaCast] createBuffers called. Size: %d\n", bufferSize);

    if (!m_backend) return ASE_NotPresent;

    // 호스트 콜백
    m_hostCallbacks = *callbacks;
    m_bufferInfos = bufferInfos;
    m_bufferSize = bufferSize;
    m_numChannels = numChannels;

    size_t maxResampledSize = (size_t)(bufferSize * (48000.0 / 44100.0) + 32);

    m_convertBufferL.resize(bufferSize);
    m_convertBufferR.resize(bufferSize);
    m_resampledDataL.resize(maxResampledSize);
    m_resampledDataR.resize(maxResampledSize);
    m_resamplerL.Setup(m_sampleRate, 48000.0);
    m_resamplerR.Setup(m_sampleRate, 48000.0);

    // 복제 콜백 함수들 연결
    m_myCallbacks.bufferSwitch = &CDeltaCastDriver::bufferSwitch;
    m_myCallbacks.bufferSwitchTimeInfo = &CDeltaCastDriver::bufferSwitchTimeInfo;
    m_myCallbacks.sampleRateDidChange = &CDeltaCastDriver::sampleRateChanged;
    m_myCallbacks.asioMessage = &CDeltaCastDriver::asioMessage;

    // bufferInfo 기준 하드웨어가 직접 메모리를 할당
    ASIOError result = m_backend->createBuffers(bufferInfos, numChannels, bufferSize, &m_myCallbacks);

    if (result == ASE_OK) {
        m_outIndexL = -1;
        m_outIndexR = -1;
        for (long i = 0; i < numChannels; i++) {
            // isInput이 False이면 출력 채널
            if (m_bufferInfos[i].isInput == ASIOFalse) {
                if (m_outIndexL == -1) {
                    m_outIndexL = i;
                    ASIOChannelInfo info = { 0 };
                    info.channel = m_bufferInfos[i].channelNum;
                    info.isInput = ASIOFalse;

                    if (m_backend->getChannelInfo(&info) == ASE_OK) {
                        m_sampleType = info.type;
                        DebugLog("Found Output L at index: %d (Type: %d)\n", i, m_sampleType);
                    }
                }
                else if (m_outIndexR == -1) {
                    m_outIndexR = i;
                    DebugLog("Found Output R at index: %d\n", i);
                    break;
                }
            }
        }
        DebugLog("[DeltaCast] Buffers created & Hook installed successfully!\n");
    }
    else {
        DebugLog("[DeltaCast] createBuffers Failed!\n");
    }

    return result;
}

void CDeltaCastDriver::CopyAudioToRingBuffer(long index) {
    if (!g_pThis || !g_pThis->m_bufferInfos) return;
    if (g_pThis->m_outIndexL == -1) return;
    if (g_pThis->m_lastProcessedBufferIndex == index) return;
    g_pThis->m_lastProcessedBufferIndex = index;

    // createBuffers 에서 받은 크기
    long blockSize = g_pThis->m_bufferSize;

    // 원본 오디오 데이터 포인터
    void* pRawL = g_pThis->m_bufferInfos[g_pThis->m_outIndexL].buffers[index];
    void* pRawR = (g_pThis->m_outIndexR != -1) ?
        g_pThis->m_bufferInfos[g_pThis->m_outIndexR].buffers[index] : nullptr;

    if (!pRawL) return;

    // 잡아둔 포인터 사용
    float* pDestL = g_pThis->m_convertBufferL.data();
    float* pDestR = g_pThis->m_convertBufferR.data();

    ASIOSampleType type = g_pThis->m_sampleType;

    switch (type) {

    // 32비트 정수 (Int32)
    case ASIOSTInt32LSB: {
        int32_t* srcL = (int32_t*)pRawL;
        int32_t* srcR = (int32_t*)pRawR;
        for (long i = 0; i < blockSize; i++) {
            pDestL[i] = (float)srcL[i] * INT32_TO_FLOAT;
            pDestR[i] = srcR ? ((float)srcR[i] * INT32_TO_FLOAT) : pDestL[i];
        }
        break;
    }

    // 32비트 부동소수점 (Float32)
    case ASIOSTFloat32LSB: {
        float* srcL = (float*)pRawL;
        float* srcR = (float*)pRawR;
        memcpy(pDestL, srcL, blockSize * sizeof(float));
        if (srcR) memcpy(pDestR, srcR, blockSize * sizeof(float));
        else memcpy(pDestR, srcL, blockSize * sizeof(float));
        break;
    }

    // 24비트 정수 (Int24 Packed)
    case ASIOSTInt24LSB: {
        uint8_t* srcL = (uint8_t*)pRawL;
        uint8_t* srcR = (uint8_t*)pRawR;
        for (long i = 0; i < blockSize; i++) {

            // Left 
            int32_t sampleL = (int32_t)((srcL[i * 3 + 2] << 24) | (srcL[i * 3 + 1] << 16) | (srcL[i * 3] << 8));
            // 부호 확장
            pDestL[i] = (float)(sampleL >> 8) * INT24_TO_FLOAT;

            // Right
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
        for (long i = 0; i < blockSize; i++) {
            pDestL[i] = (float)srcL[i] * INT16_TO_FLOAT;
            pDestR[i] = srcR ? ((float)srcR[i] * INT16_TO_FLOAT) : pDestL[i];
        }
        break;
    }

    // 64비트 부동소수점 (Double)
    case ASIOSTFloat64LSB: {
        double* srcL = (double*)pRawL;
        double* srcR = (double*)pRawR;
        for (long i = 0; i < blockSize; i++) {
            pDestL[i] = (float)srcL[i];
            pDestR[i] = srcR ? (float)srcR[i] : pDestL[i];
        }
        break;
    }

    // 그 외 포맷
    default:
        // 지원하지 않는 포맷은 0(침묵)
        memset(pDestL, 0, blockSize * sizeof(float));
        memset(pDestR, 0, blockSize * sizeof(float));
        break;
    }

    // 왼쪽 채널 처리
   size_t outSamplesL = g_pThis->m_resamplerL.Process(
        g_pThis->m_convertBufferL.data(), // 변환된 데이터
        blockSize,                        // 데이터 개수
        g_pThis->m_resampledDataL.data(), // 결과가 담길 곳
        g_pThis->m_resampledDataL.size()  
   );
   // 오른쪽 채널 처리
   size_t outSamplesR = g_pThis->m_resamplerR.Process(
        g_pThis->m_convertBufferR.data(),
        blockSize, 
        g_pThis->m_resampledDataR.data(),
        g_pThis->m_resampledDataR.size()
   );

    // 링버퍼에 Push
   if (outSamplesL > 0) {
       g_pThis->m_loopbackBufferL.Push(
           g_pThis->m_resampledDataL.data(),
           outSamplesL
       );
   }
   if (outSamplesR > 0) {
       g_pThis->m_loopbackBufferR.Push(
           g_pThis->m_resampledDataR.data(),
           outSamplesR
       );
   }
}

ASIOTime* CDeltaCastDriver::bufferSwitchTimeInfo(ASIOTime* timeInfo, long index, ASIOBool processNow) {
    // 이 함수 내에서는 절대 DebugLog, printf, new, lock 사용 금지
    // 소리는 그대로 지나감
    ASIOTime* result = nullptr;
    if (g_pThis && g_pThis->m_hostCallbacks.bufferSwitchTimeInfo) {
        result = g_pThis->m_hostCallbacks.bufferSwitchTimeInfo(timeInfo, index, processNow);
    }

    // 오디오 데이터 복제
    CopyAudioToRingBuffer(index);

    return result;
}

void CDeltaCastDriver::bufferSwitch(long index, ASIOBool directProcess) {
    if (g_pThis && g_pThis->m_hostCallbacks.bufferSwitch) {
        g_pThis->m_hostCallbacks.bufferSwitch(index, directProcess);
    }

    CopyAudioToRingBuffer(index);
}

bool CDeltaCastDriver::LoadBackendDriver() {
    if (m_backend) return true;
    if (!m_hasConfig) {
        DebugLog("[DeltaCast] Error: Configuration not loaded or invalid.\n");
        return false;
    }
    HRESULT hr = CoCreateInstance(m_targetClsid, nullptr, CLSCTX_INPROC_SERVER, m_targetClsid, (void**)&m_backend);
    if (SUCCEEDED(hr) && m_backend) {
        DebugLog("[DeltaCast] Backend Driver Loaded Successfully.\n");
        return true;
    }
    else {
        DebugLog("[DeltaCast] Failed to load backend driver (HR: 0x%x)\n", hr);
        return false;
    }
}

void CDeltaCastDriver::LoadConfiguration() {
    WCHAR modulePath[MAX_PATH];
    if (GetModuleFileNameW(g_hModule, modulePath, MAX_PATH) == 0) return;

    // DeltaCast.ini 확인
    std::wstring configPath = modulePath;
    size_t lastSlash = configPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        configPath = configPath.substr(0, lastSlash + 1);
    }
    configPath += L"DeltaCast.ini";

    DebugLog("[DeltaCast] Loading Config from: %ls\n", configPath.c_str());

    // INI에서 CLSID 읽기
    WCHAR clsidStr[64] = { 0 };
    GetPrivateProfileStringW(
        L"Settings",          // 섹션
        L"TargetDriverCLSID", // 키
        L"",                  // 기본값
        clsidStr,             // 저장할 버퍼
        64,                   // 버퍼 크기
        configPath.c_str()    // 파일 경로
    );
    // INI에서 WASAPI Device ID 읽기
    WCHAR wasapiIdBuf[256] = { 0 };
    GetPrivateProfileStringW(
        L"Settings",
        L"TargetWasapiID",
        L"", // 기본값: 빈 문자열 (기본 장치)
        wasapiIdBuf,
        256,
        configPath.c_str()
    );

    m_targetWasapiId = wasapiIdBuf;
    DebugLog("[DeltaCast] Target WASAPI ID: %ls\n", m_targetWasapiId.c_str());

    if (wcslen(clsidStr) > 0) {
        // CLSID 구조체로 변환
        HRESULT hr = CLSIDFromString(clsidStr, &m_targetClsid);
        if (SUCCEEDED(hr)) {
            m_hasConfig = true;
            DebugLog("[DeltaCast] Target CLSID Loaded: %ls\n", clsidStr);
        }
        else {
            DebugLog("[DeltaCast] Invalid CLSID format in INI file.\n");
        }
    }
    else {
        DebugLog("[DeltaCast] No TargetDriverCLSID found in INI.\n");
    }
}

ASIOBool CDeltaCastDriver::init(void* sysHandle) {
    if (!LoadBackendDriver()) return ASIOFalse;
    ASIOBool result = m_backend->init(sysHandle);
    if (result == ASIOTrue) {
        m_backend->getSampleRate(&m_sampleRate);
    }
    return result;
}

ASIOError CDeltaCastDriver::start() {
    if (!m_backend) return ASE_NotPresent;

    // WASAPI 렌더러 시작
    m_renderer.Start(&m_loopbackBufferL, &m_loopbackBufferR, m_targetWasapiId);

    return m_backend->start();
}

ASIOError CDeltaCastDriver::stop() {
    if (!m_backend) return ASE_NotPresent;

    // WASAPI 렌더러 중지
    m_renderer.Stop();

    return m_backend->stop();
}

void CDeltaCastDriver::sampleRateChanged(ASIOSampleRate sRate) {
    if (g_pThis && g_pThis->m_hostCallbacks.sampleRateDidChange) g_pThis->m_hostCallbacks.sampleRateDidChange(sRate);
}
long CDeltaCastDriver::asioMessage(long selector, long value, void* message, double* opt) {
    if (g_pThis && g_pThis->m_hostCallbacks.asioMessage) return g_pThis->m_hostCallbacks.asioMessage(selector, value, message, opt);
    return 0;
}

void CDeltaCastDriver::getDriverName(char* name) {
    strcpy_s(name, 32, "Delta_Cast ASIO");
}

long CDeltaCastDriver::getDriverVersion() { return 1; }

void CDeltaCastDriver::getErrorMessage(char* string) {
    if (m_backend) m_backend->getErrorMessage(string);
    else strcpy_s(string, 32, "Backend Not Loaded");
}

ASIOError CDeltaCastDriver::getChannels(long* numInputChannels, long* numOutputChannels) {
    return m_backend ? m_backend->getChannels(numInputChannels, numOutputChannels) : ASE_NotPresent;
}

ASIOError CDeltaCastDriver::getLatencies(long* inputLatency, long* outputLatency) {
    return m_backend ? m_backend->getLatencies(inputLatency, outputLatency) : ASE_NotPresent;
}

ASIOError CDeltaCastDriver::getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) {
    return m_backend ? m_backend->getBufferSize(minSize, maxSize, preferredSize, granularity) : ASE_NotPresent;
}

ASIOError CDeltaCastDriver::canSampleRate(ASIOSampleRate sampleRate) {
    return m_backend ? m_backend->canSampleRate(sampleRate) : ASE_NotPresent;
}

ASIOError CDeltaCastDriver::getSampleRate(ASIOSampleRate* sampleRate) {
    return m_backend ? m_backend->getSampleRate(sampleRate) : ASE_NotPresent;
}

ASIOError CDeltaCastDriver::setSampleRate(ASIOSampleRate sampleRate) {
    if (!m_backend) return ASE_NotPresent;

    ASIOError result = m_backend->setSampleRate(sampleRate);
    if (result == ASE_OK) {
        m_sampleRate = sampleRate;
    }
    return result;
}

ASIOError CDeltaCastDriver::getClockSources(ASIOClockSource* clocks, long* numSources) {
    return m_backend ? m_backend->getClockSources(clocks, numSources) : ASE_NotPresent;
}

ASIOError CDeltaCastDriver::setClockSource(long reference) {
    return m_backend ? m_backend->setClockSource(reference) : ASE_NotPresent;
}

ASIOError CDeltaCastDriver::getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) {
    return m_backend ? m_backend->getSamplePosition(sPos, tStamp) : ASE_NotPresent;
}

ASIOError CDeltaCastDriver::getChannelInfo(ASIOChannelInfo* info) {
    return m_backend ? m_backend->getChannelInfo(info) : ASE_NotPresent;
}

ASIOError CDeltaCastDriver::disposeBuffers() {
    return m_backend ? m_backend->disposeBuffers() : ASE_NotPresent;
}

ASIOError CDeltaCastDriver::controlPanel() {
    return m_backend ? m_backend->controlPanel() : ASE_NotPresent;
}

ASIOError CDeltaCastDriver::future(long selector, void* opt) {
    return m_backend ? m_backend->future(selector, opt) : ASE_NotPresent;
}

ASIOError CDeltaCastDriver::outputReady() {
    return m_backend ? m_backend->outputReady() : ASE_NotPresent;
}

// COM 구현
HRESULT STDMETHODCALLTYPE CDeltaCastDriver::QueryInterface(REFIID riid, void** ppv) {
    if (riid == IID_IUnknown || riid == kIID_IASIO || riid == CLSID_Delta_Cast) {
        *ppv = static_cast<IASIO*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE CDeltaCastDriver::AddRef() {
    return ++m_refCount;
}

ULONG STDMETHODCALLTYPE CDeltaCastDriver::Release() {
    ULONG ref = --m_refCount;
    if (ref == 0) delete this;
    return ref;
}