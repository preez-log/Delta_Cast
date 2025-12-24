#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <iostream>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <shellapi.h>
#include "Resource.h"

#pragma comment(lib, "comctl32.lib")

// 컨트롤 ID 
#define IDC_COMBO_ASIO   101 // ASIO 콤보박스
#define IDC_COMBO_WASAPI 102 // WASAPI 콤보박스
#define IDC_BTN_SAVE     103 // 저장 버튼
#define IDC_BTN_INIT     104 // 등록 버튼
#define IDC_BTN_UNINIT   105 // 제거 버튼
#define IDC_STATUS_TEXT  106 // 상태

// 드라이버 정보 구조체
struct DeviceInfo {
    std::wstring name;
    std::wstring id;
};

// 전역 변수
std::vector<DeviceInfo> g_asioList;
std::vector<DeviceInfo> g_wasapiList;
HWND hComboAsio, hComboWasapi, hBtnSave, hBtnInit, hBtnUnInit, hStatus;

// 레지스트리에서 ASIO 드라이버 목록 스캔
void ScanAsioDrivers() {
    g_asioList.clear();

    g_asioList.push_back({ L"[Virtual] Delta_Cast ASIO", L"Virtual" });
    HKEY hKey;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return;
    }

    WCHAR subKeyName[256];
    DWORD index = 0;
    DWORD len = 256;

    while (RegEnumKeyEx(hKey, index++, subKeyName, &len, NULL, NULL, NULL, NULL) == ERROR_SUCCESS) {
        HKEY hSubKey;
        if (RegOpenKeyEx(hKey, subKeyName, 0, KEY_READ, &hSubKey) == ERROR_SUCCESS) {
            WCHAR clsid[64] = { 0 };
            WCHAR description[256] = { 0 };
            DWORD size = sizeof(clsid);
            DWORD type = REG_SZ;

            // CLSID
            RegQueryValueEx(hSubKey, L"CLSID", NULL, &type, (LPBYTE)clsid, &size);

            // Description
            size = sizeof(description);
            if (RegQueryValueEx(hSubKey, L"Description", NULL, &type, (LPBYTE)description, &size) != ERROR_SUCCESS) {
                // Description이 없으면 키 이름을 사용
                wcscpy_s(description, subKeyName);
            }

            // Delta_Cast 자기 자신 제외
            if (wcscmp(description, L"Delta_Cast ASIO") != 0) {
                g_asioList.push_back({ description, clsid });
            }
            RegCloseKey(hSubKey);
        }
        len = 256;
    }
    RegCloseKey(hKey);
}

// WASAPI 장치 스캔
void ScanWasapiDevices() {
    g_wasapiList.clear();

    // "Default Device" 옵션 추가
    g_wasapiList.push_back({ L"Default Windows Output", L"" });

    IMMDeviceEnumerator* pEnumerator = NULL;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&pEnumerator);
    if (!pEnumerator) return;

    IMMDeviceCollection* pCollection = NULL;
    pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    if (pCollection) {
        UINT count;
        pCollection->GetCount(&count);
        for (UINT i = 0; i < count; i++) {
            IMMDevice* pDevice = NULL;
            pCollection->Item(i, &pDevice);
            if (pDevice) {
                LPWSTR pwszID = NULL;
                pDevice->GetId(&pwszID);
                IPropertyStore* pProps = NULL;
                pDevice->OpenPropertyStore(STGM_READ, &pProps);
                PROPVARIANT varName;
                PropVariantInit(&varName);
                if (pProps) {
                    pProps->GetValue(PKEY_Device_FriendlyName, &varName);
                    if (pwszID && varName.pwszVal) {
                        g_wasapiList.push_back({ varName.pwszVal, pwszID });
                    }
                    PropVariantClear(&varName);
                    pProps->Release();
                }
                CoTaskMemFree(pwszID);
                pDevice->Release();
            }
        }
        pCollection->Release();
    }
    pEnumerator->Release();
}

// INI 파일에 저장
bool SaveConfig(const std::wstring& asioClsid, const std::wstring& wasapiId) {
    WCHAR path[MAX_PATH];
    GetModuleFileName(NULL, path, MAX_PATH);
    std::wstring configPath = path;
    configPath = configPath.substr(0, configPath.find_last_of(L"\\/") + 1) + L"Delta_Cast.ini";
    bool b1 = WritePrivateProfileString(L"Settings", L"TargetDriverCLSID", asioClsid.c_str(), configPath.c_str());
    bool b2 = WritePrivateProfileString(L"Settings", L"TargetWasapiID", wasapiId.c_str(), configPath.c_str());
    return b1 && b2;
}

