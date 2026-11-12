#pragma once
#include <vector>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <memory>
#include <mutex>

const float CLIP_LIMIT = 1.5f;

// ---------------------------------------------------------------------------
// 리샘플러 커널: 카이저 윈도우드 싱크 폴리페이즈 테이블
//  - 소수 위치를 kPhases 단계로 나눠 가중치를 미리 계산 (행 = 단계, 열 = 탭)
//  - 다운샘플링에서는 차단 주파수를 출력 나이퀴스트로 낮추고 탭 수를 비율만큼 늘림
// ---------------------------------------------------------------------------
struct ResamplerKernel {
    static constexpr int kBaseTaps = 48;     // 같은 레이트 / 업샘플링 기준 탭 수
    static constexpr int kMaxTaps = 512;
    static constexpr int kPhases = 256;
    static constexpr double kBeta = 9.0;     // 카이저 윈도우 파라미터

    int taps = 0;       // 출력 1개에 쓰는 입력 샘플 수
    int half = 0;       // taps / 2
    double cutoff = 1.0;
    std::vector<float> table;   // (kPhases + 1) x taps

    explicit ResamplerKernel(double cutoffRatio) {
        const double kPi = 3.14159265358979323846;
        cutoff = (cutoffRatio > 0.0 && cutoffRatio < 1.0) ? cutoffRatio : 1.0;

        int n = (int)std::ceil(kBaseTaps / cutoff);
        if (n & 1) n++;
        if (n > kMaxTaps) n = kMaxTaps;
        taps = n;
        half = n / 2;
        table.assign((size_t)(kPhases + 1) * taps, 0.0f);

        const double i0Beta = BesselI0(kBeta);
        std::vector<double> row((size_t)taps);

        for (int p = 0; p <= kPhases; p++) {
            const double frac = (double)p / kPhases;
            double sum = 0.0;
            for (int j = 0; j < taps; j++) {
                // 구하려는 위치에서 j번째 입력 샘플까지의 거리
                const double t = (double)(j - half + 1) - frac;

                const double x = kPi * t * cutoff;
                const double sinc = (std::abs(x) < 1e-12) ? 1.0 : std::sin(x) / x;

                const double u = t / half;
                const double window = (std::abs(u) >= 1.0)
                    ? 0.0
                    : BesselI0(kBeta * std::sqrt(1.0 - u * u)) / i0Beta;

                row[j] = cutoff * sinc * window;
                sum += row[j];
            }
            // 행마다 합을 1로 맞춰 소수 위치에 따른 레벨 변동 제거
            float* dst = &table[(size_t)p * taps];
            for (int j = 0; j < taps; j++) dst[j] = (float)(row[j] / sum);
        }
    }

    // 같은 차단 주파수면 L/R이 테이블을 공유
    static std::shared_ptr<const ResamplerKernel> Acquire(double cutoffRatio) {
        static std::mutex mtx;
        static std::weak_ptr<const ResamplerKernel> cached;

        std::lock_guard<std::mutex> lock(mtx);
        const double c = (cutoffRatio > 0.0 && cutoffRatio < 1.0) ? cutoffRatio : 1.0;
        if (auto k = cached.lock(); k && k->cutoff == c) return k;

        auto k = std::make_shared<const ResamplerKernel>(c);
        cached = k;
        return k;
    }

private:
    // 0차 변형 베셀 함수 (카이저 윈도우용)
    static double BesselI0(double x) {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 64; k++) {
            const double h = x / (2.0 * k);
            term *= h * h;
            sum += term;
            if (term < 1e-14 * sum) break;
        }
        return sum;
    }
};

// 리샘플러
class Resampler {
public:
    void Setup(double inRate, double outRate) {
        if (inRate <= 0.0) inRate = 48000.0;
        if (outRate <= 0.0) outRate = inRate;
        m_ratio = inRate / outRate;
        m_readIndex = 0.0;

        m_kernel = ResamplerKernel::Acquire(outRate / inRate);
        m_history.assign((size_t)m_kernel->taps, 0.0f);
        Reserve(m_reservedInput);
    }

