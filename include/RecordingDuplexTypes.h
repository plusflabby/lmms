#ifndef LMMS_RECORDING_DUPLEX_TYPES_H
#define LMMS_RECORDING_DUPLEX_TYPES_H
#include <atomic>
#include <cstdint>
#include <type_traits>
#include "lmms_export.h"
namespace lmms::recording {
struct RenderEpoch
{
 std::uint64_t generation=0, revision=0;
 bool operator==(const RenderEpoch&) const = default;
};
struct RenderStamp
{
 RenderEpoch epoch;
 std::int64_t renderFirstFrame=0;
 std::uint32_t frames=0;
 bool musicalValid=false, hasTarget=false;
 std::uint32_t targetOffset=0;
};
struct OutputTransfer
{
 std::uint32_t copiedFrames=0, missingFrames=0;
 bool hasTarget=false;
 std::uint32_t targetOffset=0;
 RenderEpoch epoch;
};
struct CallbackBuffers
{
 const float* input=nullptr;
 float* output=nullptr;
 std::uint32_t frames=0, inputChannels=0, selectedChannel=0;
};
struct DuplexClockObservation
{
 double adcFirstSeconds=0, dacFirstSeconds=0;
 std::uint64_t receivedFirstFrame=0, submittedFirstFrame=0;
 std::uint32_t hostFlags=0;
 RenderEpoch epoch;
};
enum class CallbackDecision : std::uint32_t { Continue, Complete, Abort };
enum class DuplexFault : std::uint32_t
{
 None, InvalidBuffer, StaleEpoch, UnsupportedTiming, OutputUnderrun,
 CaptureOverrun, HardwareDiscontinuity, ClockObservationOverflow, StopRequested
};
struct QualifiedDuplexProfile
{
 RenderEpoch epoch;
 std::uint32_t sampleRate=0, inputChannels=0, selectedChannel=0, outputChannels=0;
 std::uint32_t minimumCallback=32, maximumCallback=4096;
 // Defaults never establish hardware eligibility. Only later physical qualification may set this.
 bool hardwareClockValidated=false;
};
struct DuplexCounters
{
 std::atomic<std::uint64_t> received{0}, submitted{0}, captured{0}, written{0};
 std::atomic<std::uint64_t> applicationInputLost{0}, applicationOutputMissing{0};
 std::atomic<std::uint64_t> hardwareFramesLost{0};
 std::atomic<std::uint32_t> hardwareLossCountKnown{1};
 std::atomic<DuplexFault> firstFault{DuplexFault::None};
};
inline constexpr std::uint32_t OutputCapacity=16384, OutputSpans=1024, NativePeriodLimit=256;
inline constexpr std::uint32_t CallbackMinimum=32, CallbackMaximum=4096;
static_assert(sizeof(RenderStamp)<=64 && std::is_trivially_copyable_v<RenderStamp>);
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic<DuplexFault>::is_always_lock_free);
static_assert(std::is_trivially_copyable_v<CallbackBuffers> && std::is_trivially_copyable_v<DuplexClockObservation>);
}
#endif
