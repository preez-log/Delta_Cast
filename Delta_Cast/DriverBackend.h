#pragma once
#include <windows.h>
#ifndef MY_ASIO
#define MY_ASIO
#include <iasiodrv.h>
#endif
#include <vector>
#include <thread>
#include <atomic>
#include <string>

#include "timer.h"

class CDeltaCastDriver;

// 드라이버 동작
class IDriverBackend {
public:
    virtual ~IDriverBackend() = default;

    // --- 생명주기 ---
    virtual ASIOError Init(void* sysHandle) = 0;
    virtual ASIOError Start() = 0;
    virtual ASIOError Stop() = 0;

    // --- 설정 및 정보 ---
    virtual ASIOError GetBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) = 0;
    virtual ASIOError GetSampleRate(ASIOSampleRate* sampleRate) = 0;
    virtual ASIOError SetSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError GetChannels(long* numInputChannels, long* numOutputChannels) = 0;
    virtual ASIOError GetChannelInfo(ASIOChannelInfo* info) = 0;
    virtual ASIOError GetSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) = 0;
    virtual ASIOError OutputReady() = 0;

	// --- 지연 시간 ---
    virtual ASIOError GetLatencies(long* inputLatency, long* outputLatency) = 0;
    virtual ASIOError CanSampleRate(ASIOSampleRate sampleRate) = 0;
    virtual ASIOError Future(long selector, void* opt) { return ASE_NotPresent; }

    // --- 버퍼 관리 ---
    virtual ASIOError CreateBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) = 0;
    virtual ASIOError DisposeBuffers() = 0;

    // 기타
    virtual ASIOError ControlPanel() { return ASE_NotPresent; }
    virtual ASIOError SetClockSource(long reference) { return ASE_NotPresent; }
    virtual ASIOError GetClockSources(ASIOClockSource* clocks, long* numSources) { *numSources = 0; return ASE_OK; }
    virtual void GetErrorMessage(char* string) { string[0] = 0; }
};

// ---------------------------------------------------------------------------
// 가상 백엔드 (Virtual)
// ---------------------------------------------------------------------------
class VirtualBackend : public IDriverBackend {
public:
    VirtualBackend(CDeltaCastDriver* owner, double sampleRate);
    virtual ~VirtualBackend();

    ASIOError Init(void* sysHandle) override { return ASE_OK; }

    ASIOError Start() override;
    ASIOError Stop() override;

    ASIOError GetBufferSize(long* min, long* max, long* pref, long* gran) override {
        *min = 128; *max = 2048; *pref = 256; *gran = -1;
        return ASE_OK;
    }

    ASIOError GetSampleRate(ASIOSampleRate* sampleRate) override {
        *sampleRate = m_sampleRate; return ASE_OK;
    }
    ASIOError SetSampleRate(ASIOSampleRate sampleRate) override {
        m_sampleRate = sampleRate; return ASE_OK;
    }

    ASIOError GetChannels(long* in, long* out) override {
        *in = 0; *out = 2; return ASE_OK;
    }

    ASIOError GetChannelInfo(ASIOChannelInfo* info) override {
        if (info->channel < 0 || info->channel >= 2) return ASE_InvalidParameter;
        info->type = ASIOSTFloat32LSB;
        info->isActive = ASIOTrue;
        info->isInput = ASIOFalse;
        sprintf_s(info->name, 32, "Delta Virtual %d", info->channel + 1);
        return ASE_OK;
    }

    ASIOError GetSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) override;
    ASIOError CreateBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) override;
    ASIOError DisposeBuffers() override;
    ASIOError OutputReady() override { return ASE_OK; }
    ASIOError SetClockSource(long reference) override { return ASE_OK; }
    ASIOError GetClockSources(ASIOClockSource* clocks, long* numSources) override {
        *numSources = 1;
        clocks[0].index = 0;
        clocks[0].isCurrentSource = ASIOTrue;
        strcpy_s(clocks[0].name, 32, "Internal Virtual Clock");
        return ASE_OK;
    }
    ASIOError GetLatencies(long* inputLatency, long* outputLatency) override {
        *inputLatency = 0;
        *outputLatency = m_bufferSize; // 출력 레이턴시는 버퍼 크기
        return ASE_OK;
    }
    ASIOError CanSampleRate(ASIOSampleRate sampleRate) override {
        if (sampleRate == 44100.0 || sampleRate == 48000.0 || sampleRate == 88200.0 || 
            sampleRate == 96000.0 || sampleRate == 176400.0 || sampleRate == 192000.0 ||
            sampleRate == 352800.0 || sampleRate == 384000.0)
            return ASE_OK;
        return ASE_NoClock;
    }
    ASIOError Future(long selector, void* opt) override { return ASE_NotPresent; }

private:
    void VirtualClockLoop(); // 가상 클럭 루프

    CDeltaCastDriver* m_owner = nullptr;
    double m_sampleRate = 48000.0;
    long m_bufferSize = 0;

    // 가상 자원
    std::vector<std::vector<float>> m_buffers;
    std::thread m_thread;
    std::atomic<bool> m_running{ false };
    std::atomic<int64_t> m_samplePos{ 0 };
};

// ---------------------------------------------------------------------------
// 프록시 백엔드 (Real Hardware)
// ---------------------------------------------------------------------------
class ProxyBackend : public IDriverBackend {
public:
    ProxyBackend(IASIO* backend) : m_backend(backend) {}
    ~ProxyBackend() { if (m_backend) { m_backend->stop(); m_backend->Release(); } }

    ASIOError Init(void* sysHandle) override { return m_backend->init(sysHandle) ? ASE_OK : ASE_NotPresent; }
    ASIOError Start() override { return m_backend->start(); }
    ASIOError Stop() override { return m_backend->stop(); }

    ASIOError GetBufferSize(long* min, long* max, long* pref, long* gran) override {
        return m_backend->getBufferSize(min, max, pref, gran);
    }
    ASIOError GetSampleRate(ASIOSampleRate* rate) override { return m_backend->getSampleRate(rate); }
    ASIOError SetSampleRate(ASIOSampleRate rate) override { return m_backend->setSampleRate(rate); }
    ASIOError GetChannels(long* in, long* out) override { return m_backend->getChannels(in, out); }
    ASIOError GetChannelInfo(ASIOChannelInfo* info) override { return m_backend->getChannelInfo(info); }
    ASIOError GetSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) override { return m_backend->getSamplePosition(sPos, tStamp); }

    ASIOError CreateBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) override {
        return m_backend->createBuffers(bufferInfos, numChannels, bufferSize, callbacks);
    }
    ASIOError DisposeBuffers() override { return m_backend->disposeBuffers(); }
    ASIOError OutputReady() override { return m_backend->outputReady(); }
    ASIOError ControlPanel() override { return m_backend->controlPanel(); }
    ASIOError SetClockSource(long ref) override { return m_backend->setClockSource(ref); }
    ASIOError GetLatencies(long* inputLatency, long* outputLatency) override {
        return m_backend->getLatencies(inputLatency, outputLatency);
    }
    ASIOError CanSampleRate(ASIOSampleRate sampleRate) override {
        return m_backend->canSampleRate(sampleRate);
    }
    ASIOError Future(long selector, void* opt) override {
        return m_backend->future(selector, opt);
    }

private:
    IASIO* m_backend = nullptr;
};