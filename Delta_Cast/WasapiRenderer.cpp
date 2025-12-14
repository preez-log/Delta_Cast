#include "WasapiRenderer.h"
#include <functiondiscoverykeys_devpkey.h>

// 해제
template <class T> void SafeRelease(T** ppT) {
    if (*ppT) { (*ppT)->Release(); *ppT = nullptr; }
}

CWasapiRenderer::CWasapiRenderer() {
}

CWasapiRenderer::~CWasapiRenderer() {
    Stop();
}

bool CWasapiRenderer::Start(LockFreeRingBuffer<float>* pBufferL, LockFreeRingBuffer<float>* pBufferR, const std::wstring& deviceId) {
    if (m_bRunning) return true;

    m_pBufferL = pBufferL;
    m_pBufferR = pBufferR;
    m_bRunning = true;

    // 별도 스레드에서 오디오 렌더링
    m_renderThread = std::thread(&CWasapiRenderer::RenderThreadFunc, this, deviceId);

    // 끊김 방지
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
                pProps->GetValue(PKEY_Device_FriendlyName, &varName); // 사람이 읽을 수 있는 이름

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

void CWasapiRenderer::RenderThreadFunc(std::wstring targetDeviceId) {
    try {
        // 스레드 시작 시 COM 초기화 
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) {
            // 아파트먼트 모델 시도
            hr = CoInitialize(nullptr);
        }
        // 장치 열거
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
            CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&m_pEnumerator);
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

        // 초기화 (공유 모드)
        // REFTIMES_PER_SEC = 10000000 (100ns) / 버퍼 길이 10ms
        REFERENCE_TIME hnsRequestedDuration = 10000000 / 100;

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

        // 버퍼 크기 확인
        UINT32 bufferFrameCount;
        m_pAudioClient->GetBufferSize(&bufferFrameCount);

        // 재생 시작
        m_pAudioClient->Start();

        BYTE* pData = nullptr;
        UINT32 padding;
        UINT32 available;

        // --- 렌더링 루프 ---
        while (m_bRunning) {

            // 신호 대기
            DWORD waitResult = WaitForSingleObject(hEvent, 1000);
            if (waitResult != WAIT_OBJECT_0) break;

            // 현재 버퍼에 데이터 확인
            hr = m_pAudioClient->GetCurrentPadding(&padding);

            // 가용 공간
            available = bufferFrameCount - padding;

            if (available > 0) {
                // WASAPI 버퍼 잠금
                hr = m_pRenderClient->GetBuffer(available, &pData);
                if (FAILED(hr)) {
                    break;
                }
                if (SUCCEEDED(hr)) {
                    float* pOut = (float*)pData;
                    int channels = pMixFormat->nChannels;

                    // 링버퍼 확인
                    size_t availableSamples = m_pBufferL->GetAvailableRead();

                    if (availableSamples > bufferFrameCount * 4) {
                        // 버림
                        float dummy[1024];
                        m_pBufferL->Pop(dummy, 1024);
                        m_pBufferR->Pop(dummy, 1024);
                    }
                    else if (availableSamples < bufferFrameCount / 2) {
                        memset(pOut, 0, available * channels * sizeof(float));
                        m_pRenderClient->ReleaseBuffer(available, 0);
                        continue;
                    }

                    // 임시버퍼
                    std::vector<float> tempL(available);
                    std::vector<float> tempR(available);

                    // 링버퍼 Pop
                    size_t readL = m_pBufferL->Pop(tempL.data(), available);
                    size_t readR = m_pBufferR->Pop(tempR.data(), available);

                    // 부족시 0(침묵)
                    for (size_t i = readL; i < available; i++) tempL[i] = 0.0f;
                    for (size_t i = readR; i < available; i++) tempR[i] = 0.0f;

                    // Interleaving: L R L R ...
                    for (UINT32 i = 0; i < available; i++) {
                        pOut[i * channels + 0] = tempL[i]; // Left
                        if (channels > 1) {
                            pOut[i * channels + 1] = tempR[i]; // Right
                        }
                    }

                    m_pRenderClient->ReleaseBuffer(available, 0);
                }
            }
        }

        // 정리
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