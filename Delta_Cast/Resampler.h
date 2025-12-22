#pragma once
#include <vector>
#include <cmath>
#include <algorithm>

class Resampler {
public:
    void Setup(double inRate, double outRate) {
        if (outRate == 0.0) outRate = inRate;
        m_ratio = inRate / outRate;
        m_readIndex = 0.0;
    }

    size_t Process(const float* input, size_t inCount, float* output, size_t maxOutCount) {
        if (inCount == 0 || !output) return 0;

        // 비율이 1.0 이면 복사
        if (std::abs(m_ratio - 1.0) < 0.0001) {
            size_t copyCount = (inCount < maxOutCount) ? inCount : maxOutCount;
            memcpy(output, input, copyCount * sizeof(float));
            return copyCount;
        }
        size_t outGenerated = 0;
        // 리샘플링 루프
        // m_readIndex 현재 읽기 위치
        while (m_readIndex < inCount - 1 && outGenerated < maxOutCount) {
            size_t index = (size_t)m_readIndex;
            float frac = (float)(m_readIndex - index);

            float val1 = input[index];
            float val2 = input[index + 1];

            // 선형 보간 
            output[outGenerated++] = val1 * (1.0f - frac) + val2 * frac;

            m_readIndex += m_ratio;
        }

        // 남은 인덱스 처리
        m_readIndex -= inCount;
        if (m_readIndex < 0.0) m_readIndex = 0.0;

        return outGenerated;
    }

private:
    double m_ratio = 1.0;
    double m_readIndex = 0.0;
};