#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include "RingBuffer.h"

struct AudioDevice {
    std::wstring id;
    std::wstring name;
};

class CWasapiRenderer {
public:
    CWasapiRenderer();
    ~CWasapiRenderer();

    // 장치 정보
    std::vector<AudioDevice> GetOutputDevices();

    // 초기화 및 재생 시작
    bool Start(LockFreeRingBuffer<float>* pBufferL, LockFreeRingBuffer<float>* pBufferR, const std::wstring& deviceId);
    // 재생 중지
    void Stop();

private:
    void RenderThreadFunc(std::wstring targetDeviceId);

    std::atomic<bool> m_bRunning{ false };
    std::thread m_renderThread;

    LockFreeRingBuffer<float>* m_pBufferL = nullptr;
    LockFreeRingBuffer<float>* m_pBufferR = nullptr;

    // WASAPI 인터페이스
    IMMDeviceEnumerator* m_pEnumerator = nullptr;
    IMMDevice* m_pDevice = nullptr;
    IAudioClient* m_pAudioClient = nullptr;
    IAudioRenderClient* m_pRenderClient = nullptr;
};