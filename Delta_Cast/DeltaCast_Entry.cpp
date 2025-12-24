#include <Windows.h>
#include <string>
#include "DeltaCastFactory.h"
#include "DeltaCastGuids.h"

HMODULE g_hModule = nullptr;
LONG g_cDllRef = 0;

bool CreateRegistryKey(HKEY hKeyRoot, const std::wstring& keyPath, const std::wstring& valueName, const std::wstring& valueData) {
    HKEY hKey;
    LONG lRes = RegCreateKeyEx(hKeyRoot, keyPath.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL);
    if (lRes != ERROR_SUCCESS) return false;

    lRes = RegSetValueEx(hKey, valueName.c_str(), 0, REG_SZ, (const BYTE*)valueData.c_str(), (DWORD)(valueData.length() + 1) * sizeof(wchar_t));
    RegCloseKey(hKey);

    return (lRes == ERROR_SUCCESS);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) { g_hModule = hModule; DisableThreadLibraryCalls(hModule); }
    return TRUE;
}
STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (rclsid != CLSID_Delta_Cast) return CLASS_E_CLASSNOTAVAILABLE;
    CDeltaCastFactory* pFactory = new CDeltaCastFactory();
    if (!pFactory) return E_OUTOFMEMORY;
    HRESULT hr = pFactory->QueryInterface(riid, ppv);
    pFactory->Release();
    return hr;
}
STDAPI DllCanUnloadNow() { return (g_cDllRef == 0) ? S_OK : S_FALSE; }

// 레지스트리 등록
STDAPI DllRegisterServer() {
    wchar_t modulePath[MAX_PATH];
    if (GetModuleFileName(g_hModule, modulePath, MAX_PATH) == 0) return E_FAIL;

    LPOLESTR clsidStr;
    if (StringFromCLSID(CLSID_Delta_Cast, &clsidStr) != S_OK) return E_FAIL;
    std::wstring clsidWStr = clsidStr;
    CoTaskMemFree(clsidStr);

    // 키 이름 'Delta_Cast ASIO'
    std::wstring asioKey = L"SOFTWARE\\ASIO\\Delta_Cast ASIO";

    // 키 생성
    if (!CreateRegistryKey(HKEY_LOCAL_MACHINE, asioKey, L"CLSID", clsidWStr)) return E_FAIL;

    // Description도 키 이름과 토씨 하나 안 틀리고 똑같이 설정!
    if (!CreateRegistryKey(HKEY_LOCAL_MACHINE, asioKey, L"Description", L"Delta_Cast ASIO")) return E_FAIL;

    // COM 서버 등록
    std::wstring comKey = L"CLSID\\" + clsidWStr + L"\\InprocServer32";
    if (!CreateRegistryKey(HKEY_CLASSES_ROOT, comKey, L"", modulePath)) return E_FAIL;
    if (!CreateRegistryKey(HKEY_CLASSES_ROOT, comKey, L"ThreadingModel", L"Both")) return E_FAIL; // Both로 설정

    return S_OK;
}

STDAPI DllUnregisterServer() {
    RegDeleteKey(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO\\Delta_Cast ASIO");

    // GUID 변경시 확인 필요
    RegDeleteKey(HKEY_CLASSES_ROOT, L"CLSID\\{219E19EF-B9DC-4103-A2BB-90AC8D2C3BF0}\\InprocServer32");
    RegDeleteKey(HKEY_CLASSES_ROOT, L"CLSID\\{219E19EF-B9DC-4103-A2BB-90AC8D2C3BF0}");
    return S_OK;
}