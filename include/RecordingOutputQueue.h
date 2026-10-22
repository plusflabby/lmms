#ifndef LMMS_RECORDING_OUTPUT_QUEUE_H
#define LMMS_RECORDING_OUTPUT_QUEUE_H
#include "RecordingDuplexTypes.h"
#include "SampleFrame.h"
#include <array>
#include <span>
namespace lmms::recording {
class RecordingOutputQueue;
class LMMS_EXPORT ProducerReservation
{
public:
 ProducerReservation()=default;
 ~ProducerReservation();
 ProducerReservation(ProducerReservation&&) noexcept;
 ProducerReservation& operator=(ProducerReservation&&) noexcept;
 ProducerReservation(const ProducerReservation&)=delete;
 ProducerReservation& operator=(const ProducerReservation&)=delete;
 explicit operator bool() const noexcept {return m_owner!=nullptr;}
private:
 friend class RecordingOutputQueue;
 RecordingOutputQueue* m_owner=nullptr;
 std::uint64_t m_first=0, m_descriptor=0;
 std::uint32_t m_frames=0;
};
// One producer and one consumer, each with exclusive ownership of its endpoint.
// A queue is bound to one epoch for its entire lifetime; never reset live cursors.
class LMMS_EXPORT RecordingOutputQueue
{
public:
 explicit RecordingOutputQueue(RenderEpoch epoch):m_epoch(epoch){}
 RecordingOutputQueue(const RecordingOutputQueue&)=delete;
 RecordingOutputQueue& operator=(const RecordingOutputQueue&)=delete;
 ProducerReservation reservePeriod(RenderEpoch,std::uint32_t frames) noexcept;
 bool publish(ProducerReservation,std::span<const SampleFrame>,RenderStamp) noexcept;
 void consume(float* stereo,std::uint32_t frames,OutputTransfer&) noexcept;
private:
 friend class ProducerReservation;
 void cancel(ProducerReservation&) noexcept;
 const RenderEpoch m_epoch;
 std::array<SampleFrame,OutputCapacity> m_pcm{};
 std::array<RenderStamp,OutputSpans> m_stamps{};
 alignas(64) std::atomic<std::uint64_t> m_frameRead{0};
 alignas(64) std::atomic<std::uint64_t> m_descriptorRead{0};
 alignas(64) std::atomic<std::uint64_t> m_descriptorWrite{0};
 std::uint64_t m_frameWrite=0;
 std::uint32_t m_partial=0;
 bool m_reserved=false, m_targetPublished=false;
};
}
#endif
