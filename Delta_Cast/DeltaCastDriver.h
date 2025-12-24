#pragma once
#include <windows.h>
#include <iasiodrv.h>
#include <atomic>
#include <memory>

#include "RingBuffer.h"
#include "DriverBackend.h"
#include "WasapiRenderer.h"
#include "Resampler.h"

class CDeltaCastDriver : public IASIO {
public:
    CDeltaCastDriver();
    virtual ~CDeltaCastDriver();

    virtual ASIOBool init(void* sysHandle) override;
    virtual ASIOError start() override;
    virtual ASIOError stop() override;
    virtual void getDriverName(char* name) override;
    virtual long getDriverVersion() override;
    virtual void getErrorMessage(char* string) override;
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

    // --- COM 구현 --- 
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    virtual ULONG STDMETHODCALLTYPE AddRef() override;
    virtual ULONG STDMETHODCALLTYPE Release() override;

    // --- 전역 접근 포인터 ---
    static CDeltaCastDriver* g_pThis;

	// --- 송출 버퍼 ---
    LockFreeRingBuffer<float> m_loopbackBufferL{ 131072 }; // 약 2초 분량
    LockFreeRingBuffer<float> m_loopbackBufferR{ 131072 };

	// --- 버퍼 스위치 트리거 ---
    void TriggerBufferSwitch(long doubleBufferIndex);

    friend class VirtualBackend;
    friend class ProxyBackend;

private:
    // --- 설정 ---
    void LoadConfiguration();

    // 실제 동작을 담당할 전략 객체
    std::unique_ptr<IDriverBackend> m_backendImpl;

    // --- 공통 오디오 처리 ---
    void CopyAudioToRingBuffer(long index);

    ASIOCallbacks m_hostCallbacks;
    ASIOCallbacks m_myCallbacks;
    ASIOBufferInfo* m_bufferInfos = nullptr;
    ASIOSampleType m_sampleType = ASIOSTFloat32LSB;
    ASIOSampleRate m_sampleRate = 44100.0;
    long m_numChannels = 0;
    long m_bufferSize = 0;
    long m_outIndexL = -1;
    long m_outIndexR = -1;
    long m_lastProcessedBufferIndex = -1;

    std::vector<float> m_convertBufferL;
    std::vector<float> m_convertBufferR;
    std::vector<float> m_resampledDataL;
    std::vector<float> m_resampledDataR;
    Resampler m_resamplerL;
    Resampler m_resamplerR;

    // WASAPI 렌더러
    CWasapiRenderer m_renderer;

    // --- ASIO 표준 함수 ---
    static void bufferSwitch(long doubleBufferIndex, ASIOBool directProcess);
    static ASIOTime* bufferSwitchTimeInfo(ASIOTime* timeInfo, long index, ASIOBool processNow);
    static void sampleRateChanged(ASIOSampleRate sRate);
    static long asioMessage(long selector, long value, void* message, double* opt);

    // COM 참조 카운트
    std::atomic<ULONG> m_refCount{ 1 };

    // 설정값 임시 저장
    CLSID m_targetClsid = { 0 };
    std::wstring m_targetWasapiId;
    bool m_isVirtualMode = false;
};