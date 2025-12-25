#include "WasapiRenderer.h"
#include <functiondiscoverykeys_devpkey.h>
#include <immintrin.h>
#include <avrt.h>
#pragma comment(lib, "avrt.lib")

const float INT32_TO_FLOAT = 4.65661287e-10f;
const float INT24_TO_FLOAT = 1.19209290e-7f;
const float INT16_TO_FLOAT = 3.05175781e-5f;

// 해제
template <class T> void SafeRelease(T** ppT) {
    if (*ppT) { (*ppT)->Release(); *ppT = nullptr; }
}

CWasapiRenderer::CWasapiRenderer() {}

CWasapiRenderer::~CWasapiRenderer() { Stop(); }

bool CWasapiRenderer::Start(ByteRingBuffer* pBufferL, ByteRingBuffer* pBufferR,
    const std::wstring& deviceId,
    ASIOSampleType sampleType, double inputSampleRate)
{
    if (m_bRunning) return true;

    m_pBufferL = pBufferL;
    m_pBufferR = pBufferR;
    m_sampleType = sampleType;
    m_inputRate = inputSampleRate;

    m_bRunning = true;
    m_renderThread = std::thread(&CWasapiRenderer::RenderThreadFunc, this, deviceId);

    // 오디오 스레드 우선순위 높임
    SetThreadPriority(m_renderThread.native_handle(), THREAD_PRIORITY_HIGHEST);

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

    IMMDeviceEnumerator* pEnumerator = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (FAILED(hr)) return devices;

    IMMDeviceCollection* pCollection = nullptr;
    // 활성화된 장치 검색
    hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);

    if (SUCCEEDED(hr)) {
        UINT count;
        pCollection->GetCount(&count);

        for (UINT i = 0; i < count; i++) {
            IMMDevice* pDevice = nullptr;
            if (SUCCEEDED(pCollection->Item(i, &pDevice))) {
                LPWSTR pwszID = nullptr;
                pDevice->GetId(&pwszID); // 장치 고유 ID

                IPropertyStore* pProps = nullptr;
                pDevice->OpenPropertyStore(STGM_READ, &pProps);

                PROPVARIANT varName;
                PropVariantInit(&varName);
                pProps->GetValue(PKEY_Device_FriendlyName, &varName);

                if (pwszID && varName.pwszVal) {
                    devices.push_back({ pwszID, varName.pwszVal });
                }

                PropVariantClear(&varName);
                CoTaskMemFree(pwszID);
                SafeRelease(&pProps);
                SafeRelease(&pDevice);
            }
        }
    }
    SafeRelease(&pCollection);
    SafeRelease(&pEnumerator);
    return devices;
}

