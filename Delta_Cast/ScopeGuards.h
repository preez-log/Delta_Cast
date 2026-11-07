#pragma once
// RAII 래퍼 모음

#include <windows.h>
#include <avrt.h>
#include <objbase.h>

// COM 라이브러리 초기화/해제 (CoInitializeEx / CoUninitialize).
class ComInitGuard {
public:
    explicit ComInitGuard(DWORD coInit = COINIT_MULTITHREADED) {
        m_hr = CoInitializeEx(nullptr, coInit);
    }
    ~ComInitGuard() {
        if (SUCCEEDED(m_hr)) CoUninitialize();
    }
    HRESULT hr() const { return m_hr; }

    ComInitGuard(const ComInitGuard&) = delete;
    ComInitGuard& operator=(const ComInitGuard&) = delete;
private:
    HRESULT m_hr;
};

// 일반 커널 핸들
class HandleGuard {
public:
    HandleGuard() = default;
    explicit HandleGuard(HANDLE h) : m_h(h) {}
    ~HandleGuard() { reset(); }

    void reset(HANDLE h = nullptr) {
        if (m_h && m_h != INVALID_HANDLE_VALUE) CloseHandle(m_h);
        m_h = h;
    }
    HANDLE get() const { return m_h; }
    bool valid() const { return m_h && m_h != INVALID_HANDLE_VALUE; }

    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
private:
    HANDLE m_h = nullptr;
};

// MMCSS 작업 등록
class MmcssGuard {
public:
    MmcssGuard() = default;
    explicit MmcssGuard(HANDLE h) : m_h(h) {}
    ~MmcssGuard() { if (m_h) AvRevertMmThreadCharacteristics(m_h); }

    HANDLE get() const { return m_h; }

    MmcssGuard(const MmcssGuard&) = delete;
    MmcssGuard& operator=(const MmcssGuard&) = delete;
private:
    HANDLE m_h = nullptr;
};

// CoTaskMemAlloc
template <class T>
class CoMemGuard {
public:
    CoMemGuard() = default;
    explicit CoMemGuard(T* p) : m_p(p) {}
    ~CoMemGuard() { if (m_p) CoTaskMemFree(m_p); }

    T** put() { return &m_p; }   //API 전달용
    T* get() const { return m_p; }
    T* operator->() const { return m_p; }

    CoMemGuard(const CoMemGuard&) = delete;
    CoMemGuard& operator=(const CoMemGuard&) = delete;
private:
    T* m_p = nullptr;
};