// 드라이버 등록 / 해제
void RunDriverCommand(HWND hWnd, bool install) {
    WCHAR path[MAX_PATH];
    GetModuleFileName(NULL, path, MAX_PATH);
    std::wstring dllPath = path;
    // exe가 있는 폴더에서 dll 찾기
    dllPath = dllPath.substr(0, dllPath.find_last_of(L"\\/") + 1) + L"Delta_Cast.dll";

    // 파일 존재 확인
    if (GetFileAttributes(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBox(hWnd, L"Delta_Cast.dll not found!", L"Error", MB_ICONERROR);
        return;
    }

    // regsvr32 명령어 실행
    std::wstring params = install ? L"/s \"" : L"/u /s \"";
    params += dllPath + L"\"";

    // ShellExecute 실행
    HINSTANCE hRes = ShellExecute(NULL, L"open", L"regsvr32.exe", params.c_str(), NULL, SW_HIDE);

    // 반환값이 32보다 크면 성공
    if ((intptr_t)hRes > 32) {
        if (install) {
            SetWindowText(hStatus, L"Status: Driver Registered Successfully!");
            MessageBox(hWnd, L"Driver Initialized & Registered!\nSelect 'Delta_Cast ASIO' in your game.", L"Success", MB_ICONINFORMATION);
        }
        else {
            SetWindowText(hStatus, L"Status: Driver Unregistered.");
            MessageBox(hWnd, L"Driver Removed from System.", L"Info", MB_ICONINFORMATION);
        }
    }
    else {
        MessageBox(hWnd, L"Failed to execute regsvr32.", L"Error", MB_ICONERROR);
    }
}

// 윈도우 프로시저
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        {
            // 설명 텍스트
            CreateWindow(L"STATIC", L"Select ASIO hardware to use:",
                WS_CHILD | WS_VISIBLE,
                20, 20, 300, 20, hWnd, NULL, ((LPCREATESTRUCT)lParam)->hInstance, NULL);

            // 폰트 설정 (기본 시스템 폰트)
            HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

            // ASIO (input)
            CreateWindow(L"STATIC", L"1. Select Real ASIO Hardware:", WS_CHILD | WS_VISIBLE, 20, 20, 300, 20, hWnd, NULL, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            hComboAsio = CreateWindow(L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 20, 45, 340, 200, hWnd, (HMENU)IDC_COMBO_ASIO, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            SendMessage(hComboAsio, WM_SETFONT, (WPARAM)hFont, 0);

            // WASAPI (output)
            CreateWindow(L"STATIC", L"2. Select Loopback Output (Windows):", WS_CHILD | WS_VISIBLE, 20, 80, 300, 20, hWnd, NULL, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            hComboWasapi = CreateWindow(L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 20, 105, 340, 200, hWnd, (HMENU)IDC_COMBO_WASAPI, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            SendMessage(hComboWasapi, WM_SETFONT, (WPARAM)hFont, 0);

            // 저장 버튼
            hBtnSave = CreateWindow(L"BUTTON", L"Save Config", 
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 
                20, 150, 100, 30, hWnd, 
                (HMENU)IDC_BTN_SAVE, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            SendMessage(hBtnSave, WM_SETFONT, (WPARAM)hFont, 0);

            // Init (Register) 버튼 - 초기에는 비활성화
            hBtnInit = CreateWindow(L"BUTTON", L"Init Driver", 
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED, 
                130, 150, 100, 30, hWnd, 
                (HMENU)IDC_BTN_INIT, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            SendMessage(hBtnInit, WM_SETFONT, (WPARAM)hFont, 0);

            // Uninstall 버튼
            hBtnUnInit = CreateWindow(L"BUTTON", L"Uninstall", 
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 
                240, 150, 80, 30, hWnd, 
                (HMENU)IDC_BTN_UNINIT, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            SendMessage(hBtnUnInit, WM_SETFONT, (WPARAM)hFont, 0);

            hStatus = CreateWindow(L"STATIC", L"Status: Waiting for configuration...", 
                WS_CHILD | WS_VISIBLE, 
                20, 190, 340, 20, hWnd, 
                (HMENU)IDC_STATUS_TEXT, ((LPCREATESTRUCT)lParam)->hInstance, NULL);
            SendMessage(hStatus, WM_SETFONT, (WPARAM)hFont, 0);

            // 드라이버 스캔
            ScanAsioDrivers();
            ScanWasapiDevices();

            // 콤보박스
            for (const auto& d : g_asioList) SendMessage(hComboAsio, CB_ADDSTRING, 0, (LPARAM)d.name.c_str());
            for (const auto& d : g_wasapiList) SendMessage(hComboWasapi, CB_ADDSTRING, 0, (LPARAM)d.name.c_str());

            // 첫 번째 항목 선택
            SendMessage(hComboAsio, CB_SETCURSEL, 0, 0);
            SendMessage(hComboWasapi, CB_SETCURSEL, 0, 0);
        }
        break;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_BTN_SAVE) {
            int idxAsio = (int)SendMessage(hComboAsio, CB_GETCURSEL, 0, 0);
            int idxWasapi = (int)SendMessage(hComboWasapi, CB_GETCURSEL, 0, 0);

            if (idxAsio >= 0 && idxAsio < g_asioList.size() && idxWasapi >= 0 && idxWasapi < g_wasapiList.size()) {
                if (SaveConfig(g_asioList[idxAsio].id, g_wasapiList[idxWasapi].id)) {
                    SetWindowText(hStatus, L"Status: Configuration Saved. Press 'Init'!");
                    // 저장 성공 > Init 버튼 활성화
                    EnableWindow(hBtnInit, TRUE);
                }
                else {
                    SetWindowText(hStatus, L"Failed to save INI.");
                }
            }
        }
        else if (LOWORD(wParam) == IDC_BTN_INIT) { // Init 버튼
            RunDriverCommand(hWnd, true); // 설치 (Register)
        }
        else if (LOWORD(wParam) == IDC_BTN_UNINIT) { // Uninstall 버튼
            RunDriverCommand(hWnd, false); // 제거 (Unregister)
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// 메인 함수
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    CoInitialize(NULL);
    WNDCLASSEXW wcex = { 
        sizeof(WNDCLASSEX), 
        CS_HREDRAW | CS_VREDRAW, WndProc, 0, 0, hInstance, 
        NULL, LoadCursor(NULL, IDC_ARROW), (HBRUSH)(COLOR_WINDOW + 1), 
        NULL, L"ConfigCls", LoadIcon(hInstance, MAKEINTRESOURCE(IDI_SMALL)) };

    RegisterClassExW(&wcex);

    // 윈도우 생성
    HWND hWnd = CreateWindowW(L"ConfigCls", L"Delta_Cast Config V1.1.0", 
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, 
        0, 400, 260, NULL, NULL, hInstance, NULL);
    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}