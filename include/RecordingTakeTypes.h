#ifndef LMMS_RECORDING_TAKE_TYPES_H
#define LMMS_RECORDING_TAKE_TYPES_H
#include "RecordingInputRuntime.h"
#include <QString>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <QByteArray>
class QIODevice;
namespace lmms { class AudioImportPolicy; }
namespace lmms::recording {
// Control-thread observations, deliberately separate from ADC/native clock claims.
struct TakeSessionFacts
{
 std::int64_t targetTick=0,prerollOriginTick=0;
 std::uint64_t rawReadyObservedMilliseconds=0,framesAtReadiness=0;
 int tempo=0,numerator=0,denominator=0;
 bool performanceReached=false,cancelled=false;
};
struct TakeDescriptor
{
 QString projectId,takeId,label;
 InputEpoch epoch;
 std::uint64_t projectGeneration=0,maxRawFrames=0;
 std::int64_t targetTick=0;
 std::uint32_t sampleRate=0,countInBars=0;
 std::optional<TakeSessionFacts> session;
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
struct RecordingFileIdentity
{
 std::uint32_t volume=0;
 std::uint64_t index=0;
 bool operator==(const RecordingFileIdentity&) const=default;
};
struct RawTakeFacts
{
 QString projectId,takeId;
 std::uint32_t sampleRate=0;
 std::uint64_t rawFrames=0;
 RecordingFileIdentity sourceIdentity;
 QByteArray sourceDigest;
};
struct TakeManifest
{
 unsigned schema=1;
 TakeDescriptor descriptor;
 TakeProgress progress;
 RawTakeFacts source;
 RecordingFileIdentity journalIdentity;
 QByteArray journalDigest;
 QString state,code,captureIntegrity="incomplete",placementState="saved_unattached";
 QString placementQuality="uncalibrated",driftQuality="unmeasured";
 QString recoveredFromProjectId,recoveredFromTakeId,diagnosticCode;
 std::uint64_t timestampDiscontinuities=0,observationsDropped=0;
 std::uint64_t recoveredFromDurableFrames=0,recoveredFromUnknownTailBytes=0,recoveredFromReadableFrames=0;
 bool sourceFaultKnown=false,observationLossKnown=false;
 std::uint64_t fileBytes=0,durableFrames=0,recoverableFrames=0,unknownTailBytes=0;
 bool tornJournalTail=false;
 InputFault originalFault=InputFault::None;
 InputTimestampQuality timestampQuality=InputTimestampQuality::Unavailable;
};
// Only a checked library factory may mint an exact read-only sealed source token.
class SealedTakeToken
{
public:
 ~SealedTakeToken();
 SealedTakeToken(SealedTakeToken&&) noexcept;
 SealedTakeToken& operator=(SealedTakeToken&&) noexcept;
 SealedTakeToken(const SealedTakeToken&)=delete;
 SealedTakeToken& operator=(const SealedTakeToken&)=delete;
 const RawTakeFacts& facts() const noexcept;
 bool isCurrent() const noexcept;
private:
 friend class RecordingLibrary;
 friend class ::lmms::AudioImportPolicy;
 struct State;
 explicit SealedTakeToken(std::unique_ptr<State>);
 QIODevice& readOnlySource() const noexcept;
 std::unique_ptr<State> m_state;
};
enum class TakeTerminal : std::uint8_t { NoTakeCreated,Incomplete,Complete,SavedUnattached,SealIntent };
struct TakeJournalCursor {std::uint64_t lastSequence=0;TakeProgress lastProgress;};
}
#endif
