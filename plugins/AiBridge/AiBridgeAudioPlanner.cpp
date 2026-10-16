#include "AiBridgeAudioPlanner.h"
#include <QJsonDocument>
#include <QCryptographicHash>
#include <algorithm>
#include <cmath>
#include <limits>
#include "Note.h"
namespace lmms {
QString audioWorldHash(const AiBridgeAudioWorld& world)
{
	auto tracks=world.tracks;std::sort(tracks.begin(),tracks.end(),[](const auto& a,const auto& b){return a.id<b.id;});
	QJsonArray rows;for(const auto& t:tracks) rows.append(QJsonObject{{"id",t.id},{"name",t.name},{"volume",t.volume},{"panning",t.panning},{"muted",t.muted},{"fx",t.fxChannel}});
	const QJsonObject data{{"generation",QString::number(world.generation)},{"tempo",world.tempo},{"ticks_per_bar",double(world.ticksPerBar)},
		{"output_frames_per_tick",world.outputFramesPerTick},{"tempo_automated",world.tempoAutomated},{"tracks",rows},{"clips",audioStateHash(world.clips)}};
	return QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(data).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}
AiBridgeAudioPlan AiBridgeAudioPlanner::plan(const AiBridgeAudioWorld& initial,const std::vector<AiBridgeAudioEdit>& edits)
{
	using Kind=AiBridgeAudioEditKind;
	if(edits.empty() || edits.size()>MaxSteps || initial.clips.size()>MaxCollisionRows) return {"invalid_argument"};
	if(initial.tempoAutomated) return {"unsupported_tempo_map"};
	if(!std::isfinite(initial.outputFramesPerTick) || initial.outputFramesPerTick<=0 || audioStateHash(initial.clips).isEmpty()) return {"unsupported_audio_operation"};
	const qint64 maximum=std::numeric_limits<qint32>::max();QSet<QString> affected;int newClips=0;
	for(const auto& track:initial.tracks) if(!std::isfinite(track.volume) || !std::isfinite(track.panning)) return {"unsupported_audio_operation"};
	for(const auto& row:initial.clips) if(row.length<1 || row.length>maximum || row.position<0 || row.position>maximum-row.length) return {"unsupported_audio_operation"};
	for(const auto& request:edits)
	{
		if(request.kind!=Kind::Move && request.kind!=Kind::Duplicate && request.kind!=Kind::Delete) return {"invalid_argument"};
		if(request.kind==Kind::Delete ? request.position.has_value() || !request.destinationTrackId.isEmpty() : !request.position.has_value()) return {"invalid_argument"};
		const auto found=std::find_if(initial.clips.begin(),initial.clips.end(),[&](const auto& s){return s.id==request.clipId;});
		if(found==initial.clips.end() || !initial.selected.contains(request.clipId)) return {"stale_id"};
		if(request.expectedAudioStateHash.isEmpty() || audioStateHash({*found})!=request.expectedAudioStateHash) return {"clip_state_mismatch"};
		if(request.kind!=Kind::Delete)
		{
			// These transient native overrides are not serialized by SampleClip.
			if(found->playbackFrequency!=DefaultBaseFreq || found->amplification!=1.0 || found->solo || !found->nativeName.isEmpty()) return {"unsupported_audio_operation"};
			if(!found->color.isEmpty() && (found->color.size()!=9 || !found->color.startsWith("#ff",Qt::CaseInsensitive))) return {"unsupported_audio_operation"};
			if(request.kind==Kind::Duplicate && found->sourceStatus=="unverified_legacy") return {"unsupported_audio_operation"};
			if(found->sourceStatus!="available" && found->sourceStatus!="embedded" && found->sourceStatus!="unverified_legacy")
				return {found->sourceStatus.startsWith("audio_") || found->sourceStatus.startsWith("import_") || found->sourceStatus=="source_too_large" ? found->sourceStatus : QStringLiteral("unsupported_audio_operation")};
			const double offsetFrames=double(float(found->offset)*float(initial.outputFramesPerTick));
			if(found->length<1 || found->length>maximum || found->sourceFrames<1 || found->sourceFrames>quint64(maximum) || found->sampleRate<1
				|| offsetFrames>=2147483648.0 || offsetFrames< -2147483648.0) return {"unsupported_audio_operation"};
			if(*request.position<0 || *request.position>maximum-found->length) return {"invalid_argument"};
			if(request.kind==Kind::Move && !request.destinationTrackId.isEmpty() && request.destinationTrackId!=found->trackId) return {"unsupported_audio_operation"};
		}
		if(request.kind==Kind::Duplicate)
		{
			if(request.destinationTrackId.isEmpty()) return {"invalid_argument"};
			if(std::none_of(initial.tracks.begin(),initial.tracks.end(),[&](const auto& t){return t.id==request.destinationTrackId;})) return {"stale_id"};
			++newClips;
		}
		affected.insert(request.clipId);
	}
	if(affected.size()+newClips>MaxAffectedClips) return {"invalid_argument"};
	AiBridgeAudioPlan result;result.initial=initial;result.affectedClips=affected.size()+newClips;auto projected=initial;bool changed=false;
	for(size_t index=0;index<edits.size();++index)
	{
		const auto& r=edits[index];const auto found=std::find_if(projected.clips.begin(),projected.clips.end(),[&](const auto& s){return s.id==r.clipId;});
		if(found==projected.clips.end()) return {"invalid_argument"};
		AiBridgeAudioPlanStep step;step.index=int(index);step.kind=r.kind;step.before=*found;step.beforeWorldHash=audioWorldHash(projected);
		QString operation;
		if(r.kind==Kind::Delete){operation="delete";projected.clips.erase(found);projected.selected.remove(r.clipId);}
		else
		{
			auto after=*found;after.position=*r.position;
			if(r.kind==Kind::Duplicate)
			{
				if(projected.clips.size()>=MaxCollisionRows) return {"result_too_large"};
				operation="duplicate";after.id=QStringLiteral("@copy:%1").arg(index);after.trackId=r.destinationTrackId;
				const auto track=std::find_if(projected.tracks.begin(),projected.tracks.end(),[&](const auto& t){return t.id==after.trackId;});
				after.trackVolume=track->volume;after.trackPanning=track->panning;after.trackMuted=track->muted;after.fxChannel=track->fxChannel;
			}
			else {operation="move";step.changed=after.position!=step.before.position;}
			if(step.changed) for(const auto& existing:projected.clips)
			{
				if(r.kind==Kind::Move && existing.id==r.clipId) continue;
				if(existing.trackId==after.trackId && after.position<existing.position+existing.length && after.position+after.length>existing.position) return {"destination_conflict"};
			}
			step.after=after;
			if(r.kind==Kind::Duplicate){projected.clips.push_back(after);projected.selected.insert(after.id);}else *found=after;
		}
		changed=changed || step.changed;step.afterWorldHash=audioWorldHash(projected);result.steps.push_back(step);
		if(result.previewHasMore) continue;
		QJsonObject row{{"step",int(index)},{"operation",operation},{"clip_id",r.clipId},{"changed",step.changed}};
		if(step.after){row.insert("track_id",step.after->trackId);row.insert("position_ticks",double(step.after->position));row.insert("length_ticks",double(step.after->length));row.insert("source_offset_ticks",double(step.after->offset));}
		if(r.kind==Kind::Duplicate) row.insert("result_ref",step.after->id);
		if(result.preview.size()<MaxPreviewRows)
		{
			auto next=result.preview;next.append(row);
			if(QJsonDocument(next).toJson(QJsonDocument::Compact).size()<=32*1024) result.preview=next;else result.previewHasMore=true;
		}
		else result.previewHasMore=true;
	}
	result.code=changed ? "ok" : "no_effect";return result;
}
}
