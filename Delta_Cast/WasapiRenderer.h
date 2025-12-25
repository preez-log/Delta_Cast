#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#ifndef MY_ASIO
#define MY_ASIO
#include <iasiodrv.h>
#endif
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include "RingBuffer.h"
#include "Resampler.h"

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
    bool Start(ByteRingBuffer* pBufferL, ByteRingBuffer* pBufferR,
        const std::wstring& deviceId,
        ASIOSampleType sampleType, double inputSampleRate, size_t threshold);
    // 재생 중지
    void Stop();

private:
    void RenderThreadFunc(std::wstring targetDeviceId, size_t threshold);
    void ConvertRawToFloat(const void* input, float* output, size_t sampleCount);

    std::atomic<bool> m_bRunning{ false };
    std::thread m_renderThread;

    ByteRingBuffer* m_pBufferL = nullptr;
    ByteRingBuffer* m_pBufferR = nullptr;

    ASIOSampleType m_sampleType = ASIOSTFloat32LSB;
    double m_inputRate = 48000.0;

    Resampler m_resamplerL;
    Resampler m_resamplerR;

    // 임시 버퍼
    std::vector<uint8_t> m_rawTempL, m_rawTempR;
    std::vector<float>   m_floatTempL, m_floatTempR;
    std::vector<float>   m_resampledTempL, m_resampledTempR;

    // WASAPI 인터페이스
    IMMDeviceEnumerator* m_pEnumerator = nullptr;
    IMMDevice* m_pDevice = nullptr;
    IAudioClient* m_pAudioClient = nullptr;
    IAudioRenderClient* m_pRenderClient = nullptr;
};