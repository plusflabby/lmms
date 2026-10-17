#include "AiBridgeAudioRangePlanner.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSet>
#include <algorithm>
#include <climits>
namespace lmms {
QString audioRangeWorldHash(const AiBridgeAudioRangeWorld& world)
{
	if(world.clips.size()>4096) return {};
	std::vector<AiBridgeAudioClipState> states;QSet<QString> ids;
	for(const auto& clip:world.clips)
	{
		if(ids.contains(clip.state.id) || clip.generation!=world.generation) return {};
		ids.insert(clip.state.id);states.push_back(clip.state);
	}
	const auto hash=audioStateHash(states);if(hash.isEmpty()) return {};
	const auto& w=world.nativeWorld;
	const QJsonObject row{{"generation",QString::number(world.generation)},{"clips",hash},
		{"tempo",w.tempoBpm},{"rate",w.outputRate},{"ticks_per_beat",w.ticksPerBeat},
		{"tempo_automated",w.tempoAutomationPresent},{"topology",world.fingerprint}};
	return QString::fromLatin1(QCryptographicHash::hash(QByteArray("aibridge-audio-range-world-v2:")+
		QJsonDocument(row).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
}
AiBridgeAudioRangePlan planAudioRange(const AiBridgeAudioRangeRequest& r,const AiBridgeAudioClipSnapshot& clip,const AiBridgeAudioRangeWorld& world)
{
	using Kind=AiBridgeAudioRangeKind;
	if(r.kind!=Kind::TrimLeft && r.kind!=Kind::TrimRight && r.kind!=Kind::Split) return {"invalid_argument"};
	if(r.clipId!=clip.state.id || clip.generation!=world.generation) return {"stale_id"};
	const auto& s=clip.state;const auto hash=audioStateHash({s});
	if(r.expectedAudioStateHash.isEmpty() || hash.isEmpty() || hash!=r.expectedAudioStateHash) return {"clip_state_mismatch"};
	const auto found=std::find_if(world.clips.begin(),world.clips.end(),[&](const auto& c){return c.state.id==r.clipId;});
	if(found==world.clips.end() || audioStateHash({found->state})!=hash) return {"plan_stale"};
	if(!s.invalidRangeMetadataDigest.isEmpty()) return {s.rangeStatus};
	AudioWindowGeometry g{s.position,s.length,s.offset,s.autoResize,s.reversed,s.solo,s.nativeName,s.color,s.playbackFrequency,s.amplification};
	QString sourceCode=s.sourceStatus=="available" && clip.sourceVerified ? QStringLiteral("ok") : s.sourceStatus;
	if(sourceCode=="available" || sourceCode=="embedded" || sourceCode=="unverified_legacy" || sourceCode=="available_unloaded") sourceCode="unsupported_audio_operation";
	AudioWindowStorage storage;
	if(s.rangeWindowV2){storage.kind=AudioWindowStorageKind::V2;storage.valueV2=s.rangeWindowV2;}
	else if(s.rangeWindow){storage.kind=AudioWindowStorageKind::V1;storage.value=s.rangeWindow;}
	const auto eligibility=storage.kind==AudioWindowStorageKind::Legacy
		?convertLegacyAudioWindow(g,world.nativeWorld,sourceCode,clip.verifiedSourceInfo)
		:evaluateAudioWindow(storage,g,world.nativeWorld,sourceCode,clip.verifiedSourceInfo);
	if(eligibility.code!="ok") return {eligibility.code};
	if(clip.verifiedSourceInfo->frames!=s.sourceFrames || clip.verifiedSourceInfo->sampleRate!=s.sampleRate
		|| clip.verifiedSourceInfo->sourceSha256!=s.sourceDigest) return {"audio_source_changed"};
	const auto initial=*eligibility.windowV2;
	const qint64 end=s.position+s.length;
	const bool sameEdge=(r.kind==Kind::TrimLeft && r.boundaryTick==s.position) || (r.kind==Kind::TrimRight && r.boundaryTick==end);
	if(!sameEdge && (r.boundaryTick<=s.position || r.boundaryTick>=end)) return {"unrepresentable_audio_boundary"};
	const auto worldHash=audioRangeWorldHash(world);if(worldHash.isEmpty()) return {"unsupported_audio_operation"};
	// Every retained interval must be free of other clips. Do not reflow neighbors.
	const qint64 begin=r.kind==Kind::TrimLeft?r.boundaryTick:s.position;
	const qint64 finish=r.kind==Kind::TrimRight?r.boundaryTick:end;
	for(const auto& c:world.clips)
	{
		const auto& other=c.state;
		if(other.position<0 || other.length<1 || other.length>INT_MAX || other.position>INT_MAX-other.length) return {"unsupported_audio_operation"};
		if(other.id!=s.id && other.trackId==s.trackId && begin<other.position+other.length && finish>other.position) return {"destination_conflict"};
	}
	AiBridgeAudioRangePlan result;result.code=sameEdge?"no_effect":"ok";result.expectedWorldHash=worldHash;result.before=clip;
	if(sameEdge) return result;
	const qint64 frame=initial.base.inFrame+(r.boundaryTick-s.position)*initial.base.framesPerTick;
	auto left=initial,right=initial;auto lg=g,rg=g;
	left.base.outFrame=frame;left.base.lengthTicks=qint32(r.boundaryTick-s.position);lg.lengthTicks=left.base.lengthTicks;
	right.base.inFrame=frame;right.base.lengthTicks=qint32(end-r.boundaryTick);right.base.offsetTicks=-qint32(frame/right.base.framesPerTick);
	rg.positionTicks=r.boundaryTick;rg.lengthTicks=right.base.lengthTicks;rg.offsetTicks=right.base.offsetTicks;
	if(r.kind==Kind::TrimLeft) result.steps.push_back({AiBridgeAudioRangeStep::Kind::PublishLeft,right,rg});
	else
	{
		result.steps.push_back({AiBridgeAudioRangeStep::Kind::PublishLeft,left,lg});
		if(r.kind==Kind::Split) result.steps.push_back({AiBridgeAudioRangeStep::Kind::CreateRight,right,rg});
	}
	for(const auto& step:result.steps) if(validateAudioWindowRecord(step.window)!="ok") return {"unrepresentable_audio_boundary"};
	return result;
}
}
