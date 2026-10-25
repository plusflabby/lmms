#ifndef LMMS_RECORDING_TAKE_TYPES_H
#define LMMS_RECORDING_TAKE_TYPES_H
#include "RecordingInputRuntime.h"
#include <QString>
#include <array>
#include <cstdint>
namespace lmms::recording {
struct TakeDescriptor
{
 QString projectId,takeId,label;
 InputEpoch epoch;
 std::uint64_t projectGeneration=0,maxRawFrames=0;
 std::int64_t targetTick=0;
 std::uint32_t sampleRate=0,countInBars=0;
};
struct TakeProgress
{
 std::uint64_t sequence=0,receivedFrames=0,acceptedFrames=0,writtenFrames=0;
 std::uint64_t applicationDroppedFrames=0,intentionallyUnrecordedFrames=0;
 std::uint64_t clippedSamples=0,invalidSamples=0,hardwareLostFrames=0;
 InputFault fault=InputFault::None;
 InputTimestampQuality timestampQuality=InputTimestampQuality::Unavailable;
 bool hardwareLossCountKnown=true;
 std::array<std::uint8_t,32> sourceDigest{};
 bool sourceDigestKnown=false;
};
enum class TakeTerminal : std::uint8_t { NoTakeCreated,Incomplete,Complete,SavedUnattached,SealIntent };
struct TakeJournalCursor {std::uint64_t lastSequence=0;TakeProgress lastProgress;};
}
#endif
