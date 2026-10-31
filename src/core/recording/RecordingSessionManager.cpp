#include "RecordingSessionManager.h"
#include "RecordingAttachment.h"
#include "RecordingPlacement.h"
#include "RecordingSessionState.h"
#include "AudioEngine.h"
#include "AudioDevice.h"
#include "RecordingAudioBackendPolicy.h"
#include "Engine.h"
#include "Song.h"
#include <QPointer>
#include <QThread>
#include <QUuid>
#include <QCryptographicHash>
#include <QHash>
#include <atomic>
#include <limits>
#include <algorithm>
namespace lmms::recording {
namespace {
bool opaqueId(const QString& text){const QUuid id(text);return !id.isNull() && id.toString(QUuid::WithoutBraces)==text;}
QString publicCode(const QString& value)
{
 if(value.isEmpty())return {};
 if(value.size()>96)return "recording_failed";
 for(const auto c:value)if(!((c>='a' && c<='z') || (c>='0' && c<='9') || c=='_' || c=='-'))return "recording_failed";
 return value;
}
QString transportHash(const RecordingTransportSnapshot& s)
{
 QByteArray bytes=s.projectId.toUtf8();
 for(const auto value:{s.projectGeneration,s.contextRevision,s.seekRevision,s.stateRevision,quint64(s.tempo),quint64(s.numerator),quint64(s.denominator),quint64(s.ticksPerBar),quint64(s.sampleRate)})
 {bytes+=':';bytes+=QByteArray::number(value);}
 return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
}
bool sameConfiguration(const RecordingTransportSnapshot& a,const RecordingTransportSnapshot& b)
{
 return a.available && b.available && b.outputCaptureSafe && a.projectId==b.projectId && a.projectGeneration==b.projectGeneration && a.contextRevision==b.contextRevision
  && a.tempo==b.tempo && a.numerator==b.numerator && a.denominator==b.denominator && a.ticksPerBar==b.ticksPerBar && a.sampleRate==b.sampleRate
  && !b.exporting && !b.looping && !b.metronomeActive && b.constantTempo && b.stableMeter;
}
QString eligibility(const RecordingTransportSnapshot& s)
{
 if(!s.available)return "transport_unavailable";
 if(!s.outputCaptureSafe)return "output_capture_unsupported";
 if(s.playing || s.paused || s.exporting)return "transport_busy";
 if(s.looping)return "unsupported_loop";
 if(s.metronomeActive)return "unsupported_metronome";
 if(!s.constantTempo || !s.stableMeter)return "unsupported_transport_automation";
 if(s.sampleRate!=44100 && s.sampleRate!=48000)return "unsupported_rate";
 if(s.tempo<10 || s.tempo>999 || s.numerator<=0 || s.denominator<=0 || s.ticksPerBar<=0)return "unsupported_meter";
 return {};
}
quint64 countinFrames(qint64 ticks,const RecordingTransportSnapshot& s)
{
 // Checked native tick duration is a nominal resource budget, never an ADC onset.
 if(ticks<0 || ticks>std::numeric_limits<int>::max() || !s.sampleRate || s.tempo<=0)return std::numeric_limits<quint64>::max();
 const auto n=quint64(ticks)*s.sampleRate*5,denominator=quint64(s.tempo)*4;
 return n/denominator+quint64(n%denominator!=0);
}
QString inputFailure(const RecordingHostSnapshot& h)
{
 if(h.input.diagnostic.failedStage==InputDiagnosticStage::FirstCallback)return "input_not_ready";
 if(!h.errorCode.isEmpty())return publicCode(h.errorCode);
 if(!h.input.errorCode.isEmpty())return publicCode(h.input.errorCode);
 switch(h.input.input.fault)
 {
  case InputFault::None:break;
  case InputFault::ObservationOverflow:return "observation_overflow";
  case InputFault::CaptureOverrun:return "input_overrun";
  case InputFault::HardwareDiscontinuity:return "hardware_discontinuity";
  default:return "input_failed";
 }
 if(h.writer.fault!=WriterFault::None)return "writer_failed";
 return {};
}
}
struct RecordingSessionManager::Impl
{
 Impl(RecordingServiceHost& input,RecordingTransportControl& native):host(input),transport(native),attachment(input,native)
 {
  const auto world=transport.snapshot();context=makeContext(world);state=std::make_unique<RecordingSessionState>(context);current.projectGeneration=world.projectGeneration;
 }
 RecordingServiceHost& host;RecordingTransportControl& transport;RecordingAttachment attachment;QThread* owner=QThread::currentThread();
 const QString processId=QUuid::createUuid().toString(QUuid::WithoutBraces);
 LocalArmContext context;std::unique_ptr<RecordingSessionState> state;
 std::optional<PreparedTake> prepared;RecordingTransportSnapshot frozen,expected;
 RecordingLocalArmSnapshot admitted;
 RecordingStatus current;std::vector<RecordingStatus> history;
 struct Request {QString prepared;quint64 generation;};QHash<QString,Request> requests;
 quint64 now=0,maximumFrames=0;
 RecordingPlaybackOwnership playback;
 bool active=false,rawSeen=false,backingStarted=false,stopping=false,disarming=false,closing=false,invalidated=false;
 unsigned commandDepth=0;
 void assertOwner() const{Q_ASSERT(QThread::currentThread()==owner);}
 LocalArmContext makeContext(const RecordingTransportSnapshot& world) const
 {return {processId,world.projectId.isEmpty()?QString("unconfigured"):world.projectId,QString("local-input"),QString("root-%1").arg(host.rootRevision()),world.projectGeneration};}
 void base()
 {
  static_cast<Status&>(current)=state->status();current.quiescent=!active && !commandDepth && host.quiescent() && !playback.serial;
 }
 RecordingReply reply(bool ok,const QString& code) const
 {auto status=current;status.code=code;return {ok,code,std::move(status)};}
 const RecordingStatus* oldTake(const QString& take) const
 {const auto found=std::find_if(history.begin(),history.end(),[&](const auto& value){return value.takeId==take;});return found==history.end()?nullptr:&*found;}
 void syncContext(const RecordingTransportSnapshot& world)
 {
  const auto next=makeContext(world);if(next==context)return;
  state->invalidate(next);context=next;
  if(active){invalidated=true;current.cancelled=true;stopping=true;disarming=true;if(current.captureCode.isEmpty())current.captureCode="context_changed";}
  else{prepared.reset();if(host.localArmSnapshot().armed)std::ignore=host.disarm();base();}
 }
 TakeSessionFacts sessionFacts() const
 {return {current.intendedTargetTick,current.prerollOriginTick,current.rawStartObservedMilliseconds,current.framesAtReadiness,
   frozen.tempo,frozen.numerator,frozen.denominator,current.performanceReached,current.cancelled};}
 void saveSessionFacts(){std::ignore=host.noteSessionFacts(admitted.epoch,sessionFacts());}
 void counts(const RecordingHostSnapshot& h)
 {
  if(h.input.input.epoch!=admitted.epoch)return;
  const auto& in=h.input.input;current.receivedFrames=in.receivedFrames;current.acceptedFrames=in.acceptedFrames;current.writtenFrames=h.writer.writtenFrames;
  current.durableFrames=h.writer.durableFrames;current.applicationDroppedFrames=in.applicationDroppedFrames;current.intentionallyUnrecordedFrames=in.intentionallyUnrecordedFrames;
  current.inputPeak=in.inputPeak;
  current.clippedSamples=h.writer.clippedSamples;current.invalidSamples=in.invalidSamples;current.observationsDropped=in.observationsDropped;
  current.hardwareLossCountKnown=h.input.firstCallbackSeen && in.hardwareLossCountKnown;current.hardwareLostFrames=in.hardwareLostFrames;
  current.inputTimestampQuality=h.input.observations.observations?std::max(in.timestampQuality,h.input.observations.quality):in.timestampQuality;
  if(in.acceptedFrames && !rawSeen){rawSeen=true;std::ignore=state->rawCaptureObserved(current.takeId);current.rawStartObservedMilliseconds=now;current.framesAtReadiness=in.receivedFrames;}
 }
 bool live(const RecordingHostSnapshot& h) const
 {
  return h.input.state==RecordingControllerState::Capturing && h.input.firstCallbackSeen && h.input.input.epoch==admitted.epoch
   && h.input.input.acceptedFrames>0 && h.input.input.receivedFrames<maximumFrames && h.input.input.acceptedFrames<maximumFrames
   && h.input.input.fault==InputFault::None && h.input.input.stopReason==InputStopReason::None
   && h.writer.state!=WriterState::StorageBlocked && h.writer.fault==WriterFault::None;
 }
 void queueStop(bool revoke,bool cancel,const QString& code={})
 {
  stopping=true;disarming=disarming||revoke;current.cancelled=current.cancelled||cancel;
  if(!code.isEmpty() && current.captureCode.isEmpty())current.captureCode=publicCode(code);
  if(revoke)std::ignore=state->disarm();else std::ignore=state->stop(current.takeId);
  base();
 }
 bool stopWork()
 {
  if(commandDepth)return true;
  saveSessionFacts();
  if(playback.serial){++commandDepth;const auto result=transport.stopOwned(playback);--commandDepth;playback={};if(!result.ok && current.captureCode.isEmpty())current.captureCode="song_stop_failed";}
  const auto before=host.snapshot();
  if(before.quiescent)return true;
  RecordingHostResult result;
    ++commandDepth;
  if(closing){std::ignore=host.requestStop(admitted.epoch);result=host.requestShutdown(now);}
  else if(disarming)result=host.disarm();
  else result=host.requestStop(admitted.epoch);
  --commandDepth;
  if(!result.ok && result.code!="shutdown_pending" && current.captureCode.isEmpty())current.captureCode=publicCode(result.code);
  return result.ok || result.code=="shutdown_pending";
 }
 void finish(const RecordingHostSnapshot& h)
 {
  counts(h);
  const auto error=inputFailure(h);if(!error.isEmpty() && current.captureCode.isEmpty())current.captureCode=error;
  if(state->status().phase!=Phase::Quiescing && state->status().phase!=Phase::Finalizing)std::ignore=state->stop(current.takeId);
  if(state->status().phase==Phase::Quiescing)std::ignore=state->quiesced(current.takeId);
  const auto& in=h.input.input;
  const bool complete=rawSeen && h.writer.state==WriterState::Sealed && h.writer.audioSealed && h.writer.journalSealed && h.writer.sourceSha256.size()==32
   && h.writer.writtenFrames==in.acceptedFrames && h.writer.removedFrames==h.writer.writtenFrames && !h.writer.pendingFrames
   && in.receivedFrames>=in.acceptedFrames && in.receivedFrames-in.acceptedFrames==in.intentionallyUnrecordedFrames
   && !in.applicationDroppedFrames && !in.invalidSamples && !in.observationsDropped && in.fault==InputFault::None
   && in.hardwareLossCountKnown && !in.hardwareLostFrames && h.writer.fault==WriterFault::None;
  current.captureIntegrity=complete?CaptureIntegrity::Complete:rawSeen?CaptureIntegrity::Incomplete:CaptureIntegrity::Failed;
  current.placementState=complete && current.performanceReached && !current.cancelled && !invalidated?PlacementState::AwaitingLocalPlacement:PlacementState::SavedUnattached;
  if(rawSeen && state->status().phase==Phase::Finalizing)std::ignore=state->finish(current.takeId,complete?TakeOutcome::SavedUnattached:TakeOutcome::Incomplete);
  std::ignore=state->disarm();base();current.code=rawSeen?(complete?"take_saved_unattached":"take_incomplete"):"no_take_created";
  active=false;prepared.reset();stopping=disarming=false;current.quiescent=host.quiescent() && !playback.serial;current.armed=false;
  history.push_back(current);if(history.size()>64)history.erase(history.begin());
 }
 void launch(const RecordingHostSnapshot& h)
 {
  if(!live(h))return;
  if(h.input.input.receivedFrames>=maximumFrames || countinFrames(current.intendedTargetTick-current.prerollOriginTick,frozen)>=maximumFrames-h.input.input.receivedFrames)
  {queueStop(false,true,"startup_exhausted_take_budget");std::ignore=stopWork();return;}
  ++commandDepth;const auto sought=transport.seekSong(current.prerollOriginTick);--commandDepth;
  const auto afterSeek=transport.snapshot();counts(host.snapshot());
  if(stopping){std::ignore=stopWork();return;}
  if(!sought.ok || !sameConfiguration(frozen,afterSeek) || afterSeek.playing || afterSeek.paused || afterSeek.songTick!=current.prerollOriginTick
   || afterSeek.seekRevision!=frozen.seekRevision+1 || afterSeek.stateRevision!=frozen.stateRevision)
  {queueStop(true,true,"transport_changed");std::ignore=stopWork();return;}
  if(!live(host.snapshot())){queueStop(false,true,"capture_ended_during_startup");std::ignore=stopWork();return;}
  std::ignore=state->primed(current.takeId);
  ++commandDepth;const auto started=transport.playSong();--commandDepth;playback=started.ownership;
  const auto after=transport.snapshot();counts(host.snapshot());
  if(stopping){std::ignore=stopWork();return;}
  if(!started.ok || !sameConfiguration(afterSeek,after) || after.seekRevision!=afterSeek.seekRevision || after.stateRevision!=afterSeek.stateRevision+1
   || !after.playing || !after.songMode || after.paused)
  {queueStop(true,true,started.ok?"transport_changed":"song_start_failed");std::ignore=stopWork();return;}
  backingStarted=true;expected=after;
  if(!live(host.snapshot())){queueStop(false,!current.performanceReached,"capture_ended_during_startup");std::ignore=stopWork();}
 }
};
RecordingSessionManager::RecordingSessionManager(RecordingServiceHost& host,RecordingTransportControl& transport):m_impl(std::make_unique<Impl>(host,transport)){}
RecordingSessionManager::~RecordingSessionManager()
{
 if(m_impl && !quiescent())std::ignore=requestShutdown();
 // The host remains the closing barrier and owns all live stream/writer storage.
}
RecordingCapabilities RecordingSessionManager::capabilities() const noexcept
{RecordingCapabilities value;value.placementImplemented=true;return value;}
RecordingStatus RecordingSessionManager::status() const
{
 auto& s=*m_impl;s.assertOwner();auto value=s.current;
 if(value.takeId.isEmpty())value.projectGeneration=s.transport.snapshot().projectGeneration;
 value.armed=value.armed && (s.active || s.host.localArmSnapshot().armed);
 return value;
}
RecordingReply RecordingSessionManager::prepare(const PrepareTakeRequest& request)
{
 auto& s=*m_impl;s.assertOwner();if(s.active || s.commandDepth)return s.reply(false,"recording_busy");if(s.closing)return s.reply(false,"shutting_down");
 if(request.metronomeEnabled)return s.reply(false,"unsupported_metronome");
 if(request.startTick<0 || request.startTick>std::numeric_limits<int>::max() || request.countInBars<0 || request.countInBars>2
  || !request.expectedProjectGeneration || request.takeLabel.trimmed().isEmpty() || request.takeLabel.size()>128)return s.reply(false,"invalid_request");
 for(const auto c:request.takeLabel)if(c.unicode()<32)return s.reply(false,"invalid_request");
 if(request.takeLabel.toUtf8().size()>96)return s.reply(false,"label_resource_limit");
 const auto world=s.transport.snapshot();s.syncContext(world);
 if(request.expectedProjectGeneration!=world.projectGeneration)return s.reply(false,"stale_project");
 if(s.host.projectIdentity().id.isEmpty() || !s.host.rootRevision())return s.reply(false,"library_required");
 if(world.projectId!=s.host.projectIdentity().id)return s.reply(false,"stale_project");
 if(const auto error=eligibility(world);!error.isEmpty())return s.reply(false,error);
 const auto ticks=qint64(request.countInBars)*world.ticksPerBar;
 if(ticks>std::numeric_limits<int>::max())return s.reply(false,"invalid_request");
 if(request.startTick<ticks)return s.reply(false,"insufficient_preroll");
 const auto arm=s.host.localArmSnapshot();if(arm.armed && arm.sampleRate!=world.sampleRate)return s.reply(false,"unsupported_rate");
 const auto maximum=arm.armed?arm.maxRawFrames:quint64(world.sampleRate)*120;
 if(countinFrames(ticks,world)>=maximum)return s.reply(false,"countin_exceeds_take_budget");
 PreparedTake p;p.context=s.context;p.startTick=request.startTick;p.countInBars=request.countInBars;p.label=request.takeLabel;p.sampleRate=world.sampleRate;p.transportHash=transportHash(world);
 const auto accepted=s.state->prepare(p);if(!accepted.ok)return s.reply(false,accepted.code);
 p.preparedId=accepted.status.preparedId;s.prepared=p;s.frozen=s.expected=world;s.current={};s.base();s.current.code="prepared";s.current.takeLabel=request.takeLabel;
 s.current.intendedTargetTick=request.startTick;s.current.prerollOriginTick=request.startTick-ticks;s.current.countInTicksRemaining=ticks;s.current.nativeTick=world.songTick;
 s.invalidated=false;return s.reply(true,"prepared");
}
RecordingReply RecordingSessionManager::arm(LocalInputTicket&& ticket)
{
 auto& s=*m_impl;s.assertOwner();if(s.active || s.commandDepth)return s.reply(false,"recording_busy");if(s.closing)return s.reply(false,"shutting_down");
 const auto world=s.transport.snapshot();s.syncContext(world);
 const auto accepted=s.host.adoptLocalArm(std::move(ticket));if(!accepted.ok)return s.reply(false,accepted.code);
 const auto facts=s.host.localArmSnapshot();
 if(!world.available || facts.epoch.projectGeneration!=world.projectGeneration || world.projectId!=s.host.projectIdentity().id)
 {std::ignore=s.host.disarm();std::ignore=s.state->disarm();s.base();return s.reply(false,"stale_project");}
 const auto armed=s.state->arm(s.context);s.base();return s.reply(armed.ok,armed.code);
}
RecordingReply RecordingSessionManager::start(const StartTakeRequest& request)
{
 auto& s=*m_impl;s.assertOwner();
 if(!opaqueId(request.preparedTakeId) || !opaqueId(request.startRequestId) || !request.expectedProjectGeneration)return s.reply(false,"invalid_request");
 if(const auto found=s.requests.find(request.startRequestId);found!=s.requests.end())
 {
  if(found->prepared!=request.preparedTakeId || found->generation!=request.expectedProjectGeneration)return s.reply(false,"conflicting_start");
  if(s.active && s.current.takeId==request.preparedTakeId)return s.reply(true,"start_accepted");
  if(const auto* old=s.oldTake(request.preparedTakeId)){auto copy=*old;copy.armed=s.current.armed;return {copy.captureIntegrity!=CaptureIntegrity::Failed,copy.code,std::move(copy)};}
  return s.reply(false,"stale_preparation");
 }
 if(s.active || s.commandDepth)return s.reply(false,"recording_busy");if(s.closing)return s.reply(false,"shutting_down");
 const auto world=s.transport.snapshot();s.syncContext(world);
 if(request.expectedProjectGeneration!=world.projectGeneration)return s.reply(false,"stale_project");
 if(!s.prepared || s.prepared->preparedId!=request.preparedTakeId)return s.reply(false,"stale_preparation");
 const auto facts=s.host.localArmSnapshot();if(!s.state->status().armed || !facts.armed)return s.reply(false,"not_armed");
 if(facts.epoch.projectGeneration!=world.projectGeneration || facts.rootRevision!=s.host.rootRevision())return s.reply(false,"arm_expired");
 if(facts.sampleRate!=world.sampleRate || s.prepared->sampleRate!=facts.sampleRate)return s.reply(false,"unsupported_rate");
 if(transportHash(world)!=s.prepared->transportHash || !eligibility(world).isEmpty())
 {std::ignore=s.state->disarm();std::ignore=s.host.disarm();s.prepared.reset();s.base();return s.reply(false,"stale_preparation");}
 if(countinFrames(s.current.intendedTargetTick-s.current.prerollOriginTick,world)>=facts.maxRawFrames)return s.reply(false,"countin_exceeds_take_budget");
 if(s.requests.size()>=64)return s.reply(false,"resource_limit");
 const auto accepted=s.state->start(request.preparedTakeId,request.startRequestId,request.expectedProjectGeneration);if(!accepted.ok)return s.reply(false,accepted.code);
 s.requests.insert(request.startRequestId,{request.preparedTakeId,request.expectedProjectGeneration});
 s.active=true;s.rawSeen=s.backingStarted=s.stopping=s.disarming=s.invalidated=false;s.playback={};s.admitted=facts;s.maximumFrames=facts.maxRawFrames;s.base();s.current.code="start_accepted";
 auto files=s.host.createUniqueTake({s.current.takeId,s.prepared->label},facts.sampleRate,facts.maxRawFrames);
 if(!files.sink){s.queueStop(true,true,files.code);std::ignore=s.host.disarm();s.finish(s.host.snapshot());return s.reply(false,publicCode(files.code));}
 TakeDescriptor descriptor;descriptor.projectId=s.host.projectIdentity().id;descriptor.takeId=s.current.takeId;descriptor.label=s.prepared->label;descriptor.epoch=facts.epoch;
 descriptor.projectGeneration=world.projectGeneration;descriptor.sampleRate=facts.sampleRate;descriptor.maxRawFrames=facts.maxRawFrames;descriptor.targetTick=s.prepared->startTick;descriptor.countInBars=s.prepared->countInBars;descriptor.session=s.sessionFacts();
 const auto started=s.host.startTake(descriptor,std::move(files.sink),s.now,std::move(files.receipt));
 if(!started.ok){s.queueStop(true,true,started.code);std::ignore=s.host.disarm();s.finish(s.host.snapshot());return s.reply(false,publicCode(started.code));}
 return s.reply(true,"start_accepted");
}
RecordingReply RecordingSessionManager::stop(const QString& take)
{
 auto& s=*m_impl;s.assertOwner();if(take.isEmpty())return s.reply(false,"stale_take");
 if(s.active && take==s.current.takeId){s.queueStop(false,!s.current.performanceReached);if(s.commandDepth)return s.reply(true,"stop_deferred");const auto ok=s.stopWork();s.base();return s.reply(ok,ok?"stop_requested":"shutdown_pending");}
 if(const auto* old=s.oldTake(take)){auto copy=*old;copy.armed=s.current.armed;return {true,copy.code,std::move(copy)};}
 const auto stopped=s.state->stop(take);s.base();return s.reply(stopped.ok,stopped.code);
}
RecordingReply RecordingSessionManager::disarm()
{
 auto& s=*m_impl;s.assertOwner();if(s.active){s.queueStop(true,!s.current.performanceReached);if(s.commandDepth)return s.reply(true,"disarm_deferred");const auto ok=s.stopWork();s.base();return s.reply(ok,ok?"disarmed":"shutdown_pending");}
 std::ignore=s.state->disarm();const auto result=s.host.disarm();s.base();return s.reply(result.ok,result.code);
}
RecordingReply RecordingSessionManager::requestShutdown()
{
 auto& s=*m_impl;s.assertOwner();s.closing=true;
 if(s.active){s.queueStop(true,!s.current.performanceReached);std::ignore=s.stopWork();return s.reply(quiescent(),quiescent()?"quiescent":"shutdown_pending");}
 std::ignore=s.state->disarm();const auto result=s.host.requestShutdown(s.now);s.base();return s.reply(result.ok,result.code);
}
RecordingPlacementReview RecordingSessionManager::reviewPlacement(const ManualPlacementApproval& approval)
{
	auto& s = *m_impl;
	s.assertOwner();
	if (s.active || s.commandDepth) return {QStringLiteral("recording_busy"), {}, {}};
	if (s.closing) return {QStringLiteral("shutting_down"), {}, {}};
	if (!opaqueId(approval.takeId) || !approval.expectedProjectGeneration || !approval.placementRevision)
		return {QStringLiteral("manual_approval_required"), {}, {}};
	const auto world = s.transport.snapshot();
	s.syncContext(world);
	if (approval.expectedProjectGeneration != world.projectGeneration)
		return {QStringLiteral("stale_project"), {}, {}};
	const RecordingStatus* takeStatus = nullptr;
	if (s.current.takeId == approval.takeId && s.current.captureIntegrity != CaptureIntegrity::Pending)
		takeStatus = &s.current;
	else
	{
		const auto found = std::find_if(s.history.begin(), s.history.end(), [&](const auto& value) {
			return value.takeId == approval.takeId;
		});
		if (found != s.history.end()) takeStatus = &*found;
	}
	if (!takeStatus) return {QStringLiteral("stale_take"), {}, {}};
	if (takeStatus->projectGeneration != approval.expectedProjectGeneration)
		return {QStringLiteral("stale_project"), {}, {}};
	if (takeStatus->captureIntegrity != CaptureIntegrity::Complete || takeStatus->cancelled || !takeStatus->performanceReached)
		return {QStringLiteral("take_saved_unattached"), {}, {}};
	const auto project = s.host.projectIdentity();
	if (project.id.isEmpty() || project.id != world.projectId)
		return {QStringLiteral("stale_project"), {}, {}};
	if (!s.host.quiescent()) return {QStringLiteral("recording_busy"), {}, {}};
	auto opened = s.host.openSealedTake(project, approval.takeId);
	if (!opened.source) return {QStringLiteral("take_saved_unattached"), {}, {}};
	const auto geometry = RecordingPlacement::captureGeometry(s.transport, opened.source->facts(), world,
		approval.placementRevision);
	const auto placement = RecordingPlacement::plan(opened.source->facts(), approval, geometry);
	if (!placement.plan) return {placement.code, {}, {}};
	return {QStringLiteral("ok"), std::make_shared<const PlacementPlan>(std::move(*placement.plan)),
		std::optional<SealedTakeToken>(std::move(*opened.source))};
}
RecordingReply RecordingSessionManager::confirmPlacement(const ManualPlacementApproval& approval)
{
	auto& s=*m_impl;s.assertOwner();
	if(s.active || s.commandDepth)return s.reply(false,"recording_busy");
	if(s.closing)return s.reply(false,"shutting_down");
	if(!opaqueId(approval.takeId) || !approval.expectedProjectGeneration || !approval.placementRevision)
		return s.reply(false,"manual_approval_required");
	const auto world=s.transport.snapshot();s.syncContext(world);
	if(approval.expectedProjectGeneration!=world.projectGeneration)return s.reply(false,"stale_project");
	RecordingStatus* takeStatus=nullptr;
	if(s.current.takeId==approval.takeId && s.current.captureIntegrity!=CaptureIntegrity::Pending)takeStatus=&s.current;
	else
	{
		const auto found=std::find_if(s.history.begin(),s.history.end(),[&](const auto& value){return value.takeId==approval.takeId;});
		if(found!=s.history.end())takeStatus=&*found;
	}
	if(!takeStatus)return s.reply(false,"stale_take");
	const bool wasAttached=takeStatus->placementState==PlacementState::Attached;
	if(takeStatus->projectGeneration!=approval.expectedProjectGeneration)return s.reply(false,"stale_project");
	if(takeStatus->captureIntegrity!=CaptureIntegrity::Complete || takeStatus->cancelled || !takeStatus->performanceReached)
	{
		if(!wasAttached)takeStatus->placementState=PlacementState::SavedUnattached;
		takeStatus->code="take_saved_unattached";
		return {false,takeStatus->code,*takeStatus};
	}
	if(takeStatus->placementState==PlacementState::Attached && !takeStatus->placementConfirmed)
		return {false,"placement_state_invalid",*takeStatus};
	const auto project=s.host.projectIdentity();
	if(project.id.isEmpty() || project.id!=world.projectId)return s.reply(false,"stale_project");
	auto opened=s.host.openSealedTake(project,approval.takeId);
	if(!opened.source)
	{
		if(!wasAttached) takeStatus->placementState=PlacementState::SavedUnattached;
		takeStatus->code=wasAttached?opened.code:"take_saved_unattached";
		return {false,takeStatus->code,*takeStatus};
	}
	const auto geometry=RecordingPlacement::captureGeometry(s.transport,opened.source->facts(),world,approval.placementRevision);
	const auto placement=RecordingPlacement::plan(opened.source->facts(),approval,geometry);
	if(!placement.plan)
	{
		if(!wasAttached) takeStatus->placementState=PlacementState::SavedUnattached;
		takeStatus->code=wasAttached?placement.code:"take_saved_unattached";
		return {false,takeStatus->code,*takeStatus};
	}
	const auto attached=s.attachment.attach(*opened.source,*placement.plan,approval.expectedProjectGeneration);
	if(!attached.attached)
	{
		if(!wasAttached)takeStatus->placementState=PlacementState::SavedUnattached;
		takeStatus->placementConfirmed=wasAttached;
		takeStatus->code=attached.code=="saved_unattached" && !wasAttached?"take_saved_unattached":publicCode(attached.code);
		return {false,takeStatus->code,*takeStatus};
	}
	takeStatus->placementState=PlacementState::Attached;
	takeStatus->placementConfirmed=true;
	takeStatus->placementQuality="uncalibrated";
	takeStatus->driftQuality="unmeasured";
	takeStatus->sampleAccuracyVerified=false;
	takeStatus->code=attached.code=="already_attached"?"already_attached":"take_attached";
	return {true,takeStatus->code,*takeStatus};
}
TakePage RecordingSessionManager::takes(PageRequest request) const
{
 auto& s=*m_impl;s.assertOwner();if(request.offset<0 || request.limit<1 || request.limit>100)return {"invalid_page"};
 TakePage page;page.code="ok";const auto begin=std::min<quint64>(quint64(request.offset),s.history.size());const auto end=std::min<quint64>(begin+unsigned(request.limit),s.history.size());
 for(auto i=begin;i<end;++i)page.takes.push_back(s.history[std::size_t(i)]);page.nextOffset=qint64(end);page.hasMore=end<s.history.size();return page;
}
bool RecordingSessionManager::quiescent() const{return !m_impl->active && !m_impl->commandDepth && !m_impl->playback.serial && m_impl->host.quiescent();}
RecordingStatus RecordingSessionManager::poll(quint64 now)
{
 auto& s=*m_impl;s.assertOwner();if(s.commandDepth)return status();s.now=now;
 const auto world=s.transport.snapshot();s.syncContext(world);
 if(s.active)
 {
  s.counts(s.host.snapshot());
  if(!s.stopping && (!sameConfiguration(s.expected,world) || world.seekRevision!=s.expected.seekRevision || world.stateRevision!=s.expected.stateRevision
   || s.host.rootRevision()!=s.admitted.rootRevision || s.host.localArmSnapshot().authorizationId!=s.admitted.authorizationId))
   s.queueStop(true,true,"transport_changed");
  if(s.stopping)std::ignore=s.stopWork();
 }
 if(s.active)s.saveSessionFacts();
 ++s.commandDepth;auto h=s.host.poll(now);--s.commandDepth;
 if(!s.active){s.base();return status();}
 s.counts(h);
 const auto error=inputFailure(h);
 if(!s.stopping && (!error.isEmpty() || h.input.input.stopReason!=InputStopReason::None || h.state==RecordingHostState::Finalizing || h.state==RecordingHostState::FailedRetained || h.quiescent))
 {s.queueStop(false,!s.current.performanceReached,error);std::ignore=s.stopWork();h=s.host.snapshot();}
 if(!s.stopping && !s.backingStarted)s.launch(h);
 const auto after=s.transport.snapshot();s.current.nativeTick=after.songTick;
 if(s.backingStarted && !s.stopping)
 {
  s.current.countInTicksRemaining=std::max<qint64>(0,s.current.intendedTargetTick-after.songTick);
  if(after.songTick>=s.current.intendedTargetTick && !s.current.performanceReached)
  {s.current.performanceReached=true;std::ignore=s.state->captureStarted(s.current.takeId);}
 }
 h=s.host.snapshot();s.counts(h);
 if(s.active && h.quiescent){if(!s.stopping)s.queueStop(false,!s.current.performanceReached);if(s.playback.serial)std::ignore=s.stopWork();s.finish(h);}
 else if(s.stopping && s.rawSeen && (h.input.state==RecordingControllerState::Quiescent || h.input.state==RecordingControllerState::Closed)
  && s.state->status().phase==Phase::Quiescing)std::ignore=s.state->quiesced(s.current.takeId);
 s.saveSessionFacts();s.base();return status();
}
namespace {
struct NativeRevisions
{
 std::atomic<quint64> context{0},seek{0},state{0};std::atomic_bool valid{true};
 void bump(std::atomic<quint64>& revision) noexcept{if(revision.fetch_add(1,std::memory_order_acq_rel)==std::numeric_limits<quint64>::max())valid.store(false,std::memory_order_release);}
};
class NativeControl final : public QObject,public RecordingTransportControl
{
public:
 explicit NativeControl(Song& owner):song(&owner)
 {
  const auto counters=revisions;
  connect(&owner,&Song::projectAboutToChange,this,[counters]{counters->bump(counters->context);},Qt::DirectConnection);
  connect(&owner,&Song::projectLoaded,this,[counters]{counters->bump(counters->context);},Qt::DirectConnection);
  connect(&owner,&Song::projectFileNameChanged,this,[counters]{counters->bump(counters->context);},Qt::DirectConnection);
  connect(&owner,&Song::tempoChanged,this,[counters]{counters->bump(counters->context);},Qt::DirectConnection);
  connect(&owner,&Song::tempoAutomationTopologyChanged,this,[counters]{counters->bump(counters->context);},Qt::DirectConnection);
  connect(&owner,&Song::timeSignatureChanged,this,[counters]{counters->bump(counters->context);},Qt::DirectConnection);
  connect(&owner,&Song::playbackStateChanged,this,[counters]{counters->bump(counters->state);},Qt::DirectConnection);
  auto& timeline=owner.getTimeline(Song::PlayMode::Song);
  connect(&timeline,&Timeline::positionJumped,this,[counters]{counters->bump(counters->seek);},Qt::DirectConnection);
  connect(&timeline,&Timeline::loopEnabledChanged,this,[counters]{counters->bump(counters->context);},Qt::DirectConnection);
  if(auto* audio=Engine::audioEngine())connect(audio,&AudioEngine::sampleRateChanged,this,[counters]{counters->bump(counters->context);},Qt::DirectConnection);
 }
 ~NativeControl() override{disconnect();}
 RecordingTransportSnapshot snapshot() const override
 {
  Q_ASSERT(QThread::currentThread()==thread());RecordingTransportSnapshot s;
  auto* owner=song.data();auto* audio=Engine::audioEngine();if(!owner || !audio || !revisions->valid.load(std::memory_order_acquire))return s;
  auto guard=audio->requestChangesGuard();
  s.available=true;s.outputCaptureSafe=audio->audioDev() && RecordingAudioBackendPolicy::independentInputOutputAllowed(audio->audioDevName(),audio->audioDev()->supportsCapture(),RecordingAudioBackendPolicy::qualificationBuild());s.playing=owner->isPlaying();s.paused=owner->isPaused();s.exporting=owner->isExporting();s.songMode=owner->playMode()==Song::PlayMode::Song;
  s.projectGeneration=owner->recordingProjectIdentity().revision();s.projectId=owner->recordingProjectIdentity().cachedIdentity().id;
  s.songTick=owner->getPlayPos(Song::PlayMode::Song).getTicks();s.looping=owner->getTimeline(Song::PlayMode::Song).loopEnabled();
  s.tempo=owner->getTempo();s.numerator=owner->getTimeSigModel().getNumerator();s.denominator=owner->getTimeSigModel().getDenominator();s.ticksPerBar=owner->ticksPerBar();
  s.sampleRate=audio->outputSampleRate();s.metronomeActive=owner->metronome().active();s.constantTempo=!owner->tempoModel().isAutomatedOrControlled();
  s.stableMeter=!owner->getTimeSigModel().numeratorModel().isAutomatedOrControlled() && !owner->getTimeSigModel().denominatorModel().isAutomatedOrControlled();
  s.contextRevision=revisions->context.load(std::memory_order_acquire);s.seekRevision=revisions->seek.load(std::memory_order_acquire);s.stateRevision=revisions->state.load(std::memory_order_acquire);return s;
 }
 RecordingTransportResult seekSong(qint64 tick) override
 {
  const auto before=snapshot();if(!before.available)return {false,"transport_unavailable"};
  if(tick<0 || tick>std::numeric_limits<int>::max())return {false,"invalid_tick"};
  if(before.playing || before.paused || before.exporting)return {false,"transport_busy"};
  // Transport is stopped. No file, stream or join operation is performed while a model guard is held.
  song->setPlayPos(int(tick),Song::PlayMode::Song);
  const auto after=snapshot();const bool ok=after.available && !after.playing && !after.paused && !after.exporting && after.songTick==tick
   && after.projectGeneration==before.projectGeneration && after.contextRevision==before.contextRevision && after.stateRevision==before.stateRevision && after.seekRevision==before.seekRevision+1;
  return {ok,ok?QString("ok"):QString("transport_changed")};
 }
 RecordingTransportResult playSong() override
 {
  const auto before=snapshot();if(!before.available)return {false,"transport_unavailable"};
  if(before.playing || before.paused || before.exporting || before.looping || nextSerial==std::numeric_limits<quint64>::max())return {false,"transport_busy"};
  song->playSong();const auto after=snapshot();
  if(after.available && after.songMode && after.playing && !after.paused && after.stateRevision==before.stateRevision+1)
   ownership={++nextSerial,after.stateRevision};
  else ownership={};
  const bool ok=ownership.serial && after.projectGeneration==before.projectGeneration && after.contextRevision==before.contextRevision && after.seekRevision==before.seekRevision;
  return {ok,ok?QString("ok"):QString("transport_changed"),ownership};
 }
 RecordingTransportResult stopOwned(RecordingPlaybackOwnership expected) override
 {
  const auto current=snapshot();if(!expected.serial || expected.serial!=ownership.serial)return {true,"ownership_lost"};
  ownership={};
  if(!current.available || (!current.playing && !current.paused))return {true,"already_stopped"};
  if(!current.songMode || (current.stateRevision!=expected.stateRevision && !(current.paused && current.stateRevision==expected.stateRevision+1)))return {true,"ownership_lost"};
  song->stop();return {true,"stopped"};
 }
private:
 QPointer<Song> song;
 // Callbacks retain only this POD counter sink, never a manager/model pointer.
 std::shared_ptr<NativeRevisions> revisions=std::make_shared<NativeRevisions>();
 quint64 nextSerial=0;RecordingPlaybackOwnership ownership;
};
}
std::unique_ptr<RecordingTransportControl> createNativeRecordingTransport(Song& song){return std::make_unique<NativeControl>(song);}
}
