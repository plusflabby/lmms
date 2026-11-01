#ifndef LMMS_RECORDING_INPUT_RUNTIME_H
#define LMMS_RECORDING_INPUT_RUNTIME_H
#include "RecordingRing.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>
namespace lmms::recording {
struct InputEpoch
{
 std::uint64_t projectGeneration=0, sessionRevision=0;
 bool operator==(const InputEpoch&) const=default;
 explicit operator bool() const noexcept { return projectGeneration && sessionRevision; }
};
struct InputProfile
{
 std::uint32_t sampleRate=0, inputChannels=0, selectedChannel=0, maxCallback=0;
 std::uint64_t maxTakeFrames=0;
};
struct InputBlock
{
 const float* input=nullptr;
 std::uint32_t frames=0, channels=0, hostFlags=0;
 double adcFirstSeconds=0, currentFirstSeconds=0;
};
enum class InputDecision : std::uint32_t { Continue, Stop };
enum class InputFault : std::uint32_t
{
 None, InvalidProfile, InvalidBuffer, InvalidSample, CounterOverflow, CaptureOverrun,
 HardwareDiscontinuity, ObservationOverflow, RingContractMismatch
};
enum class InputStopReason : std::uint32_t { None, Requested, DurationLimit, Fault };
enum class InputTimestampQuality : std::uint32_t { Observed, Unavailable, Discontinuous };
inline constexpr std::uint32_t InputHostUnderflow=1u, InputHostOverflow=2u;
struct InputObservation
{
 std::uint64_t receivedFirstFrame=0;
 std::uint32_t frames=0, hostFlags=0;
 double adcFirstSeconds=0, currentFirstSeconds=0;
 InputTimestampQuality timestampQuality=InputTimestampQuality::Unavailable;
};
struct InputSnapshot
{
 InputEpoch epoch;
 std::uint64_t receivedFrames=0, acceptedFrames=0, applicationDroppedFrames=0;
 std::uint64_t intentionallyUnrecordedFrames=0, invalidSamples=0, observationsDropped=0, hardwareLostFrames=0;
 float inputPeak=0;
 bool hardwareLossCountKnown=true;
 InputFault fault=InputFault::None;
 InputStopReason stopReason=InputStopReason::None;
 InputTimestampQuality timestampQuality=InputTimestampQuality::Unavailable;
};
class RecordingInputRuntime
{
public:
 static constexpr std::size_t ObservationCapacity=1024;
 RecordingInputRuntime(const InputProfile&,RecordingRing&) noexcept;
 bool enableCapture(InputEpoch) noexcept;
 InputDecision process(const InputBlock&) noexcept;
 void requestStop(InputEpoch) noexcept;
 // Control-thread supervision may latch a fault when the backend stops delivering callbacks.
 void reportFault(InputFault) noexcept;
 InputSnapshot snapshot() const noexcept;
 bool popObservation(InputObservation&) noexcept;
private:
 InputProfile m_profile;
 RecordingRing& m_ring;
 const bool m_profileReady;
 std::array<float,RecordingRing::MaximumCallback> m_monoScratch{};
 std::array<InputObservation,ObservationCapacity> m_observations{};
 alignas(64) std::atomic<std::uint64_t> m_observationRead{0},m_observationWrite{0};
 std::atomic<std::uint64_t> m_receivedFrames{0},m_acceptedFrames{0},m_applicationDroppedFrames{0};
 std::atomic<std::uint64_t> m_intentionallyUnrecordedFrames{0},m_invalidSamples{0},m_observationsDropped{0},m_hardwareLostFrames{0};
 std::atomic_bool m_hardwareLossCountKnown{true},m_started{false},m_enabled{false},m_stopRequested{false};
 std::atomic<std::uint64_t> m_epochGeneration{0},m_epochRevision{0};
 std::atomic<InputFault> m_fault{InputFault::None};
 std::atomic<InputStopReason> m_stopReason{InputStopReason::None};
 std::atomic<InputTimestampQuality> m_timestampQuality{InputTimestampQuality::Unavailable};
 std::atomic<std::uint32_t> m_inputPeakBits{0};
 bool m_haveClock=false,m_sawUnavailableClock=false;
 double m_lastAdcTime=0,m_lastCurrentTime=0;
 void latchFault(InputFault) noexcept;
 bool add(std::atomic<std::uint64_t>&,std::uint64_t) noexcept;
 void outside(std::uint64_t) noexcept;
};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<InputFault>::is_always_lock_free);
static_assert(std::is_trivially_copyable_v<InputBlock> && sizeof(InputObservation)<=128);
static_assert(sizeof(RecordingInputRuntime)+RecordingRing::Capacity*sizeof(float)+65536<=1024*1024);
}
#endif
