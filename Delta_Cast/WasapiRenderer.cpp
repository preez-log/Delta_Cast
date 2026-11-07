#include "WasapiRenderer.h"
#include "Logging.h"
#include "ScopeGuards.h"
#include <functiondiscoverykeys_devpkey.h>
#include <emmintrin.h>
#include <algorithm>
#include <avrt.h>
#include <cmath>
#include <stdexcept>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#pragma comment(lib, "avrt.lib")

const float INT32_TO_FLOAT = 4.65661287e-10f;
const float INT24_TO_FLOAT = 1.19209290e-7f;
const float INT16_TO_FLOAT = 3.05175781e-5f;

CWasapiRenderer::CWasapiRenderer() {}
CWasapiRenderer::~CWasapiRenderer() { Stop(); }

bool CWasapiRenderer::Start(ByteRingBuffer* pBufferL, ByteRingBuffer* pBufferR,
    const std::wstring& deviceId, ASIOSampleType sampleType, double inputSampleRate, size_t threshold)
{
    if (m_bRunning) return true;

    m_pBufferL = pBufferL;
    m_pBufferR = pBufferR;
    m_sampleType = sampleType;
    m_inputRate = inputSampleRate;

    m_bRunning = true;
    m_renderThread = std::thread(&CWasapiRenderer::RenderThreadFunc, this, deviceId, threshold);

    // 오디오 스레드 우선순위 높임
    SetThreadPriority(m_renderThread.native_handle(), THREAD_PRIORITY_TIME_CRITICAL);

    return true;
}

void CWasapiRenderer::Stop() {
    m_bRunning = false;
    if (m_renderThread.joinable()) {
        m_renderThread.join();
    }
}

std::vector<AudioDevice> CWasapiRenderer::GetOutputDevices() {
    std::vector<AudioDevice> devices;
    HRESULT hr;

    ComPtr<IMMDeviceEnumerator> pEnumerator;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pEnumerator));
    if (FAILED(hr)) return devices;

    ComPtr<IMMDeviceCollection> pCollection;
    // 활성화된 장치 검색
    hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, pCollection.GetAddressOf());

    if (SUCCEEDED(hr)) {
        UINT count;
        pCollection->GetCount(&count);

        for (UINT i = 0; i < count; i++) {
            ComPtr<IMMDevice> pDevice;
            if (SUCCEEDED(pCollection->Item(i, pDevice.GetAddressOf()))) {
                LPWSTR pwszID = nullptr;
                pDevice->GetId(&pwszID); // 장치 고유 ID

                ComPtr<IPropertyStore> pProps;
                pDevice->OpenPropertyStore(STGM_READ, pProps.GetAddressOf());

                PROPVARIANT varName;
                PropVariantInit(&varName);
                if (pProps) pProps->GetValue(PKEY_Device_FriendlyName, &varName);

                if (pwszID && varName.pwszVal) {
                    devices.push_back({ pwszID, varName.pwszVal });
                }

                PropVariantClear(&varName);
                CoTaskMemFree(pwszID);
            }
        }
    }
    return devices;
}

void CWasapiRenderer::ConvertRawToFloat(const void* input, float* output, size_t sampleCount) {
    if (!input || !output) return;

    switch (m_sampleType) {
    case ASIOSTInt32LSB: {
        const int32_t* src = (const int32_t*)input;
        size_t i = 0;
        // SSE2: 4개씩 변환
        const __m128 mulVal = _mm_set1_ps(INT32_TO_FLOAT);
        for (; i + 4 <= sampleCount; i += 4) {
            __m128i vInt = _mm_loadu_si128((const __m128i*) & src[i]);
            _mm_storeu_ps(&output[i], _mm_mul_ps(_mm_cvtepi32_ps(vInt), mulVal));
        }
        for (; i < sampleCount; ++i) output[i] = (float)src[i] * INT32_TO_FLOAT;
        break;
    }
    case ASIOSTFloat32LSB: {
        memcpy(output, input, sampleCount * sizeof(float));
        break;
    }
    case ASIOSTInt24LSB: {
        const uint8_t* src = (const uint8_t*)input;
        for (size_t i = 0; i < sampleCount; ++i) {
            int32_t s = (int32_t)((src[i * 3 + 2] << 24) | (src[i * 3 + 1] << 16) | (src[i * 3] << 8));
            output[i] = (float)(s >> 8) * INT24_TO_FLOAT;
        }
        break;
    }
    case ASIOSTInt16LSB: {
        const int16_t* src = (const int16_t*)input;
        for (size_t i = 0; i < sampleCount; ++i) output[i] = (float)src[i] * INT16_TO_FLOAT;
        break;
    }
    case ASIOSTFloat64LSB: {
        const double* src = (const double*)input;
        for (size_t i = 0; i < sampleCount; ++i) output[i] = (float)src[i];
        break;
    }
    default: // 지원 안함 -> 침묵
        memset(output, 0, sampleCount * sizeof(float));
        break;
    }
}

