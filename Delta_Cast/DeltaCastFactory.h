#pragma once
#include <Unknwn.h>
#include "DeltaCastDriver.h"

extern LONG g_cDllRef;

class CDeltaCastFactory : public IClassFactory {
public:
    CDeltaCastFactory() { InterlockedIncrement(&g_cDllRef); }
    ~CDeltaCastFactory() { InterlockedDecrement(&g_cDllRef); }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_refCount); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG ref = InterlockedDecrement(&m_refCount);
        if (ref == 0) delete this;
        return ref;
    }
    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        CDeltaCastDriver* pDriver = new CDeltaCastDriver();
        if (!pDriver) return E_OUTOFMEMORY;
        HRESULT hr = pDriver->QueryInterface(riid, ppv);
        pDriver->Release();
        return hr;
    }
    STDMETHODIMP LockServer(BOOL fLock) override {
        if (fLock) InterlockedIncrement(&g_cDllRef); else InterlockedDecrement(&g_cDllRef);
        return S_OK;
    }
private:
    LONG m_refCount = 1;
};