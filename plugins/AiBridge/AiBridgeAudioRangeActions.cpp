#include "AiBridgeAudioRangeActions.h"
#include "AudioEngine.h"
#include "AudioWindowPlayback.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include <QCoreApplication>
#include <QThread>
#include <QSet>
#include <algorithm>
namespace lmms {
#ifdef LMMS_AUDIO_RANGE_TESTING
namespace { AudioRangeTestHook testHook=nullptr; }
void setAudioRangeTestHook(AudioRangeTestHook hook) {testHook=hook;}
#define RANGE_HOOK(stage,index,clip) do {if(testHook)testHook(AudioRangeTestStage::stage,index,clip);} while(false)
#else
#define RANGE_HOOK(stage,index,clip) do {} while(false)
#endif
namespace {
bool applicationThread(){return QCoreApplication::instance() && QThread::currentThread()==QCoreApplication::instance()->thread();}
struct Capture {QString code;AiBridgeAudioRangeWorld world;};
Capture capture(const QString& trackId)
{
	AiBridgeProjectContext ids;auto* song=Engine::getSong();if(!song) return {"stale_id",{}};
	auto* track=dynamic_cast<SampleTrack*>(ids.resolveTrack(trackId));
	if(!track || std::find(song->tracks().begin(),song->tracks().end(),track)==song->tracks().end()) return {"stale_id",{}};
	if(track->getClips().size()>4096) return {"result_too_large",{}};
	Capture result;result.code="ok";auto& w=result.world;w.generation=ids.projectGeneration();
	w.nativeWorld={song->getTempo(),int(Engine::audioEngine()->outputSampleRate()),TimePos::ticksPerBar()/song->getTimeSigModel().getNumerator(),song->tempoModel().isAutomatedOrControlled()};
	// Revoke/reapprove, even with identical text, invalidates a previously copied plan.
	w.fingerprint=QString::number(ConfigManager::inst()->audioImportPolicyRevision());
	for(auto* clip:track->getClips())
	{
		auto row=AiBridgeAudioContext{}.clip(ids.idForClip(clip));if(!row.value) return {row.code,{}};
		w.clips.push_back(std::move(*row.value));
	}
	return result;
}
QJsonObject description(const AiBridgeAudioClipState& s,bool hasId=true)
{
	QJsonObject row{{"track_id",s.trackId},{"position_ticks",double(s.position)},{"length_ticks",double(s.length)},
		{"source_offset_ticks",double(s.offset)}};
	if(hasId){row.insert("clip_id",s.id);row.insert("audio_state_hash",audioStateHash({s}));}
	if(s.rangeWindowV2)
	{
		const auto& w=*s.rangeWindowV2;
		row.insert("source_in_frame",double(w.base.inFrame));row.insert("source_out_frame",double(w.base.outFrame));
	}
	return row;
}
AiBridgeAudioClipSnapshot projected(const AiBridgeAudioClipSnapshot& before,const AiBridgeAudioRangeStep& step,const QString& id)
{
	auto row=before;auto& s=row.state;s.id=id;s.position=step.geometry.positionTicks;s.length=step.geometry.lengthTicks;s.offset=step.geometry.offsetTicks;
	s.rangeWindow.reset();s.rangeWindowV2=step.window;s.rangeStatus="ok";row.endTick=s.position+s.length;row.stateHash=audioStateHash({s});return row;
}
QJsonArray report(const AiBridgeAudioRangeWorld& world,const QStringList& ids)
{
	QJsonArray result;for(const auto& id:ids)for(const auto& row:world.clips)if(row.state.id==id)result.append(description(row.state));return result;
}
bool sameSteps(const AiBridgeAudioRangePlan& a,const AiBridgeAudioRangePlan& b)
{
	if(a.code!=b.code || a.steps.size()!=b.steps.size())return false;
	for(size_t i=0;i<a.steps.size();++i)
	{
		if(a.steps[i].kind!=b.steps[i].kind) return false;
		// Hash all native metadata, including private reference-end/phase fields.
		if(projected(a.before,a.steps[i],a.before.state.id).stateHash!=projected(b.before,b.steps[i],b.before.state.id).stateHash) return false;
		const auto& x=a.steps[i].geometry;const auto& y=b.steps[i].geometry;
		if(x.autoResize!=y.autoResize || x.reversed!=y.reversed || x.solo!=y.solo || x.nativeName!=y.nativeName
			|| x.color!=y.color || x.frequency!=y.frequency || x.amplification!=y.amplification) return false;
	}
	return true;
}
}
AiBridgeAudioRangePlan prepareAudioRange(const AiBridgeAudioRangeRequest& request)
{
	if(!applicationThread()) return {"wrong_thread"};
	const auto clip=AiBridgeAudioContext{}.clip(request.clipId);if(!clip.value)return {clip.code};
	const auto current=capture(clip.value->state.trackId);if(current.code!="ok") return {current.code};
	auto plan=planAudioRange(request,*clip.value,current.world);
	if((plan.code=="ok" || plan.code=="no_effect") && !verifyAudioWindowReferenceProfile()) return {"unsupported_audio_operation"};
	return plan;
}
AiBridgeAudioEditResult executeAudioRange(const AiBridgeAudioRangePlan& plan,bool dryRun)
{
	if(!applicationThread()) return {"wrong_thread"};
	if(plan.code!="ok" && plan.code!="no_effect") return {plan.code};
	const auto current=capture(plan.before.state.trackId);if(current.code!="ok")return {current.code};
	if(plan.expectedWorldHash.isEmpty() || audioRangeWorldHash(current.world)!=plan.expectedWorldHash) return {"plan_stale"};
	if(plan.steps.size()>2 || (plan.code=="ok" && plan.steps.empty()) || (plan.code=="no_effect" && !plan.steps.empty()))return {"invalid_argument"};
	// A retained internal plan is checked again, including exact step contents.
	if(!plan.steps.empty())
	{
		const auto kind=plan.steps.size()==2?AiBridgeAudioRangeKind::Split:
			plan.steps[0].geometry.positionTicks!=plan.before.state.position?AiBridgeAudioRangeKind::TrimLeft:AiBridgeAudioRangeKind::TrimRight;
		const qint64 boundary=kind==AiBridgeAudioRangeKind::TrimLeft?plan.steps[0].geometry.positionTicks:
			plan.steps[0].geometry.positionTicks+plan.steps[0].geometry.lengthTicks;
		const auto checked=planAudioRange({kind,plan.before.state.id,plan.before.stateHash,boundary},plan.before,current.world);
		if(!sameSteps(plan,checked)) return {"invalid_argument"};
	}
	AiBridgeAudioEditResult result;result.code=plan.code;result.expectedWorldHash=plan.expectedWorldHash;
	result.observedWorldHash=plan.expectedWorldHash;
	QStringList selected{plan.before.state.id};
	result.observed=report(current.world,selected);
	for(size_t i=0;i<plan.steps.size();++i)
	{
		const bool left=plan.steps[i].kind==AiBridgeAudioRangeStep::Kind::PublishLeft;
		auto row=description(projected(plan.before,plan.steps[i],plan.before.state.id).state,left);
		auto original=plan.before.state;
		if(!original.rangeWindowV2)
		{
			const auto& w=plan.steps.front().window;
			original.rangeWindowV2=AudioSourceWindowV2{{-original.offset*w.base.framesPerTick,
				(-original.offset+original.length)*w.base.framesPerTick,w.base.phaseOriginFrame,w.base.sourceFrameCount,
				w.base.sourceRate,w.base.referenceTempoBpm,w.base.referenceOutputRate,w.base.ticksPerBeat,
				w.base.framesPerTick,qint32(original.length),qint32(original.offset)},w.referenceEndFrame};
		}
		auto before=description(original);before.insert("audio_state_hash",plan.before.stateHash);row.insert("before",before);
		const auto& first=plan.steps.front();const qint64 boundary=first.geometry.positionTicks!=plan.before.state.position
			?first.geometry.positionTicks:first.geometry.positionTicks+first.geometry.lengthTicks;
		row.insert("boundary_tick",double(boundary));row.insert("boundary_frame",double(original.rangeWindowV2->base.inFrame+
			(boundary-plan.before.state.position)*first.window.base.framesPerTick));
		row.insert("step",int(i));row.insert("operation",left?"publish_left":"create_right");result.preview.append(row);result.expected.append(row);
	}
	if(dryRun) return result;
	if(plan.code=="no_effect"){result.noEffect={0};return result;}
	AiBridgeProjectContext ids;auto* source=dynamic_cast<SampleClip*>(ids.resolveClip(plan.before.state.id));
	if(!source)return {"stale_id"};
	auto* track=dynamic_cast<SampleTrack*>(source->getTrack());if(!track)return {"unsupported_audio_operation"};
	auto expected=current.world;
	const auto changes=Engine::audioEngine()->requestChangesGuard();
	for(size_t i=0;i<plan.steps.size();++i)
	{
		const auto& step=plan.steps[i];QString error;bool postFailure=false;SampleClip* target=source;
		const auto before=capture(plan.before.state.trackId);
		if(before.code!="ok" || audioRangeWorldHash(before.world)!=audioRangeWorldHash(expected)) error="plan_stale";
		QSet<QString> known;for(const auto& row:before.world.clips)known.insert(row.state.id);
		try
		{
			if(error.isEmpty())
			{
				RANGE_HOOK(BeforeWrite,int(i),source);
				if(step.kind==AiBridgeAudioRangeStep::Kind::CreateRight)
				{
					track->addJournalCheckPoint();target=source->cloneToTrack(track);
					if(!target)error="native_copy_failed";
					else
					{
						const auto id=ids.idForClip(target);selected<<id;result.createdClipIds<<id;result.affectedClipIds<<id;
						const auto intended=projected(plan.before,step,id);expected.clips.push_back(intended);
						auto row=result.expected[int(i)].toObject();row.insert("clip_id",id);row.insert("audio_state_hash",intended.stateHash);result.expected[int(i)]=row;
						Engine::getSong()->setModified();RANGE_HOOK(AfterConstruction,int(i),target);
					}
				}
				if(error.isEmpty())
				{
					if(step.kind==AiBridgeAudioRangeStep::Kind::PublishLeft)
						for(auto& row:expected.clips)if(row.state.id==plan.before.state.id)row=projected(plan.before,step,row.state.id);
					error=target->applyAudioWindow(step.window,step.geometry);
					if(error=="ok")
					{
						error.clear();result.affectedClipIds<<ids.idForClip(target);RANGE_HOOK(AfterPublication,int(i),target);
						RANGE_HOOK(BeforeVerification,int(i),target);
					}
				}
			}
		}
		catch(...) {error="native_operation_failed";}
		const auto after=capture(plan.before.state.trackId);
		// Capture native objects created even when a virtual constructor failed after registering one.
		for(const auto& row:after.world.clips)if(!known.contains(row.state.id) && !result.createdClipIds.contains(row.state.id))
		{result.createdClipIds<<row.state.id;result.affectedClipIds<<row.state.id;selected<<row.state.id;}
		// Native notification exceptions can follow a write but precede its return.
		for(const auto& row:after.world.clips)if(selected.contains(row.state.id))
		{
			const auto prior=std::find_if(before.world.clips.begin(),before.world.clips.end(),[&](const auto& c){return c.state.id==row.state.id;});
			if(prior==before.world.clips.end())result.affectedClipIds<<row.state.id;
			else
			{
				// Source availability/policy can change without any native clip write.
				// Retain those differences in observed state/world hash, not as invented effects.
				auto canonical=row.state;canonical.sourceStatus=prior->state.sourceStatus;canonical.rangeStatus=prior->state.rangeStatus;
				if(audioStateHash({canonical})!=prior->stateHash)result.affectedClipIds<<row.state.id;
			}
		}
		if(error.isEmpty() && (after.code!="ok" || audioRangeWorldHash(after.world)!=audioRangeWorldHash(expected)))
		{error="clip_state_mismatch";postFailure=true;}
		result.expectedWorldHash=audioRangeWorldHash(expected);result.observedWorldHash=after.code=="ok"?audioRangeWorldHash(after.world):QString{};
		result.observed=report(after.world,selected);result.affectedClipIds.removeDuplicates();
		if(!error.isEmpty())
		{
			const bool effects=!result.affectedClipIds.empty();
			if(effects)Engine::getSong()->setModified();
			result.code=postFailure?"post_verification_failed":effects?"partial_failure":error;
			result.failureCode=error;result.failed.push_back(int(i));for(size_t j=i+1;j<plan.steps.size();++j)result.unapplied.push_back(int(j));return result;
		}
		result.applied.push_back(int(i));
	}
	return result;
}
#undef RANGE_HOOK
}
