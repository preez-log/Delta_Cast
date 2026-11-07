#pragma once
#include <chrono>
#include <thread>
#include <emmintrin.h>
#include <Windows.h>
#pragma comment(lib, "winmm.lib")

struct TimerResolutionSetter {
    TimerResolutionSetter() { timeBeginPeriod(1); }
    ~TimerResolutionSetter() { timeEndPeriod(1); }
};

class PrecisionClock {
public:

    using Clock = std::chrono::high_resolution_clock;
    using TimePoint = Clock::time_point;

    static inline TimePoint Now() {
        return Clock::now();
    }

    // 하이브리드 대기: 2ms 이상 남으면 sleep, 그 이하는 spin
    static void WaitUntil(TimePoint target) {
        auto now = Clock::now();
        while (now < target) {
            if (target - now > std::chrono::milliseconds(2))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            else
                _mm_pause();
            now = Clock::now();
        }
    }
};