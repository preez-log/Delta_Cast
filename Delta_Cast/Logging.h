#pragma once
// 공용 로깅 유틸리티.
// - LogError : 릴리즈 빌드에서 출력 - 실패/경고
// - LogInfo  : 릴리즈 빌드에서 출력 - 초기화 결과 등 정보성 이벤트
// - DebugLog : _DEBUG 빌드에서만 출력 - 상세 추적
// ASIO 콜백 안에서는 호출 금지

#include <cstdarg>
#include <cstdio>
#include <windows.h>
#include <string>

namespace dc_log_detail {
    inline void Write(const char* level, const char* fmt, va_list args) {
        char buf[2048];
        int n = snprintf(buf, sizeof(buf), "[Delta_Cast]%s ", level);
        if (n < 0) return;
        if (n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;

        int m = vsnprintf(buf + n, sizeof(buf) - n - 1, fmt, args);
        if (m < 0) m = 0;
        size_t len = (size_t)n + (size_t)m;
        if (len > sizeof(buf) - 2) len = sizeof(buf) - 2;   // 잘린 경우

        buf[len] = '\n';
        buf[len + 1] = '\0';

        wchar_t wbuf[2048];
        if (MultiByteToWideChar(CP_UTF8, 0, buf, -1, wbuf, 2048) > 0)
            OutputDebugStringW(wbuf);
        else
            OutputDebugStringA(buf);

    }
}

inline std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// 에러/중요 이벤트 로그
inline void LogError(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    dc_log_detail::Write(" [ERR]", fmt, args);
    va_end(args);
}

// 일반 정보 로그
inline void LogInfo(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    dc_log_detail::Write(" [INFO]", fmt, args);
    va_end(args);
}

// 디버그 빌드 전용 상세 로그
inline void DebugLog(const char* fmt, ...) {
#ifdef _DEBUG
    va_list args; va_start(args, fmt);
    dc_log_detail::Write(" [DBG]", fmt, args);
    va_end(args);
#else
    (void)fmt;
#endif
}