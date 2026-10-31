#ifndef LMMS_RECORDING_PLACEMENT_H
#define LMMS_RECORDING_PLACEMENT_H
#include "RecordingSessionManager.h"
#include "RecordingTakeTypes.h"
#include <optional>
namespace lmms::recording {
// Copied control-owner geometry. Reviewed facts are retained from local review;
// current facts are refreshed before planning. Neither is an ADC/DAC clock.
struct NativeGeometrySnapshot
{
 RecordingTransportSnapshot current,reviewed;
 RawTakeFacts reviewedSource;
 quint64 placementRevision=0;
 float sourceFramesPerTick=0;
};
struct PlacementPlan
{
 qint64 positionTicks=0,offsetTicks=0,lengthTicks=0;
 quint64 effectiveSourceInFrame=0,nativeEndFrame=0,rawEndFrame=0,placementRevision=0;
 QString placementQuality="uncalibrated",driftQuality="unmeasured";
 bool sampleAccuracyVerified=false;
};
struct PlacementResult {QString code;std::optional<PlacementPlan> plan;};
class RecordingPlacement
{
public:
 // No source/device I/O, model mutation or attachment. Source authorization is Task 7.
 static PlacementResult plan(const RawTakeFacts&,const ManualPlacementApproval&,const NativeGeometrySnapshot&);
 static PlacementResult revalidatePlan(const RawTakeFacts&,const PlacementPlan&,const NativeGeometrySnapshot&);
 static PlacementResult restorePersistedPlan(const RawTakeFacts&,qint64 positionTicks,qint64 offsetTicks,
  qint64 lengthTicks,quint64 placementRevision,const NativeGeometrySnapshot&);
 static NativeGeometrySnapshot captureGeometry(RecordingTransportControl&,const RawTakeFacts& reviewedSource,
  const RecordingTransportSnapshot& reviewedTransport,quint64 placementRevision);
};
}
#endif
