#include "RecordingPlacement.h"
#include "AudioEngine.h"
#include "Engine.h"
#include "Song.h"
#include <QCoreApplication>
#include <QThread>
#include <QUuid>
#include <cmath>
#include <limits>
namespace lmms::recording {
namespace {
bool uuid(const QString& text)
{
 const QUuid value(text);
 return !value.isNull() && value.toString(QUuid::WithoutBraces)==text;
}
bool sameSource(const RawTakeFacts& a,const RawTakeFacts& b)
{
 return a.projectId==b.projectId && a.takeId==b.takeId && a.sampleRate==b.sampleRate && a.rawFrames==b.rawFrames
  && a.sourceIdentity==b.sourceIdentity && a.sourceDigest.size()==32 && a.sourceDigest==b.sourceDigest;
}
bool sameGeometry(const RecordingTransportSnapshot& a,const RecordingTransportSnapshot& b)
{
 return b.available && a.projectId==b.projectId && a.projectGeneration==b.projectGeneration
  && a.contextRevision==b.contextRevision && a.seekRevision==b.seekRevision && a.stateRevision==b.stateRevision
  && a.tempo==b.tempo && a.numerator==b.numerator && a.denominator==b.denominator
  && a.ticksPerBar==b.ticksPerBar && a.sampleRate==b.sampleRate;
}
bool samePlan(const PlacementPlan& a,const PlacementPlan& b)
{
 return a.positionTicks==b.positionTicks && a.offsetTicks==b.offsetTicks && a.lengthTicks==b.lengthTicks
  && a.effectiveSourceInFrame==b.effectiveSourceInFrame && a.nativeEndFrame==b.nativeEndFrame && a.rawEndFrame==b.rawEndFrame
  && a.placementRevision==b.placementRevision && a.placementQuality==b.placementQuality
  && a.driftQuality==b.driftQuality && a.sampleAccuracyVerified==b.sampleAccuracyVerified;
}
// Deliberately retain native float multiplication/truncation. SampleTrack::play
// maps source-time ticks this way; rational/double rounding would change boundaries.
float nativeProduct(qint64 ticks,float framesPerTick)
{
 return float(ticks)*framesPerTick;
}
bool nativeFrame(qint64 ticks,float framesPerTick,quint64& result)
{
 const auto frame=nativeProduct(ticks,framesPerTick);
 if(!std::isfinite(frame) || frame<0 || double(frame)>std::numeric_limits<int>::max())return false;
 result=quint64(frame);return true;
}
}
PlacementResult RecordingPlacement::plan(const RawTakeFacts& source,const ManualPlacementApproval& approval,const NativeGeometrySnapshot& geometry)
{
 if(!uuid(approval.takeId) || !approval.expectedProjectGeneration || !approval.placementRevision)return {"manual_approval_required",{}};
 const auto& current=geometry.current;
 if(!current.available)return {"transport_unavailable",{}};
 if(!uuid(source.projectId) || !uuid(source.takeId) || source.sourceDigest.size()!=32 || !source.sourceIdentity.index)return {"invalid_source",{}};
 if(source.takeId!=approval.takeId || source.projectId!=current.projectId
  || approval.expectedProjectGeneration!=current.projectGeneration || current.projectGeneration!=geometry.reviewed.projectGeneration)return {"stale_project",{}};
 if(approval.placementRevision!=geometry.placementRevision)return {"stale_placement",{}};
 if(source.sampleRate!=44100 && source.sampleRate!=48000)return {"unsupported_rate",{}};
 if(current.sampleRate!=source.sampleRate)return {"unsupported_rate",{}};
 if(!source.rawFrames)return {"empty_source",{}};
 if(source.rawFrames>quint64(source.sampleRate)*120)return {"source_limit",{}};
 if(!sameSource(source,geometry.reviewedSource))return {"stale_source",{}};
 if(current.playing || current.paused || current.exporting)return {"transport_busy",{}};
 if(current.looping)return {"unsupported_loop",{}};
 if(!current.constantTempo || !current.stableMeter)return {"unsupported_transport_automation",{}};
 if(!sameGeometry(current,geometry.reviewed))return {"stale_geometry",{}};
 const auto fpt=geometry.sourceFramesPerTick;
 if(current.tempo<10 || current.tempo>999 || current.numerator<1 || current.numerator>32 || current.denominator<1 || current.denominator>32
  || current.ticksPerBar!=192*current.numerator/current.denominator || !std::isfinite(fpt) || fpt<=0)return {"invalid_geometry",{}};
 constexpr auto maximum=std::numeric_limits<int>::max();
 const auto q=approval.sourceInTicks,p=approval.targetTick;
 if(q<0 || q>maximum)return {"invalid_source_in",{}};
 if(p<0 || p>maximum)return {"invalid_position",{}};
 quint64 first=0;
 if(!nativeFrame(q,fpt,first))return {"invalid_geometry",{}};
 if(first>=source.rawFrames)return {"source_in_at_or_after_eof",{}};
 qint64 lo=1,hi=maximum-q;
 if(hi<1 || double(nativeProduct(q+hi,fpt))<double(source.rawFrames))return {"tick_overflow",{}};
 // At most 31 comparisons, no scan proportional to take duration or target tick.
 while(lo<hi)
 {
  const auto middle=lo+(hi-lo)/2;
  if(double(nativeProduct(q+middle,fpt))>=double(source.rawFrames))hi=middle;
  else lo=middle+1;
 }
 if(p>maximum-lo)return {"tick_overflow",{}};
 quint64 end=0;
 if(!nativeFrame(q+lo,fpt,end) || end<source.rawFrames)return {"invalid_geometry",{}};
 PlacementPlan plan;plan.positionTicks=p;plan.offsetTicks=-q;plan.lengthTicks=lo;
 plan.effectiveSourceInFrame=first;plan.nativeEndFrame=end;plan.rawEndFrame=source.rawFrames;plan.placementRevision=approval.placementRevision;
 return {"ok",std::move(plan)};
}
PlacementResult RecordingPlacement::revalidatePlan(const RawTakeFacts& source,const PlacementPlan& candidate,const NativeGeometrySnapshot& geometry)
{
 ManualPlacementApproval approval;
 approval.takeId=source.takeId;approval.expectedProjectGeneration=geometry.current.projectGeneration;
 approval.placementRevision=candidate.placementRevision;approval.sourceInTicks=-candidate.offsetTicks;approval.targetTick=candidate.positionTicks;
 const auto current=plan(source,approval,geometry);
 if(!current.plan)return current;
 if(!samePlan(*current.plan,candidate))return {"placement_review_stale",{}};
 return current;
}
PlacementResult RecordingPlacement::restorePersistedPlan(const RawTakeFacts& source,qint64 positionTicks,qint64 offsetTicks,
 qint64 lengthTicks,quint64 revision,const NativeGeometrySnapshot& geometry)
{
 if(offsetTicks>0 || offsetTicks==std::numeric_limits<qint64>::min() || lengthTicks<=0 || !revision)
  return {"persisted_geometry_mismatch",{}};
 ManualPlacementApproval approval;approval.takeId=source.takeId;approval.expectedProjectGeneration=geometry.current.projectGeneration;
 approval.placementRevision=revision;approval.sourceInTicks=-offsetTicks;approval.targetTick=positionTicks;
 const auto current=plan(source,approval,geometry);
 if(!current.plan)return current;
 if(current.plan->positionTicks!=positionTicks || current.plan->offsetTicks!=offsetTicks || current.plan->lengthTicks!=lengthTicks)
  return {"persisted_geometry_mismatch",{}};
 return current;
}
NativeGeometrySnapshot RecordingPlacement::captureGeometry(RecordingTransportControl& transport,const RawTakeFacts& source,
 const RecordingTransportSnapshot& reviewed,quint64 revision)
{
 NativeGeometrySnapshot result;result.reviewed=reviewed;result.reviewedSource=source;result.placementRevision=revision;
 auto* application=QCoreApplication::instance();
 if(!application || QThread::currentThread()!=application->thread())return result;
 auto* audio=Engine::audioEngine();auto* song=Engine::getSong();if(!audio || !song)return result;
 const auto current=transport.snapshot();if(!current.available)return result;
 {
  const auto guard=audio->requestChangesGuard();
  if(current.tempo!=song->getTempo() || current.sampleRate!=audio->outputSampleRate()
   || current.numerator!=song->getTimeSigModel().getNumerator() || current.denominator!=song->getTimeSigModel().getDenominator()
   || current.ticksPerBar!=song->ticksPerBar())return result;
  result.sourceFramesPerTick=Engine::framesPerTick(source.sampleRate);
 }
 const auto after=transport.snapshot();
 if(!sameGeometry(current,after) || current.playing!=after.playing || current.paused!=after.paused || current.exporting!=after.exporting)return result;
 result.current=after;return result;
}
}
