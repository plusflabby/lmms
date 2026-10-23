#ifndef LMMS_RECORDING_RING_H
#define LMMS_RECORDING_RING_H
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include "lmms_export.h"
#include <limits>
namespace lmms::recording {
struct PushResult
{
 std::uint32_t acceptedFrames=0,droppedFrames=0;
 bool valid=false,limitReached=false,counterOverflow=false;
};
class LMMS_EXPORT RecordingRing
{
public:
 static constexpr std::uint32_t Capacity=131072;
 static constexpr std::uint32_t MaximumCallback=4096;
 static bool checkedCounterAdvance(std::uint64_t current,std::uint64_t delta,std::uint64_t& next) noexcept;
 RecordingRing(std::uint32_t sampleRate,std::uint32_t capacityFrames=Capacity,std::uint64_t maxTakeFrames=0);
 RecordingRing(const RecordingRing&)=delete;
 RecordingRing& operator=(const RecordingRing&)=delete;
 ~RecordingRing();
 bool isReady() const noexcept{return bool(m_frames);}
 std::uint32_t capacityFrames() const noexcept{return m_capacity;}
 PushResult push(const float* interleaved,std::size_t frames,std::uint32_t channels,std::uint32_t channelIndex) noexcept;
 std::size_t pop(float* preallocatedDestination,std::size_t capacity) noexcept;
 bool resetAfterQuiescence() noexcept;
 std::uint64_t acceptedFrames() const noexcept{return m_totalAccepted.load(std::memory_order_acquire);}
 std::uint64_t droppedFrames() const noexcept{return m_dropped.load(std::memory_order_acquire);}
 bool counterOverflowed() const noexcept{return m_counterOverflow.load(std::memory_order_acquire);}
private:
 const std::uint32_t m_sampleRate,m_capacity;
 const std::uint64_t m_maxTakeFrames;
 std::unique_ptr<float[]> m_frames;
 alignas(64) std::atomic<std::uint64_t> m_read{0};
 alignas(64) std::atomic<std::uint64_t> m_write{0};
 std::atomic<std::uint64_t> m_totalAccepted{0},m_dropped{0};
 std::atomic_bool m_counterOverflow{false};
};
}
#endif
