#pragma once
#include <atomic>
#include <vector>

// ---------------------------------------------------------------------------
// Lock-Free Ring Buffer
// ---------------------------------------------------------------------------
template <typename T>
class LockFreeRingBuffer {
public:
    explicit LockFreeRingBuffer(size_t size = 65536)
        : m_size(size), m_mask(size - 1), m_buffer(size) {
        // 읽기/쓰기 포인터
        m_writeIndex.store(0, std::memory_order_relaxed);
        m_readIndex.store(0, std::memory_order_relaxed);
    }

    // 데이터 밀어넣기
    void Push(const T* data, size_t count) {
        size_t writeIdx = m_writeIndex.load(std::memory_order_acquire);
        size_t readIdx = m_readIndex.load(std::memory_order_acquire);

        // 여유 공간 확인
        size_t avail = m_size - (writeIdx - readIdx);
        if (avail < count) return;

        for (size_t i = 0; i < count; ++i) {
            // 비트 연산
            m_buffer[(writeIdx + i) & m_mask] = data[i];
        }

        // 인덱스 업데이트
        m_writeIndex.store(writeIdx + count, std::memory_order_release);
    }

    size_t GetAvailableRead() const {
        size_t w = m_writeIndex.load(std::memory_order_acquire);
        size_t r = m_readIndex.load(std::memory_order_acquire);
        return w - r;
    }

    // 데이터 꺼내오기
    size_t Pop(T* output, size_t count) {
        size_t writeIdx = m_writeIndex.load(std::memory_order_acquire);
        size_t readIdx = m_readIndex.load(std::memory_order_acquire);

        size_t availableData = writeIdx - readIdx;
        if (availableData == 0) return 0; // 데이터 없음

        size_t toRead = (count > availableData) ? availableData : count;

        for (size_t i = 0; i < toRead; ++i) {
            output[i] = m_buffer[(readIdx + i) & m_mask];
        }

        // 인덱스 업데이트
        m_readIndex.store(readIdx + toRead, std::memory_order_release);
        return toRead;
    }

private:
    std::vector<T> m_buffer;
    size_t m_size;
    size_t m_mask;
    std::atomic<size_t> m_writeIndex;
    std::atomic<size_t> m_readIndex;
};