// IAudioClient3 저지연 공유 스트림 협상을 시도하고, 불가하면 기존 v1 경로로 폴백
HRESULT CWasapiRenderer::InitSharedStream(IAudioClient* pAudioClient, WAVEFORMATEX* pMixFormat, HANDLE hEvent,
                                            ComPtr<IAudioRenderClient>& outRenderClient, bool& outUsedLowLatency,
                                            UINT32& outPeriodFrames) {
    outUsedLowLatency = false;
    outPeriodFrames = 0;
    HRESULT hr = E_FAIL;

    // IAudioClient3 저지연
    if (!m_forceLegacyInit) {
        ComPtr<IAudioClient3> pClient3;
        if (SUCCEEDED(pAudioClient->QueryInterface(IID_PPV_ARGS(&pClient3))) && pClient3) {
            UINT32 defaultPeriod = 0, fundamentalPeriod = 0, minPeriod = 0, maxPeriod = 0;
            hr = pClient3->GetSharedModeEnginePeriod(pMixFormat,
                &defaultPeriod, &fundamentalPeriod, &minPeriod, &maxPeriod);

            if (SUCCEEDED(hr) && minPeriod > 0) {
                // 저지연 공유 스트림은 믹스 포맷을 그대로 사용해야 함.
                hr = pClient3->InitializeSharedAudioStream(
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK, minPeriod, pMixFormat, nullptr);

                if (SUCCEEDED(hr)) {
                    outUsedLowLatency = true;
                    outPeriodFrames = minPeriod;
                    LogInfo("IAudioClient3 period: %u frames (%.2f ms), default %u",
                        minPeriod, minPeriod * 1000.0 / pMixFormat->nSamplesPerSec, defaultPeriod);
                }
                else {
                    DebugLog("InitializeSharedAudioStream failed (0x%08X), falling back to v1", hr);
                }
            }
            else {
                DebugLog("GetSharedModeEnginePeriod failed (0x%08X), falling back to v1", hr);
            }
        }
    }

    // v1 폴백 (저지연 실패 / 미지원)
    if (!outUsedLowLatency) {
        REFERENCE_TIME hnsRequestedDuration = 50000; // 5ms 요청(엔진이 10ms로 정렬)
        hr = pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            hnsRequestedDuration, 0, pMixFormat, nullptr);
        if (FAILED(hr)) {
            DebugLog("v1 Initialize failed (0x%08X)", hr);
            return hr;
        }
        DebugLog("WASAPI v1 shared path initialized");

        // 엔진 기본 주기 (100ns 단위) → 프레임
        REFERENCE_TIME defaultPeriod = 0, minDevicePeriod = 0;
        if (SUCCEEDED(pAudioClient->GetDevicePeriod(&defaultPeriod, &minDevicePeriod)))
            outPeriodFrames = (UINT32)(defaultPeriod * pMixFormat->nSamplesPerSec / 10000000);
    }

    // 이벤트 핸들 + 렌더
    hr = pAudioClient->SetEventHandle(hEvent);
    if (FAILED(hr)) { DebugLog("SetEventHandle failed (0x%08X)", hr); return hr; }

    hr = pAudioClient->GetService(IID_PPV_ARGS(&outRenderClient));
    if (FAILED(hr)) { DebugLog("GetService(IAudioRenderClient) failed (0x%08X)", hr); return hr; }

    return S_OK;
}

