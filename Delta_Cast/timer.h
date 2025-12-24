#pragma once
#include <chrono>
#include <thread>
#include <immintrin.h>

class PrecisionClock {
public:
    static constexpr uint32_t TICKS_PER_SECOND = 8000; // 1틱 = 0.125ms

    using Clock = std::chrono::high_resolution_clock;
    using SystemClock = std::chrono::system_clock;
    using TimePoint = Clock::time_point;
    using Duration = std::chrono::nanoseconds;

    static inline TimePoint Now() {
        return Clock::now();
    }

    // 프로그램 시작 후 경과 시간 (초 단위)
    static double GetTimeSeconds() {
        static const auto start_time = Now();
        std::chrono::duration<double> elapsed = Now() - start_time;
        return elapsed.count();
    }

    static std::string GetDateString() {
        auto now = SystemClock::now();
        auto in_time_t = SystemClock::to_time_t(now);

        std::stringstream ss;
        struct tm buf;
        localtime_s(&buf, &in_time_t);  // 2025년 12월 21일 17시 30분 -> "251221-173045"

        ss << std::put_time(&buf, "%y%m%d-%H%M%S");
        return ss.str();
    }

    // [Convert] 초(Seconds) -> 틱(Ticks)
    static inline uint32_t SecondsToTicks(double seconds) {
        if (seconds < 0.0) return 0;
        return static_cast<uint32_t>(seconds * TICKS_PER_SECOND);
    }

    // [Convert] 틱(Ticks) -> 초(Seconds)
    static inline double TicksToSeconds(uint32_t ticks) {
        return static_cast<double>(ticks) / TICKS_PER_SECOND;
    }

    // 정밀 대기 (타겟 마이크로초 단위)
    static void WaitUntil(TimePoint target_time) {
        auto now = Now();
        while (now < target_time) {
            auto remaining = target_time - now;

            // 남은 시간이 2ms 이상일 때만 Sleep으로 CPU 양보 (1000Hz 미만)
            // 1000, 8000Hz 등 고주파 루프에서는 항상 남은시간이 2ms 이하 이므로 sleep 호출 되지않음
            if (remaining > std::chrono::milliseconds(2)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            else {
                _mm_pause();
            }
            now = Now(); // 시간 갱신
        }
    }
};

class Timer {
public:
    
    Timer() {
        // 초기화
        last_time_ = PrecisionClock::Now();
        delta_time_ = 0.0f;
    }

    // 매 프레임 호출
    void Tick() {
        auto current_time = PrecisionClock::Now();

        // 정밀한 시간 차이 (duration -> float 변환)
        std::chrono::duration<float> diff = current_time - last_time_;
        delta_time_ = diff.count();

        last_time_ = current_time;

        // [안전장치] 최대 0.05초로 제한
        if (delta_time_ > 0.05f) delta_time_ = 0.05f;
    }

    float GetDeltaTime() const {
        return delta_time_;
    }

private:
    PrecisionClock::TimePoint last_time_;
    float delta_time_;
};