#pragma once
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// 클럭 드리프트 제어기
//  - ASIO 클럭과 송출 장치 클럭의 미세한 속도 차이를 리샘플 비율로 흡수
//  - 입력: 팝 직후 링버퍼 채움량 (입력 프레임 단위)
//  - 출력: 리샘플 비율 보정값. 실제 비율 = 명목 비율 * (1 + 보정)
//          예) +20e-6 = 입력을 20ppm 더 빨리 소비
// ---------------------------------------------------------------------------
class DriftController {
public:
    static constexpr double kLoopTimeSec = 10.0;        // 비례 응답 시정수
    static constexpr double kIntegralTimeSec = 100.0;   // 적분 시정수
    static constexpr double kFilterTimeSec = 1.0;       // 채움량 평활 시정수
    static constexpr double kMaxCorrection = 1000e-6;   // 보정 한계 ±1000ppm
    static constexpr double kResyncExcessSec = 0.030;   // 목표보다 이만큼 넘치면 한 번에 버림

    // 새 세션, 모든 상태 초기화
    void Setup(double inputRate, double targetFrames) {
        m_rate = (inputRate > 0.0) ? inputRate : 48000.0;
        m_target = targetFrames;
        m_integral = 0.0;
        Reset();
    }

    // 재버퍼링, 재동기화 드리프트 학습값 유지
    void Reset() {
        m_primed = false;
        m_avgFill = m_target;
        m_correction = m_integral;
    }

    // fillFrames: 팝 직후 채움량, dtSeconds: 이전 호출 이후 지난 시간
    double Update(double fillFrames, double dtSeconds) {
        if (!m_primed) {
            m_avgFill = fillFrames;
            m_primed = true;
        }
        else {
            const double a = 1.0 - std::exp(-dtSeconds / kFilterTimeSec);
            m_avgFill += a * (fillFrames - m_avgFill);
        }

        const double errorSec = (m_avgFill - m_target) / m_rate;
        const double p = errorSec / kLoopTimeSec;

        // 한계에 걸린 상태에서 같은 방향으로는 더 쌓지 않음
        const double unclamped = p + m_integral;
        const bool saturated = (unclamped > kMaxCorrection && p > 0.0)
            || (unclamped < -kMaxCorrection && p < 0.0);
        if (!saturated) m_integral += p * dtSeconds / kIntegralTimeSec;
        m_integral = std::clamp(m_integral, -kMaxCorrection, kMaxCorrection);

        m_correction = std::clamp(p + m_integral, -kMaxCorrection, kMaxCorrection);
        return m_correction;
    }

    // 목표를 크게 넘었으면 버려야 할 프레임 수, 아니면 0
    double ExcessFrames(double fillFrames) const {
        const double limit = m_target + kResyncExcessSec * m_rate;
        return (fillFrames > limit) ? (fillFrames - m_target) : 0.0;
    }

    double GetCorrection() const { return m_correction; }
    double GetLearnedDrift() const { return m_integral; }
    double GetAverageFill() const { return m_avgFill; }
    double GetTarget() const { return m_target; }

private:
    double m_rate = 48000.0;
    double m_target = 0.0;
    double m_avgFill = 0.0;
    double m_correction = 0.0;
    double m_integral = 0.0;
    bool m_primed = false;
};