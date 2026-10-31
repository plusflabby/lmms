#ifndef LMMS_RECORDING_TYPES_H
#define LMMS_RECORDING_TYPES_H
#include <QString>
#include "lmms_export.h"
#include "RecordingInputRuntime.h"
#include <array>
#include <optional>
#include <vector>
namespace lmms::recording {
enum class Phase { Disarmed, Armed, Prepared, Priming, Preroll, Recording, Quiescing, Finalizing };
enum class TakeOutcome { None, Complete, SavedUnattached, Incomplete, NoTakeCreated };
struct LocalArmContext
{
 QString processId,projectId,deviceId,libraryRootId;
 quint64 projectGeneration=0;
 bool operator==(const LocalArmContext&) const=default;
};
struct PreparedTake
{
 QString preparedId;
 LocalArmContext context;
 qint64 startTick=0;
 int countInBars=0;
 bool metronome=false;
 QString label,transportHash;
 unsigned sampleRate=0;
};
struct Status
{
 Phase phase=Phase::Disarmed;
 bool armed=false,incomplete=false;
 QString preparedId,takeId,startRequestId;
 quint64 projectGeneration=0;
 TakeOutcome outcome=TakeOutcome::None;
};
enum class CaptureIntegrity {Pending,Complete,Incomplete,Failed};
enum class PlacementState {AwaitingLocalPlacement,ConfirmedUnattached,Attached,SavedUnattached};
struct RecordingCapabilities
{
 std::array<unsigned,2> sampleRates{44100,48000};
 unsigned maximumTakeSeconds=120,channels=1;
 bool requiresLocalArm=true,metronomeSupported=false,placementImplemented=false,sampleAccuracyVerified=false;
};
struct PrepareTakeRequest
{
 qint64 startTick=0;
 QString takeLabel;
 int countInBars=0;
 bool metronomeEnabled=false;
 quint64 expectedProjectGeneration=0;
};
struct StartTakeRequest {QString preparedTakeId;quint64 expectedProjectGeneration=0;QString startRequestId;};
struct PageRequest {qint64 offset=0;int limit=25;};
class RecordingRecorderView;
class ManualPlacementApproval
{
public:
 ManualPlacementApproval()=default;
private:
 friend class RecordingRecorderView;
 friend class RecordingPlacement;
 QString takeId;
 quint64 expectedProjectGeneration=0,placementRevision=0;
 qint64 sourceInTicks=0,targetTick=0;
};
struct RecordingStatus : Status
{
 QString takeLabel;
 QString code,captureCode,placementQuality="uncalibrated",driftQuality="unmeasured",outputContinuity="unobserved";
 CaptureIntegrity captureIntegrity=CaptureIntegrity::Pending;
 PlacementState placementState=PlacementState::SavedUnattached;
 bool sampleAccuracyVerified=false,placementConfirmed=false,cancelled=false,performanceReached=false,quiescent=true;
 qint64 intendedTargetTick=0,prerollOriginTick=0,nativeTick=0,countInTicksRemaining=0;
 quint64 rawStartObservedMilliseconds=0,framesAtReadiness=0;
 quint64 receivedFrames=0,acceptedFrames=0,writtenFrames=0,durableFrames=0,applicationDroppedFrames=0;
 quint64 intentionallyUnrecordedFrames=0,clippedSamples=0,invalidSamples=0,observationsDropped=0,hardwareLostFrames=0;
 bool hardwareLossCountKnown=false;
 InputTimestampQuality inputTimestampQuality=InputTimestampQuality::Unavailable;
};
struct RecordingReply {bool ok=false;QString code;RecordingStatus status;};
struct TakePage {QString code;std::vector<RecordingStatus> takes;qint64 nextOffset=0;bool hasMore=false;};
struct Result {bool ok=false;QString code;Status status;};
}
#endif
