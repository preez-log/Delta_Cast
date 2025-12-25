#pragma once
#include <vector>
#include <cmath>
#include <algorithm>

const float HEADROOM_GAIN = 0.98f;
const float CLIP_LIMIT = 1.5f;

class Resampler {
public:
    void Setup(double inRate, double outRate) {
        if (outRate == 0.0) outRate = inRate;
        m_ratio = inRate / outRate;
        m_readIndex = 0.0;
		// 히스토리 초기화
        for (int i = 0; i < 4; i++) m_history[i] = 0.0f;
    }

    inline float CubicInterp(float y0, float y1, float y2, float y3, float t) {
        float a0, a1, a2, a3;
        float t2 = t * t;

        a0 = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
        a1 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        a2 = -0.5f * y0 + 0.5f * y2;
        a3 = y1;

        return a0 * t * t2 + a1 * t2 + a2 * t + a3;
    }

    double GetReadIndex() const { return m_readIndex; }

    size_t Process(const float* input, size_t inCount, float* output, size_t maxOutCount) {
        if (inCount == 0 || !output) return 0;

        // 비율이 1.0 이면 단순 복사
        if (std::abs(m_ratio - 1.0) < 0.0001) {
            size_t copyCount = (inCount < maxOutCount) ? inCount : maxOutCount;
            memcpy(output, input, copyCount * sizeof(float));
            UpdateHistory(input, inCount);
            return copyCount;
        }
        size_t outGenerated = 0;

        // 리샘플링 루프
        while (outGenerated < maxOutCount) {
            double pos = m_readIndex;
            long index = (long)floor(pos);
            float frac = (float)(pos - index);

			// 인덱스 범위 체크
            if (index < -4) break;
            if (index >= (long)inCount) { break; }

            float p0, p1, p2, p3;
            // --- P0 (index - 1) ---
            long idx0 = index - 1;
            if (idx0 >= 0) p0 = input[idx0];
            else           p0 = m_history[4 + idx0];

            // --- P1 (index) ---
            if (index >= 0) p1 = input[index];
            else            p1 = m_history[4 + index];

            // --- P2 (index + 1) ---
            long idx2 = index + 1;
            if (idx2 >= 0) {
                if (idx2 < (long)inCount) p2 = input[idx2];
                else break; // 데이터 부족
            }
            else {
                p2 = m_history[4 + idx2];
            }

            // --- P3 (index + 2) ---
            long idx3 = index + 2;
            if (idx3 >= 0) {
                if (idx3 < (long)inCount) p3 = input[idx3];
                else p3 = p2; // 끝부분 복제
            }
            else {
                p3 = m_history[4 + idx3];
            }

            // 3차 보간 계산
            float sample = CubicInterp(p0, p1, p2, p3, frac);
			// 약간의 헤드룸 적용
            sample *= HEADROOM_GAIN;

            // 클리핑 방지
            if (sample > CLIP_LIMIT) sample = CLIP_LIMIT;
            if (sample < -CLIP_LIMIT) sample = -CLIP_LIMIT;

            output[outGenerated++] = sample;

            m_readIndex += m_ratio;
        }

        // 남은 인덱스 처리
        m_readIndex -= inCount;
		// 히스토리 업데이트
        UpdateHistory(input, inCount);

        return outGenerated;
    }

private:
    void UpdateHistory(const float* input, size_t inCount) {
        if (inCount >= 4) {
            // 끝에서 4개 가져옴
            memcpy(m_history, &input[inCount - 4], 4 * sizeof(float));
        }
        else {
            for (size_t i = 0; i < inCount; i++) {
                // 하나씩 밀기
                m_history[0] = m_history[1];
                m_history[1] = m_history[2];
                m_history[2] = m_history[3];
                m_history[3] = input[i];
            }
        }
    }

    double m_ratio = 1.0;
    double m_readIndex = 0.0;
    float m_history[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
};