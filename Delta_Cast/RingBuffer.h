#pragma once
#include <atomic>
#include <vector>

// ---------------------------------------------------------------------------
// Lock-Free Ring Buffer
// ---------------------------------------------------------------------------
class ByteRingBuffer {
public:
    explicit ByteRingBuffer(size_t sizeBytes = 65536) // 기본 4MB
        : m_size(sizeBytes), m_mask(sizeBytes - 1), m_buffer(sizeBytes) {
        // 읽기/쓰기 포인터 초기화
        m_writeIndex.store(0, std::memory_order_relaxed);
        m_readIndex.store(0, std::memory_order_relaxed);
    }

    // 데이터 밀어넣기
    void Push(const void* input, size_t numBytes) {
        size_t writeIdx = m_writeIndex.load(std::memory_order_acquire);
        size_t readIdx = m_readIndex.load(std::memory_order_acquire);

        size_t avail = m_size - (writeIdx - readIdx);
        if (avail < numBytes) {
            return;
        }

        const uint8_t* pIn = static_cast<const uint8_t*>(input);

        // 링버퍼 끝부분 처리
        size_t offset = writeIdx & m_mask;
        size_t toEnd = m_size - offset;

        if (numBytes <= toEnd) {
            memcpy(&m_buffer[offset], pIn, numBytes);
        }
        else {
            memcpy(&m_buffer[offset], pIn, toEnd);
            memcpy(&m_buffer[0], pIn + toEnd, numBytes - toEnd);
        }

        // 인덱스 업데이트
        m_writeIndex.store(writeIdx + numBytes, std::memory_order_release);
    }

    size_t GetAvailableRead() const {
        size_t w = m_writeIndex.load(std::memory_order_acquire);
        size_t r = m_readIndex.load(std::memory_order_acquire);
        return w - r;
    }

    // 데이터 꺼내오기
    size_t Pop(void* output, size_t numBytes) {
        size_t writeIdx = m_writeIndex.load(std::memory_order_acquire);
        size_t readIdx = m_readIndex.load(std::memory_order_acquire);

        size_t availableData = writeIdx - readIdx;
        if (availableData == 0) return 0; // 데이터 없음

        size_t toRead = (numBytes > availableData) ? availableData : numBytes;
        uint8_t* pOut = static_cast<uint8_t*>(output);

        size_t offset = readIdx & m_mask;
        size_t toEnd = m_size - offset;

        if (toRead <= toEnd) {
            memcpy(pOut, &m_buffer[offset], toRead);
        }
        else {
            memcpy(pOut, &m_buffer[offset], toEnd);
            memcpy(pOut + toEnd, &m_buffer[0], toRead - toEnd);
        }

        // 인덱스 업데이트
        m_readIndex.store(readIdx + toRead, std::memory_order_release);
        return toRead;
    }

    // 현재 쓸 수 있는 공간
    size_t GetAvailableWrite() const {
        size_t w = m_writeIndex.load(std::memory_order_acquire);
        size_t r = m_readIndex.load(std::memory_order_acquire);
        size_t used = w - r;
        return (used < m_size) ? (m_size - used) : 0;
    }

    // 현재 채워진 데이터 
    size_t GetFillSize() const {
        size_t w = m_writeIndex.load(std::memory_order_acquire);
        size_t r = m_readIndex.load(std::memory_order_acquire);
        return w - r;
    }

private:
    std::vector<uint8_t> m_buffer;
    size_t m_size;
    size_t m_mask;
    alignas(64) std::atomic<size_t> m_writeIndex;
    alignas(64) std::atomic<size_t> m_readIndex;
    char _padding[64];
};