void CWasapiRenderer::RenderThreadFunc(std::wstring targetDeviceId, size_t safeThreshold) {

    // COM 초기화 (스코프 끝에서 자동 CoUninitialize)
    ComInitGuard com(COINIT_MULTITHREADED);

    // MMCSS 등록 (스코프 끝에서 자동 AvRevert)
    DWORD taskIndex = 0;
    MmcssGuard mmcss(AvSetMmThreadCharacteristics(L"Pro Audio", &taskIndex));
    if (mmcss.get() == NULL) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }

    // 멤버에서 내려온 스레드 지역 COM 포인터 (스코프 끝/예외 시 자동 Release)
    ComPtr<IMMDeviceEnumerator> pEnumerator;
    ComPtr<IMMDevice>           pDevice;
    ComPtr<IAudioClient>        pAudioClient;
    ComPtr<IAudioRenderClient>  pRenderClient;
    try {
        // 스레드 시작 시 COM 초기화 
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&pEnumerator));
        if (FAILED(hr)) throw std::runtime_error("Enum failed");

        // 출력 장치 가져오기
        if (targetDeviceId.empty()) pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, pDevice.GetAddressOf());
        else pEnumerator->GetDevice(targetDeviceId.c_str(), pDevice.GetAddressOf());
        if (!pDevice) throw std::runtime_error("No Device");

        // 장치 이름 (로그용)
        std::wstring deviceName = L"(unknown)";
        {
            ComPtr<IPropertyStore> props;
            if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, props.GetAddressOf()))) {
                PROPVARIANT v; PropVariantInit(&v);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.pwszVal)
                    deviceName = v.pwszVal;
                PropVariantClear(&v);
            }
        }

        // Audio Client 활성화
        hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)pAudioClient.GetAddressOf());
        if (FAILED(hr)) {
            char errMsg[64];
            sprintf_s(errMsg, "Activate Failed (HR: 0x%08X)", hr);
            throw std::runtime_error(errMsg);
        }

        // 포맷 설정 (Float 32bit, Stereo, 48kHz or 44.1kHz) 
        CoMemGuard<WAVEFORMATEX> mixFormat;
        pAudioClient->GetMixFormat(mixFormat.put());
        WAVEFORMATEX* pMixFormat = mixFormat.get();

        int bitDepth = pMixFormat->wBitsPerSample;
        bool isFloat = (pMixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);

        if (pMixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            WAVEFORMATEXTENSIBLE* pExt = (WAVEFORMATEXTENSIBLE*)pMixFormat;
            if (IsEqualGUID(pExt->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
                isFloat = true;
            }
            else if (IsEqualGUID(pExt->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
                isFloat = false;
            }
        }

        // 리샘플러 설정
        double outRate = (double)pMixFormat->nSamplesPerSec;
        if (m_inputRate <= 0.0) m_inputRate = 48000.0;
        m_resamplerL.Setup(m_inputRate, outRate);
        m_resamplerR.Setup(m_inputRate, outRate);

        bool needResample = (std::abs(m_inputRate - outRate) > 1.0);

        // 이벤트 핸들
        HandleGuard event(CreateEvent(nullptr, FALSE, FALSE, nullptr));
        if (!event.valid()) { LogError("CreateEvent failed"); throw std::runtime_error("CreateEvent failed"); }
        HANDLE hEvent = event.get();

        // 공유 스트림 초기화: IAudioClient3 저지연 협상 → 실패 시 v1
        bool usedLowLatency = false;
        UINT32 periodFrames = 0;
        hr = InitSharedStream(pAudioClient.Get(), pMixFormat, hEvent, pRenderClient, usedLowLatency, periodFrames);
        if (FAILED(hr)) {
            char buf[80]; sprintf_s(buf, "InitSharedStream Failed (0x%08X)", hr);
            LogError("%s", buf);
            throw std::runtime_error(buf);
        }
        LogInfo("WASAPI: %s | %u Hz %u ch %d-bit %s | %s | resample %s",
            ToUtf8(deviceName).c_str(),
            pMixFormat->nSamplesPerSec, pMixFormat->nChannels, bitDepth,
            isFloat ? "float" : "int",
            usedLowLatency ? "low-latency (IAudioClient3)" : "standard (v1)",
            needResample ? "ON" : "OFF");

        // 재생 시작
        pAudioClient->Start();

        // 샘플 크기
        int sampleSizeBytes = 4;
        switch (m_sampleType) {
        case ASIOSTInt16LSB: sampleSizeBytes = 2; break;
        case ASIOSTInt24LSB: sampleSizeBytes = 3; break;
        case ASIOSTFloat64LSB: sampleSizeBytes = 8; break;
        }

        // 버퍼 프레임 수 확인
        UINT32 bufferFrameCount;
        pAudioClient->GetBufferSize(&bufferFrameCount);

        // 레이턴시 진단 로그
        {
            REFERENCE_TIME streamLatency = 0;
            pAudioClient->GetStreamLatency(&streamLatency);   // 100ns 단위
            LogInfo("WASAPI buffer: %u frames (%.2f ms) | stream latency: %.2f ms",
                bufferFrameCount, bufferFrameCount * 1000.0 / pMixFormat->nSamplesPerSec,
                streamLatency / 10000.0);
        }

        // 유지할 최대 padding: 주기 2개 분량 (주기를 모르면 버퍼 전체)
        const UINT32 targetPadding = (periodFrames > 0)
            ? (std::min)(periodFrames * 2, bufferFrameCount)
            : bufferFrameCount;
        LogInfo("WASAPI target padding: %u frames (%.2f ms)",
            targetPadding, targetPadding * 1000.0 / pMixFormat->nSamplesPerSec);

        // 여유 공간 확보: 엔진 버퍼 프레임 수의 배수로 임시 버퍼
        const size_t kFrameHeadroom = 4;
        size_t maxFrames = (size_t)bufferFrameCount * kFrameHeadroom;
        if (maxFrames < 4096) maxFrames = 4096; // 최소 안전장치

        // 벡터 메모리 할당
        m_rawTempL.resize(maxFrames * sampleSizeBytes);
        m_rawTempR.resize(maxFrames * sampleSizeBytes);
        m_floatTempL.resize(maxFrames);
        m_floatTempR.resize(maxFrames);
        m_resampledTempL.resize(maxFrames);
        m_resampledTempR.resize(maxFrames);

        bool isBuffering = true;

        while (m_bRunning) {
            DWORD waitResult = WaitForSingleObject(hEvent, 2000);
            if (waitResult == WAIT_TIMEOUT) {
                continue;
            }
            if (waitResult != WAIT_OBJECT_0) {
                break;
            }
            UINT32 padding;
            if (FAILED(pAudioClient->GetCurrentPadding(&padding))) { 
                continue; 
            }
            UINT32 framesNeeded = (padding < targetPadding) ? (targetPadding - padding) : 0;
            if (framesNeeded == 0) {
                continue;
            }

            BYTE* pData;
            HRESULT hrBuffer = pRenderClient->GetBuffer(framesNeeded, &pData);
            if (FAILED(hrBuffer)) {
                if (hrBuffer == AUDCLNT_E_DEVICE_INVALIDATED) break; // 장치 연결 끊김
                continue;
            }

            // 초기 버퍼링
            size_t bytesAvailable = m_pBufferL->GetAvailableRead();

            if (!isBuffering && bytesAvailable < 128) {
                isBuffering = true;
            }
            if (isBuffering) {
                memset(pData, 0, framesNeeded * pMixFormat->nBlockAlign);
                pRenderClient->ReleaseBuffer(framesNeeded, 0);

                // 재생
                if (bytesAvailable > safeThreshold) {
                    isBuffering = false;
                }
                continue;
            }

            double ratio = m_inputRate / outRate;
            // 필요한 입력 샘플 수 계산
            size_t samplesToRead = (size_t)ceil(framesNeeded * ratio);
            size_t samplesAvailable = bytesAvailable / sampleSizeBytes;

            // Underrun
            if (samplesToRead > samplesAvailable) {
                samplesToRead = samplesAvailable;
            }

            if (samplesToRead > 0) {
                // Pop (Byte 단위)
                size_t bytesRead = samplesToRead * sampleSizeBytes;
                m_pBufferL->Pop(m_rawTempL.data(), bytesRead);
                m_pBufferR->Pop(m_rawTempR.data(), bytesRead);

                // Convert (Byte -> Float)
                ConvertRawToFloat(m_rawTempL.data(), m_floatTempL.data(), samplesToRead);
                ConvertRawToFloat(m_rawTempR.data(), m_floatTempR.data(), samplesToRead);

                // Resample (InRate -> OutRate)
                size_t generatedL, generatedR;
                if (needResample) {
                    generatedL = m_resamplerL.Process(m_floatTempL.data(), samplesToRead, m_resampledTempL.data(), framesNeeded);
                    generatedR = m_resamplerR.Process(m_floatTempR.data(), samplesToRead, m_resampledTempR.data(), framesNeeded);
                }
                else {
                    // 비율이 1.0이면 단순 복사
                    size_t copyCount = (samplesToRead < framesNeeded) ? samplesToRead : framesNeeded;
                    memcpy(m_resampledTempL.data(), m_floatTempL.data(), copyCount * sizeof(float));
                    memcpy(m_resampledTempR.data(), m_floatTempR.data(), copyCount * sizeof(float));
                    generatedL = generatedR = copyCount;
                }

                // WASAPI에 쓰기
                BYTE* pRawOut = (BYTE*)pData;
                int channels = pMixFormat->nChannels;

                // 출력 버퍼 전체를 먼저 0으로 채운다.
                // 나머지 채널이 초기화되지 않아 이전 버퍼의 쓰레기 잡음이 나가는 것을 방지.
                memset(pRawOut, 0, (size_t)framesNeeded * channels * (bitDepth / 8));

                // [32-bit Float]
                if (bitDepth == 32 && isFloat) {
                    float* pFloat = (float*)pRawOut;
                    for (UINT32 i = 0; i < framesNeeded; i++) {
                        float sampleL = (i < generatedL) ? m_resampledTempL[i] : 0.0f;
                        float sampleR = (channels > 1 && i < generatedR) ? m_resampledTempR[i] : sampleL;

                        pFloat[i * channels + 0] = sampleL;
                        if (channels > 1) pFloat[i * channels + 1] = sampleR;
                    }
                }
                // [32-bit Int]
                else if (bitDepth == 32 && !isFloat) {
                    int32_t* pInt32 = (int32_t*)pRawOut;
                    for (UINT32 i = 0; i < framesNeeded; i++) {
                        float sampleL = (i < generatedL) ? m_resampledTempL[i] : 0.0f;
                        float sampleR = (channels > 1 && i < generatedR) ? m_resampledTempR[i] : sampleL;

                        sampleL = std::clamp(sampleL, -1.0f, 1.0f);
                        sampleR = std::clamp(sampleR, -1.0f, 1.0f); // [수정됨] sampleR

                        // Float -> Int32 
                        pInt32[i * channels + 0] = (int32_t)(sampleL * 2147483647.0f);
                        if (channels > 1) pInt32[i * channels + 1] = (int32_t)(sampleR * 2147483647.0f);
                    }
                }
                // [16-bit Int]
                else if (bitDepth == 16) {
                    int16_t* pInt16 = (int16_t*)pRawOut;
                    for (UINT32 i = 0; i < framesNeeded; i++) {
                        float sampleL = (i < generatedL) ? m_resampledTempL[i] : 0.0f;
                        float sampleR = (channels > 1 && i < generatedR) ? m_resampledTempR[i] : sampleL;

                        sampleL = std::clamp(sampleL, -1.0f, 1.0f);
                        sampleR = std::clamp(sampleR, -1.0f, 1.0f); // [수정됨] sampleR

                        pInt16[i * channels + 0] = (int16_t)(sampleL * 32767.0f);
                        if (channels > 1) pInt16[i * channels + 1] = (int16_t)(sampleR * 32767.0f);
                    }
                }
                // [24-bit Int]
                else if (bitDepth == 24) {
                    uint8_t* pBytes = (uint8_t*)pRawOut;
                    for (UINT32 i = 0; i < framesNeeded; i++) {
                        float sampleL = (i < generatedL) ? m_resampledTempL[i] : 0.0f;
                        float sampleR = (channels > 1 && i < generatedR) ? m_resampledTempR[i] : sampleL;

                        sampleL = std::clamp(sampleL, -1.0f, 1.0f);
                        sampleR = std::clamp(sampleR, -1.0f, 1.0f);

                        int32_t valL = (int32_t)(sampleL * 8388607.0f);
                        int32_t valR = (int32_t)(sampleR * 8388607.0f);

                        size_t offset = i * channels * 3;
                        pBytes[offset + 0] = (valL >> 0) & 0xFF;
                        pBytes[offset + 1] = (valL >> 8) & 0xFF;
                        pBytes[offset + 2] = (valL >> 16) & 0xFF;

                        if (channels > 1) {
                            pBytes[offset + 3] = (valR >> 0) & 0xFF;
                            pBytes[offset + 4] = (valR >> 8) & 0xFF;
                            pBytes[offset + 5] = (valR >> 16) & 0xFF;
                        }
                    }
                }
            }
            else {
                // 데이터가 아예 없으면 침묵
                memset(pData, 0, framesNeeded * pMixFormat->nBlockAlign);
            }
            // 버퍼 해제
            pRenderClient->ReleaseBuffer(framesNeeded, 0);
        }

        // 정리
        pAudioClient->Stop();
    }
    catch (const std::exception& e) {
        // pAudioClient 가 살아있으면 Stop()만 명시 호출
        LogError("RenderThread aborted: %s", e.what());
        if (pAudioClient) pAudioClient->Stop();
    }
    catch (...) {
        LogError("RenderThread aborted: unknown exception");
        if (pAudioClient) pAudioClient->Stop();
    }
}