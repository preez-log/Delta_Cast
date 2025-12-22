#pragma once
#include <windows.h>
#include <iasiodrv.h>
#include <atomic>

#include "RingBuffer.h"
#include "WasapiRenderer.h"
#include "Resampler.h"

class CDeltaCastDriver : public IASIO {
public:
    CDeltaCastDriver();
    virtual ~CDeltaCastDriver();

    virtual ASIOBool init(void* sysHandle) override;
    virtual void getDriverName(char* name) override;
    virtual long getDriverVersion() override;
    virtual void getErrorMessage(char* string) override;
    virtual ASIOError start() override;
    virtual ASIOError stop() override;
    virtual ASIOError getChannels(long* numInputChannels, long* numOutputChannels) override;
    virtual ASIOError getLatencies(long* inputLatency, long* outputLatency) override;
    virtual ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) override;
    virtual ASIOError canSampleRate(ASIOSampleRate sampleRate) override;
    virtual ASIOError getSampleRate(ASIOSampleRate* sampleRate) override;
    virtual ASIOError setSampleRate(ASIOSampleRate sampleRate) override;
    virtual ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) override;
    virtual ASIOError setClockSource(long reference) override;
    virtual ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) override;
    virtual ASIOError getChannelInfo(ASIOChannelInfo* info) override;
    virtual ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) override;
    virtual ASIOError disposeBuffers() override;
    virtual ASIOError controlPanel() override;
    virtual ASIOError future(long selector, void* opt) override;
    virtual ASIOError outputReady() override;

    // COM 사용시 필요
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    virtual ULONG STDMETHODCALLTYPE AddRef() override;
    virtual ULONG STDMETHODCALLTYPE Release() override;

    // 전역 접근을 위한 포인터
    static CDeltaCastDriver* g_pThis;

    LockFreeRingBuffer<float> m_loopbackBufferL{ 131072 }; // 약 2초 분량
    LockFreeRingBuffer<float> m_loopbackBufferR{ 131072 };

private:
    ASIOCallbacks m_hostCallbacks;              // 원본 콜백
    ASIOCallbacks m_myCallbacks;                // 복제 콜백
    ASIOBufferInfo* m_bufferInfos = nullptr;    // 오디오 버퍼 위치
    ASIOSampleType m_sampleType = ASIOSTFloat32LSB;
    ASIOSampleRate m_sampleRate = 44100.0;
    std::vector<float> m_convertBufferL;
    std::vector<float> m_convertBufferR;
    long m_numChannels = 0;
    long m_bufferSize = 0;
    long m_outIndexL = -1;
    long m_outIndexR = -1;

    // 샘플링 주파수 변환
    Resampler m_resamplerL;
    Resampler m_resamplerR;
    std::vector<float> m_resampledDataL;
    std::vector<float> m_resampledDataR;

    long m_lastProcessedBufferIndex = -1;
    static void bufferSwitch(long doubleBufferIndex, ASIOBool directProcess);
    static ASIOTime* bufferSwitchTimeInfo(ASIOTime* timeInfo, long index, ASIOBool processNow);
    static void sampleRateChanged(ASIOSampleRate sRate);
    static long asioMessage(long selector, long value, void* message, double* opt);
    static void CopyAudioToRingBuffer(long index);

    std::atomic<ULONG> m_refCount{ 1 };
    CLSID m_targetClsid = { 0 };
    std::wstring m_targetWasapiId;
    bool  m_hasConfig = false;
    void LoadConfiguration();
    bool LoadBackendDriver();
    IASIO* m_backend = nullptr;
    CWasapiRenderer m_renderer;
};