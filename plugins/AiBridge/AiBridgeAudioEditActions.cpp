#include "AiBridgeAudioActions.h"
#include "Engine.h"
#include "AudioEngine.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include <QCoreApplication>
#include <QThread>
#include <QHash>
#include <algorithm>
namespace lmms {
namespace {
using Kind=AiBridgeAudioEditKind;
struct Capture {QString code;AiBridgeAudioWorld world;};
bool applicationThread(){return QCoreApplication::instance() && QThread::currentThread()==QCoreApplication::instance()->thread();}
Capture capture(QStringList trackIds,const QSet<QString>& selected,const QHash<QString,QString>& aliases)
{
	Capture result;result.code="ok";auto& w=result.world;AiBridgeProjectContext ids;
	if(!Engine::getSong()) return {"stale_id",{}};
	w.generation=ids.projectGeneration();w.tempo=Engine::getSong()->getTempo();w.ticksPerBar=TimePos::ticksPerBar();w.outputFramesPerTick=Engine::framesPerTick();w.tempoAutomated=Engine::getSong()->tempoModel().isAutomatedOrControlled();w.selected=selected;
	std::sort(trackIds.begin(),trackIds.end());trackIds.removeDuplicates();
	for(const auto& id:trackIds)
	{
		auto* object=ids.resolveTrack(id);if(!object || std::find(Engine::getSong()->tracks().begin(),Engine::getSong()->tracks().end(),object)==Engine::getSong()->tracks().end()) return {"stale_id",{}};
		auto* track=dynamic_cast<SampleTrack*>(object);if(!track)return {"unsupported_audio_operation",{}};
		if(w.clips.size()+track->getClips().size()>AiBridgeAudioPlanner::MaxCollisionRows) return {"result_too_large",{}};
		w.tracks.push_back({id,track->displayName(),track->volumeModel()->value(),track->panningModel()->value(),track->isMuted(),track->mixerChannelModel()->value(),track->numOfClips()});
		for(auto* clip:track->getClips())
		{
			const auto actual=ids.idForClip(clip);const auto logical=aliases.value(actual,actual);AiBridgeAudioClipState row;
			if(selected.contains(logical))
			{
				auto copied=AiBridgeAudioContext{}.clip(actual);if(!copied.value)return {copied.code,{}};
				row=std::move(copied.value->state);row.id=logical;
			}
			else {row.id=logical;row.trackId=id;row.position=clip->startPosition().getTicks();row.length=clip->length().getTicks();row.offset=clip->startTimeOffset().getTicks();row.sourceStatus="collision_only";}
			w.clips.push_back(std::move(row));
		}
	}
	return result;
}
QJsonArray report(const AiBridgeAudioWorld& world,const QHash<QString,QString>& actualIds)
{
	QJsonArray rows;auto clips=world.clips;std::sort(clips.begin(),clips.end(),[](const auto& a,const auto& b){return a.id<b.id;});
	for(auto s:clips)if(world.selected.contains(s.id))
	{
		s.id=actualIds.value(s.id,s.id);
		rows.append(QJsonObject{{"clip_id",s.id},{"track_id",s.trackId},{"position_ticks",double(s.position)},{"length_ticks",double(s.length)},
			{"source_offset_ticks",double(s.offset)},{"source_status",s.sourceStatus},{"audio_state_hash",audioStateHash({s})}});
	}
	return rows;
}
QJsonArray indices(const std::vector<int>& values){QJsonArray result;for(int value:values)result.append(value);return result;}
void replaceProjected(AiBridgeAudioWorld& projected,const AiBridgeAudioPlanStep& step)
{
	auto found=std::find_if(projected.clips.begin(),projected.clips.end(),[&](const auto& s){return s.id==step.before.id;});
	if(step.kind==Kind::Delete){projected.clips.erase(found);projected.selected.remove(step.before.id);}
	else if(step.kind==Kind::Duplicate){projected.clips.push_back(*step.after);projected.selected.insert(step.after->id);}
	else *found=*step.after;
}
}
AiBridgeAudioEditResult AiBridgeAudioActions::editClip(const AiBridgeAudioEdit& edit,bool dryRun) const {return batchEdit({edit},dryRun);}
AiBridgeAudioEditResult AiBridgeAudioActions::batchEdit(const std::vector<AiBridgeAudioEdit>& edits,bool dryRun) const
{
	if(!applicationThread())return {"wrong_thread"};
	if(edits.empty() || edits.size()>AiBridgeAudioPlanner::MaxSteps)return {"invalid_argument"};
	QSet<QString> selected;QStringList trackIds;AiBridgeProjectContext ids;int copies=0;
	for(const auto& r:edits)
	{
		if(r.kind!=Kind::Move && r.kind!=Kind::Duplicate && r.kind!=Kind::Delete)return {"invalid_argument"};
		selected.insert(r.clipId);if(r.kind==Kind::Duplicate)++copies;
	}
	if(selected.size()+copies>AiBridgeAudioPlanner::MaxAffectedClips)return {"invalid_argument"};
	for(const auto& r:edits)
	{
		auto* clip=ids.resolveClip(r.clipId);if(!clip)return {"stale_id"};
		if(!dynamic_cast<SampleClip*>(clip) || !dynamic_cast<SampleTrack*>(clip->getTrack()))return {"unsupported_audio_operation"};
		trackIds<<ids.idForTrack(clip->getTrack());if(!r.destinationTrackId.isEmpty())trackIds<<r.destinationTrackId;
	}
	auto initial=capture(trackIds,selected,{});if(initial.code!="ok")return {initial.code};
	const auto plan=AiBridgeAudioPlanner::plan(initial.world,edits);if(plan.code!="ok" && plan.code!="no_effect")return {plan.code};
	AiBridgeAudioEditResult result;result.code=plan.code;result.preview=plan.preview;result.previewHasMore=plan.previewHasMore;auto projected=plan.initial;
	QHash<QString,QString> aliases,actualIds;
	result.expected=report(projected,actualIds);result.observed=result.expected;
	if(dryRun)return result;
	auto stop=[&](int index,const QString& code,const QString& reason,const Capture& actual)
	{
		result.code=code;result.failureCode=reason;result.failed.push_back(index);
		for(int later=index+1;later<int(plan.steps.size());++later)result.unapplied.push_back(later);
		result.expected=report(projected,actualIds);result.observed=report(actual.world,actualIds);result.observedWorldHash=actual.code=="ok"?audioWorldHash(actual.world):QString{};
		result.affectedClipIds.removeDuplicates();
	};
	for(const auto& step:plan.steps)
	{
		auto before=capture(trackIds,projected.selected,aliases);result.expectedWorldHash=step.beforeWorldHash;
		if(before.code!="ok" || audioWorldHash(before.world)!=step.beforeWorldHash)
		{stop(step.index,result.applied.empty()?QStringLiteral("plan_stale"):QStringLiteral("partial_failure"),before.code=="ok"?QStringLiteral("plan_stale"):before.code,before);return result;}
		if(!step.changed){result.noEffect.push_back(step.index);result.observedWorldHash=audioWorldHash(before.world);continue;}
		auto* source=dynamic_cast<SampleClip*>(ids.resolveClip(step.before.id));if(!source){stop(step.index,"partial_failure","stale_id",before);return result;}
		QString nativeError;
		try {
			const auto guard=Engine::audioEngine()->requestChangesGuard();
			if(step.kind==Kind::Move){source->addJournalCheckPoint();result.affectedClipIds<<step.before.id;source->movePosition(TimePos(int(step.after->position)));}
			else if(step.kind==Kind::Delete){source->getTrack()->addJournalCheckPoint();result.affectedClipIds<<step.before.id;delete source;}
			else
			{
				auto* target=dynamic_cast<SampleTrack*>(ids.resolveTrack(step.after->trackId));
				if(!target) nativeError="stale_id";
				else
				{
					target->addJournalCheckPoint();auto* copy=source->cloneToTrack(target);
					if(!copy) nativeError="native_copy_failed";
					else
					{
						const auto id=ids.idForClip(copy);result.createdClipIds<<id;result.affectedClipIds<<id;aliases.insert(id,step.after->id);actualIds.insert(step.after->id,id);
						copy->movePosition(TimePos(int(step.after->position)));
					}
				}
			}
		}
		catch(...) {nativeError="native_operation_failed";}
		if(!nativeError.isEmpty())
		{
			auto actualSelected=projected.selected;for(const auto& logical:aliases)actualSelected.insert(logical);
			auto actual=capture(trackIds,actualSelected,aliases);
			if(actual.code!="ok" || audioWorldHash(actual.world)!=step.beforeWorldHash || !result.affectedClipIds.empty())Engine::getSong()->setModified();
			stop(step.index,"partial_failure",nativeError,actual);return result;
		}
		Engine::getSong()->setModified();replaceProjected(projected,step);auto after=capture(trackIds,projected.selected,aliases);result.expectedWorldHash=step.afterWorldHash;
		if(after.code!="ok" || audioWorldHash(after.world)!=step.afterWorldHash)
		{stop(step.index,"post_verification_failed",after.code=="ok"?QStringLiteral("clip_state_mismatch"):after.code,after);return result;}
		result.applied.push_back(step.index);result.expected=report(projected,actualIds);result.observed=report(after.world,actualIds);result.observedWorldHash=audioWorldHash(after.world);
	}
	result.affectedClipIds.removeDuplicates();return result;
}
QJsonObject AiBridgeAudioEditResult::toJson() const
{
	return {{"code",code},{"failure_code",failureCode},{"preview",preview},{"preview_has_more",previewHasMore},
		{"applied",indices(applied)},{"failed",indices(failed)},{"unapplied",indices(unapplied)},{"no_effect",indices(noEffect)},
		{"affected_clip_ids",QJsonArray::fromStringList(affectedClipIds)},{"created_clip_ids",QJsonArray::fromStringList(createdClipIds)},
		{"expected",expected},{"observed",observed},{"expected_world_hash",expectedWorldHash},{"observed_world_hash",observedWorldHash}};
}
}