    // Process()에서 할당이 일어나지 않도록 한 번에 들어올 최대 입력 수를 미리 확보
    void Reserve(size_t maxInputFrames) {
        m_reservedInput = maxInputFrames;
        const size_t taps = m_kernel ? (size_t)m_kernel->taps : 0;
        if (m_work.size() < taps + maxInputFrames) m_work.resize(taps + maxInputFrames);
    }

    double GetReadIndex() const { return m_readIndex; }

    // 위상과 히스토리는 그대로 두고 비율만 변경
    void SetRatio(double ratio) { if (ratio > 0.0) m_ratio = ratio; }
    double GetRatio() const { return m_ratio; }

    // 보간 위치(index) 뒤로 필요한 입력 샘플 수
    int GetLookahead() const { return m_kernel ? m_kernel->half : 0; }
    int GetTaps() const { return m_kernel ? m_kernel->taps : 0; }

    size_t Process(const float* input, size_t inCount, float* output, size_t maxOutCount) {
        if (!output || !m_kernel) return 0;
        if (!input) inCount = 0;

        const ResamplerKernel& k = *m_kernel;
        const int taps = k.taps;
        const int half = k.half;

        // [이전 블록 끝 taps개 | 새 입력] 을 한 줄로 이어 붙임
        if (m_work.size() < (size_t)taps + inCount) m_work.resize((size_t)taps + inCount);
        float* work = m_work.data();
        memcpy(work, m_history.data(), (size_t)taps * sizeof(float));
        if (inCount > 0) memcpy(work + taps, input, inCount * sizeof(float));

        // 히스토리 범위를 벗어난 위치 복구 (정상 흐름에서는 발생하지 않음)
        const double minIndex = -(double)(half + 1);
        if (m_readIndex < minIndex) m_readIndex = minIndex;

        size_t outGenerated = 0;
        while (outGenerated < maxOutCount) {
            const double pos = m_readIndex;
            const long index = (long)std::floor(pos);

            // index + half 까지 실제 입력이 있어야 출력 (없으면 다음 블록에서 이어서 처리)
            if (index + half >= (long)inCount) break;

            // 소수 위치 -> 테이블 행 2개와 섞는 비율
            const double phasePos = (pos - (double)index) * ResamplerKernel::kPhases;
            int phase = (int)phasePos;
            if (phase >= ResamplerKernel::kPhases) phase = ResamplerKernel::kPhases - 1;
            const float blend = (float)(phasePos - (double)phase);

            const float* c0 = &k.table[(size_t)phase * taps];
            const float* c1 = c0 + taps;
            const float* x = work + taps + index - half + 1;

            float acc0 = 0.0f, acc1 = 0.0f;
            for (int j = 0; j < taps; j++) {
                acc0 += x[j] * c0[j];
                acc1 += x[j] * c1[j];
            }
            float sample = acc0 + blend * (acc1 - acc0);

            // 클리핑 방지
            if (sample > CLIP_LIMIT) sample = CLIP_LIMIT;
            if (sample < -CLIP_LIMIT) sample = -CLIP_LIMIT;

            output[outGenerated++] = sample;
            m_readIndex += m_ratio;
        }

        // 남은 인덱스 처리
        m_readIndex -= (double)inCount;

        // 히스토리 업데이트: 이어 붙인 줄의 마지막 taps개
        memcpy(m_history.data(), work + inCount, (size_t)taps * sizeof(float));

        return outGenerated;
    }

private:
    std::shared_ptr<const ResamplerKernel> m_kernel;
    std::vector<float> m_history;   // 이전 블록 
    std::vector<float> m_work;      // 히스토리 + 입력 버퍼
    size_t m_reservedInput = 0;

    double m_ratio = 1.0;
    double m_readIndex = 0.0;
};