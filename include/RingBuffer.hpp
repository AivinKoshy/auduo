#pragma once

#include <vector>
#include <atomic>
#include <cstdint>
#include <algorithm>
#include <cmath>

namespace Auduo {

// Single-Producer Single-Consumer (SPSC) lock-free ring buffer for interleaved 32-bit float audio
class RingBuffer {
public:
    explicit RingBuffer(size_t capacityFrames, uint32_t channels = 2)
        : m_channels(channels)
    {
        // Round capacity up to power of 2 for fast bitmask wrapping
        m_capacityFrames = 1;
        while (m_capacityFrames < capacityFrames) {
            m_capacityFrames <<= 1;
        }
        m_mask = m_capacityFrames - 1;
        m_buffer.resize(m_capacityFrames * m_channels, 0.0f);
        m_writeIndex.store(0, std::memory_order_relaxed);
        m_readIndex.store(0, std::memory_order_relaxed);
    }

    // Write samples (returns number of frames actually written)
    size_t Write(const float* source, size_t frames) {
        if (!source || frames == 0) return 0;

        const size_t writeIdx = m_writeIndex.load(std::memory_order_relaxed);
        const size_t readIdx = m_readIndex.load(std::memory_order_acquire);
        const size_t usedFrames = (writeIdx >= readIdx) ? (writeIdx - readIdx) : 0;
        const size_t availableFrames = (m_capacityFrames > usedFrames) ? (m_capacityFrames - usedFrames) : 0;

        const size_t toWrite = (std::min)(frames, availableFrames);
        if (toWrite == 0) return 0;

        const size_t startPos = (writeIdx & m_mask) * m_channels;
        const size_t endFrames = m_capacityFrames - (writeIdx & m_mask);

        if (toWrite <= endFrames) {
            std::copy_n(source, toWrite * m_channels, &m_buffer[startPos]);
        } else {
            const size_t firstChunk = endFrames * m_channels;
            const size_t secondChunk = (toWrite - endFrames) * m_channels;
            std::copy_n(source, firstChunk, &m_buffer[startPos]);
            std::copy_n(source + firstChunk, secondChunk, &m_buffer[0]);
        }

        m_writeIndex.store(writeIdx + toWrite, std::memory_order_release);
        return toWrite;
    }

    // Read samples (returns number of frames actually read; fills remaining with silence)
    size_t Read(float* destination, size_t frames) {
        if (!destination || frames == 0) return 0;

        const size_t writeIdx = m_writeIndex.load(std::memory_order_acquire);
        const size_t readIdx = m_readIndex.load(std::memory_order_relaxed);
        const size_t availableFrames = (writeIdx >= readIdx) ? (writeIdx - readIdx) : 0;

        const size_t toRead = (std::min)(frames, availableFrames);

        if (toRead > 0) {
            const size_t startPos = (readIdx & m_mask) * m_channels;
            const size_t endFrames = m_capacityFrames - (readIdx & m_mask);

            if (toRead <= endFrames) {
                std::copy_n(&m_buffer[startPos], toRead * m_channels, destination);
            } else {
                const size_t firstChunk = endFrames * m_channels;
                const size_t secondChunk = (toRead - endFrames) * m_channels;
                std::copy_n(&m_buffer[startPos], firstChunk, destination);
                std::copy_n(&m_buffer[0], secondChunk, destination + firstChunk);
            }
            m_readIndex.store(readIdx + toRead, std::memory_order_release);
        }

        // Fill underrun with silence
        if (toRead < frames) {
            std::fill_n(destination + (toRead * m_channels), (frames - toRead) * m_channels, 0.0f);
        }

        return toRead;
    }

    // Returns available frames ready to read
    size_t AvailableRead() const {
        const size_t writeIdx = m_writeIndex.load(std::memory_order_acquire);
        const size_t readIdx = m_readIndex.load(std::memory_order_relaxed);
        return writeIdx >= readIdx ? (writeIdx - readIdx) : 0;
    }

    // Returns available space to write
    size_t AvailableWrite() const {
        return m_capacityFrames - AvailableRead();
    }

    // Discard all buffered frames
    void Clear() {
        const size_t writeIdx = m_writeIndex.load(std::memory_order_relaxed);
        m_readIndex.store(writeIdx, std::memory_order_release);
    }

    // Artificially seek/shift delay offset in frames without heap allocations
    void SetDelayFrames(size_t targetBufferedFrames) {
        if (targetBufferedFrames > m_capacityFrames) {
            targetBufferedFrames = m_capacityFrames / 2;
        }

        const size_t currentAvailable = AvailableRead();
        if (targetBufferedFrames > currentAvailable) {
            // Need more delay: inject difference in silence frames using static stack buffer
            size_t diff = targetBufferedFrames - currentAvailable;
            constexpr size_t kChunkFrames = 256;
            float zeroChunk[kChunkFrames * 2] = {0.0f};
            while (diff > 0) {
                size_t step = (std::min)(diff, kChunkFrames);
                size_t written = Write(zeroChunk, step);
                if (written == 0) break;
                diff -= written;
            }
        } else if (targetBufferedFrames < currentAvailable) {
            // Need less delay: skip frames
            size_t diff = currentAvailable - targetBufferedFrames;
            size_t readIdx = m_readIndex.load(std::memory_order_relaxed);
            m_readIndex.store(readIdx + diff, std::memory_order_release);
        }
    }

    size_t Capacity() const { return m_capacityFrames; }
    uint32_t Channels() const { return m_channels; }

private:
    std::vector<float> m_buffer;
    size_t m_capacityFrames;
    size_t m_mask;
    uint32_t m_channels;

    alignas(64) std::atomic<size_t> m_writeIndex{0};
    alignas(64) std::atomic<size_t> m_readIndex{0};
};

} // namespace Auduo