void CWasapiRenderer::ConvertRawToFloat(const void* input, float* output, size_t sampleCount) {
    if (!input || !output) return;

    if (m_sampleType == ASIOSTInt32LSB) {
        const int32_t* src = (const int32_t*)input;
        size_t i = 0;

        // 8개씩 병렬 처리
        __m256 mulVal = _mm256_set1_ps(INT32_TO_FLOAT);
        for (; i + 8 <= sampleCount; i += 8) {
            __m256i vInt = _mm256_loadu_si256((const __m256i*) & src[i]);
            __m256 vFloat = _mm256_cvtepi32_ps(vInt);
            vFloat = _mm256_mul_ps(vFloat, mulVal);
            _mm256_storeu_ps(&output[i], vFloat);
        }
        // 남은 처리
        for (; i < sampleCount; ++i) {
            output[i] = (float)src[i] * INT32_TO_FLOAT;
        }
        return;
    }

    switch (m_sampleType) {
    case ASIOSTInt32LSB: {
        const int32_t* src = (const int32_t*)input;
        for (size_t i = 0; i < sampleCount; ++i) output[i] = (float)src[i] * INT32_TO_FLOAT;
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

void CWasapiRenderer::RenderThreadFunc(std::wstring targetDeviceId) {
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristics(L"Pro Audio", &taskIndex);
    if (hTask == NULL) {
        // 실패 시 높은 우선순위
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }
    try {
        // 스레드 시작 시 COM 초기화 
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), (void**)&m_pEnumerator);
        if (FAILED(hr)) return;

        // 출력 장치 가져오기
        if (targetDeviceId.empty()) {
            m_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_pDevice);
        }
        else {
            // 사용자가 선택한 장치
            hr = m_pEnumerator->GetDevice(targetDeviceId.c_str(), &m_pDevice);
            if (FAILED(hr)) {
                // 실패 시 기본 장치
                m_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_pDevice);
            }
        }

        if (!m_pDevice) return;

        // Audio Client 활성화
        hr = m_pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_pAudioClient);
        if (FAILED(hr)) return;

        // 포맷 설정 (Float 32bit, Stereo, 48kHz or 44.1kHz) 
        WAVEFORMATEX* pMixFormat = nullptr;
        m_pAudioClient->GetMixFormat(&pMixFormat);

		// 리샘플러 설정
        double outRate = (double)pMixFormat->nSamplesPerSec;
        if (m_inputRate <= 0.0) m_inputRate = 48000.0;
        m_resamplerL.Setup(m_inputRate, outRate);
        m_resamplerR.Setup(m_inputRate, outRate);

        // 초기화 (공유 모드)
        // REFTIMES_PER_SEC = 10000000 (100ns) / 버퍼 길이 10ms
        REFERENCE_TIME hnsRequestedDuration = 100000;

        // 이벤트 핸들
        HANDLE hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!hEvent) return;

        hr = m_pAudioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            hnsRequestedDuration,
            0,
            pMixFormat,
            nullptr);

        if (FAILED(hr)) { CloseHandle(hEvent); return; }

        hr = m_pAudioClient->SetEventHandle(hEvent);
        if (FAILED(hr)) { CloseHandle(hEvent); return; }

        // 렌더 클라이언트 서비스 획득
        hr = m_pAudioClient->GetService(__uuidof(IAudioRenderClient), (void**)&m_pRenderClient);
        if (FAILED(hr)) return;

        // 재생 시작
        m_pAudioClient->Start();

		// 샘플 크기
        int sampleSizeBytes = 4;
        switch (m_sampleType) {
        case ASIOSTInt16LSB: sampleSizeBytes = 2; break;
        case ASIOSTInt24LSB: sampleSizeBytes = 3; break;
        case ASIOSTFloat64LSB: sampleSizeBytes = 8; break;
        }

		// 버퍼 프레임 수 확인
        UINT32 bufferFrameCount;
        m_pAudioClient->GetBufferSize(&bufferFrameCount);

        // 여유 공간 확보
        size_t maxFrames = bufferFrameCount * 4;
        if (maxFrames < 4096) maxFrames = 4096; // 최소 안전장치

        // 벡터 메모리 할당
        m_rawTempL.resize(maxFrames * sampleSizeBytes);
        m_rawTempR.resize(maxFrames * sampleSizeBytes);

        m_floatTempL.resize(maxFrames);
        m_floatTempR.resize(maxFrames);

        m_resampledTempL.resize(maxFrames);
        m_resampledTempR.resize(maxFrames);

        while (m_bRunning) {
            DWORD waitResult = WaitForSingleObject(hEvent, 1000);
            if (waitResult != WAIT_OBJECT_0) break;

            UINT32 padding;
            if (FAILED(m_pAudioClient->GetCurrentPadding(&padding))) continue;

            UINT32 framesNeeded = bufferFrameCount - padding;
            if (framesNeeded == 0) continue;

            BYTE* pData;
            if (FAILED(m_pRenderClient->GetBuffer(framesNeeded, &pData))) continue;

            double ratio = m_inputRate / outRate;

            size_t samplesToRead = (size_t)(framesNeeded * ratio) + 2;
            size_t bytesAvailable = m_pBufferL->GetAvailableRead();
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
                size_t generatedL = m_resamplerL.Process(m_floatTempL.data(), samplesToRead, m_resampledTempL.data(), framesNeeded);
                size_t generatedR = m_resamplerR.Process(m_floatTempR.data(), samplesToRead, m_resampledTempR.data(), framesNeeded);

                // WASAPI에 쓰기
                float* pOut = (float*)pData;
                int channels = pMixFormat->nChannels;

                // 실제로 생성된 샘플 수만큼 기록
                for (UINT32 i = 0; i < generatedL && i < framesNeeded; i++) {
                    pOut[i * channels + 0] = m_resampledTempL[i];
                    if (channels > 1) pOut[i * channels + 1] = m_resampledTempR[i];
                }

                // 부족한 부분은 0(침묵)
                if (generatedL < framesNeeded) {
                    size_t missing = framesNeeded - generatedL;

					// 마지막 샘플 복사
                    float lastSampleL = (generatedL > 0) ? pOut[(generatedL - 1) * channels + 0] : 0.0f;
                    float lastSampleR = (channels > 1 && generatedL > 0) ? pOut[(generatedL - 1) * channels + 1] : 0.0f;

					// 남은 부분 마지막 샘플로 채우기
                    for (size_t k = 0; k < missing; k++) {
                        size_t destIdx = generatedL + k;
                        pOut[destIdx * channels + 0] = lastSampleL;
                        if (channels > 1) {
                            pOut[destIdx * channels + 1] = lastSampleR;
                        }
                    }
                }
            }
            else {
                // 데이터가 아예 없으면 침묵
                memset(pData, 0, framesNeeded * pMixFormat->nBlockAlign);
            }
			// 버퍼 해제
            m_pRenderClient->ReleaseBuffer(framesNeeded, 0);
        }

        // 정리
        if (hTask) AvRevertMmThreadCharacteristics(hTask);
        CloseHandle(hEvent);
        m_pAudioClient->Stop();
        CoTaskMemFree(pMixFormat);
        SafeRelease(&m_pRenderClient);
        SafeRelease(&m_pAudioClient);
        SafeRelease(&m_pDevice);
        SafeRelease(&m_pEnumerator);
        CoUninitialize();
    }
    catch (...) {
        // 에러시 드라이버만 종료
        if (m_pAudioClient) m_pAudioClient->Stop();
    }
}