#include "AiBridgeAudioContext.h"
#include "AudioImportPolicy.h"
#include "Engine.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "TimePos.h"
#include <QCoreApplication>
#include <QThread>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
namespace lmms {
namespace {
bool applicationThread() { return QCoreApplication::instance() && QThread::currentThread()==QCoreApplication::instance()->thread(); }
bool validPage(int offset,int limit) { return offset>=0 && limit>=1 && limit<=256; }
bool fits(const QJsonObject& value) { return QJsonDocument(value).toJson(QJsonDocument::Compact).size()<=64*1024; }
QByteArray invalidRangeDigest(const QDomDocument& document)
{
	QCryptographicHash hash(QCryptographicHash::Sha256);hash.addData("aibridge-invalid-audio-window-v1:");
	const auto token=[&](const QString& value){const auto bytes=value.toUtf8();hash.addData(QByteArray::number(bytes.size()));hash.addData(":");hash.addData(bytes);};
	const auto root=document.documentElement();auto node=QDomNode(root);
	// Iterative tree walk: sorted attributes, preserved child/text order, explicit
	// exit markers. No recursive stack proportional to hostile XML nesting.
	while(!node.isNull())
	{
		hash.addData("N");token(QString::number(node.nodeType()));token(node.nodeName());token(node.nodeValue());
		QStringList names;const auto attributes=node.attributes();for(int i=0;i<attributes.size();++i)names<<attributes.item(i).nodeName();
		std::sort(names.begin(),names.end());for(const auto& name:names){hash.addData("A");token(name);token(attributes.namedItem(name).nodeValue());}
		if(!node.firstChild().isNull()){node=node.firstChild();continue;}
		while(!node.isNull())
		{
			hash.addData("X");if(node==root) return hash.result();
			if(!node.nextSibling().isNull()){node=node.nextSibling();break;}
			node=node.parentNode();
		}
	}
	return hash.result();
}
AiBridgeAudioTrackSnapshot copyTrack(AiBridgeProjectContext& ids,SampleTrack* track)
{
	return {ids.idForTrack(track),track->displayName(),track->volumeModel()->value(),track->panningModel()->value(),
		track->isMuted(),track->mixerChannelModel()->value(),track->numOfClips()};
}
}
AiBridgeAudioClipResult AiBridgeAudioContext::clip(const QString& id) const
{
	if(!applicationThread()) return {"wrong_thread",{}};
	AiBridgeProjectContext ids;
	auto* object=ids.resolveClip(id); if(!object) return {"stale_id",{}};
	auto* sample=dynamic_cast<SampleClip*>(object); auto* track=dynamic_cast<SampleTrack*>(object->getTrack());
	if(!sample || !track) return {"unsupported_audio_operation",{}};
	AiBridgeAudioClipSnapshot result; auto& state=result.state;
	state.nativeName=sample->name();state.solo=sample->isSolo();
	state.id=id;state.trackId=ids.idForTrack(track);result.generation=ids.projectGeneration();result.name=sample->displayName();
	state.position=sample->startPosition().getTicks();state.length=sample->length().getTicks();state.offset=sample->startTimeOffset().getTicks();
	result.endTick=state.position+state.length;result.ticksPerBar=TimePos::ticksPerBar();
	result.ticksPerBeat=result.ticksPerBar/Engine::getSong()->getTimeSigModel().getNumerator();
	state.muted=sample->isMuted();state.reversed=sample->sample().reversed();state.autoResize=sample->getAutoResize();
	if(sample->color()) state.color=sample->color()->name(QColor::HexArgb);
	state.trackVolume=track->volumeModel()->value();state.trackPanning=track->panningModel()->value();state.fxChannel=track->mixerChannelModel()->value();
	state.trackMuted=track->isMuted();state.playbackFrequency=sample->sample().frequency();state.amplification=sample->sample().amplification();
	const auto reference=sample->sampleFile();
	state.sourceReferenceHash=QCryptographicHash::hash(reference.toUtf8(),QCryptographicHash::Sha256);
	state.sourceIdentity=sample->sourceFileIdentity();state.sourceDigest=sample->expectedSourceDigest();
	if (!sample->usesBoundedSampleSource())
	{
		// Cached legacy/embedded content has no authorized OS file identity. Pin its
		// native buffer lifetime so equal-sized replacement invalidates old state.
		state.sourceIdentity=QCryptographicHash::hash(
			QByteArray("native-buffer-v1:")+QByteArray::number(sample->sample().buffer()->contentRevision()),
			QCryptographicHash::Sha256).toHex();
	}
	state.sourceFrames=sample->sample().sampleSize();state.sampleRate=sample->sample().sampleRate();
	result.sourceLabel=QFileInfo(reference).fileName().left(128);
	if(sample->authorizedSourceInfo()) state.sourceChannels=sample->authorizedSourceInfo()->channels;
	if(reference.isEmpty()) state.sourceStatus=state.sourceFrames ? "embedded" : "empty";
	else if(!sample->usesBoundedSampleSource())
		state.sourceStatus=sample->sourceLoadError().isEmpty() ? QStringLiteral("unverified_legacy") : sample->sourceLoadError();
	else if(sample->sourceLoadError()==QStringLiteral("unsupported_audio_operation")) state.sourceStatus=sample->sourceLoadError();
	else
	{
		auto current=AudioImportPolicy::configured().prepare(reference,state.sourceDigest,state.sourceIdentity);
		if(current.audio)
		{
			result.verifiedSourceInfo=current.audio->info();
			result.sourceVerified=true;state.sourceFrames=current.audio->info().frames;state.sampleRate=current.audio->info().sampleRate;
			state.sourceChannels=current.audio->info().channels;
			state.sourceStatus=sample->sample().sampleSize() ? "available" : "available_unloaded";
		}
		else state.sourceStatus=current.code;
	}
	const auto& window=sample->audioWindowStorage();
	if(window.kind!=AudioWindowStorageKind::Legacy)
	{
		state.rangeWindow=window.value;state.rangeWindowV2=window.valueV2;
		state.rangeStatus=sample->audioWindowStatus();
		if(!sample->sourceLoadError().isEmpty()) state.rangeStatus=sample->sourceLoadError();
		else if(state.sourceStatus!="available" && state.sourceStatus!="available_unloaded"
			&& state.sourceStatus!="empty" && state.sourceStatus!="embedded") state.rangeStatus=state.sourceStatus;
		if(window.kind==AudioWindowStorageKind::InvalidMetadata)
		{
			state.rangeStatus=window.parseCode;
			state.invalidRangeMetadataDigest=invalidRangeDigest(window.retainedInvalidXml);
		}
	}
	result.stateHash=audioStateHash({state});
	if(result.stateHash.isEmpty()) return {"unsupported_audio_operation",{}};
	if(!fits(toJson(result))) return {"result_too_large",{}};
	return {"ok",std::move(result)};
}
QString AiBridgeAudioContext::validate(const QString& id,const QString& expected) const
{
	const auto current=clip(id); if(current.code!="ok") return current.code;
	return !expected.isEmpty() && current.value->stateHash==expected ? "ok" : "clip_state_mismatch";
}
AiBridgeAudioTrackPage AiBridgeAudioContext::tracks(int offset,int limit) const
{
	if(!applicationThread()) return {"wrong_thread",{}, {}};
	if(!validPage(offset,limit)) return {"invalid_argument",{}, {}};
	AiBridgeProjectContext ids; AiBridgeAudioTrackPage result;result.code="ok";
	if(auto* song=Engine::getSong()) for(auto* object:song->tracks()) if(auto* track=dynamic_cast<SampleTrack*>(object))
	{
		const auto index=result.page.total++;
		if(index>=offset && index-offset<limit) result.items.push_back(copyTrack(ids,track));
	}
	result.page.offset=qMin(offset,result.page.total);result.page.limit=limit;result.page.hasMore=result.page.offset+result.items.size()<size_t(result.page.total);
	QJsonArray rows;for(auto& row:result.items) rows.append(QJsonObject{{"track_id",row.id},{"name",row.name},{"volume",row.volume},{"panning",row.panning},{"muted",row.muted},{"fx_channel",row.fxChannel},{"clip_count",row.clipCount}});
	if(!fits({{"tracks",rows}})) {result.code="result_too_large";result.items.clear();}
	return result;
}
AiBridgeAudioClipPage AiBridgeAudioContext::clips(const QString& trackId,int offset,int limit) const
{
	if(!applicationThread()) return {"wrong_thread",{}, {}};
	if(!validPage(offset,limit)) return {"invalid_argument",{}, {}};
	AiBridgeProjectContext ids;auto* object=ids.resolveTrack(trackId);if(!object) return {"stale_id",{}, {}};
	auto* track=dynamic_cast<SampleTrack*>(object);if(!track) return {"unsupported_audio_operation",{}, {}};
	AiBridgeAudioClipPage result;result.code="ok";result.page.total=track->numOfClips();result.page.offset=qMin(offset,result.page.total);result.page.limit=limit;
	const auto end=qMin(result.page.total,result.page.offset+limit);result.page.hasMore=end<result.page.total;
	for(int i=result.page.offset;i<end;++i)
	{
		auto copied=clip(ids.idForClip(track->getClip(i)));if(copied.code!="ok") {result.code=copied.code;result.items.clear();return result;}
		result.items.push_back(std::move(*copied.value));
	}
	QJsonArray rows;for(auto& item:result.items) rows.append(toJson(item));
	if(!fits({{"clips",rows}})) {result.code="result_too_large";result.items.clear();}
	return result;
}
QJsonObject AiBridgeAudioContext::toJson(const AiBridgeAudioClipSnapshot& s)
{
	const auto& c=s.state;
	return {{"clip_id",c.id},{"track_id",c.trackId},{"name",s.name},{"audio_state_hash",s.stateHash},
		{"project_generation",QString::number(s.generation)},{"position_ticks",double(c.position)},{"length_ticks",double(c.length)},
		{"end_tick",double(s.endTick)},{"source_offset_ticks",double(c.offset)},{"ticks_per_bar",double(s.ticksPerBar)},{"ticks_per_beat",double(s.ticksPerBeat)},
		{"source_label",s.sourceLabel},{"source_status",c.sourceStatus},{"source_verified",s.sourceVerified},
		{"source_frames",double(c.sourceFrames)},{"sample_rate",c.sampleRate},{"source_channels",c.sourceChannels ? QJsonValue(*c.sourceChannels):QJsonValue(QJsonValue::Null)},
		{"source_duration_seconds",c.sampleRate>0 && c.sourceFrames>0 ? QJsonValue(double(c.sourceFrames)/c.sampleRate):QJsonValue(QJsonValue::Null)},
		{"muted",c.muted},{"solo",c.solo},{"reversed",c.reversed},{"auto_resize",c.autoResize},{"color",c.color},
		{"track_volume",c.trackVolume},{"track_panning",c.trackPanning},{"fx_channel",c.fxChannel},
		{"track_muted",c.trackMuted},{"playback_frequency",c.playbackFrequency},{"amplification",c.amplification}};
}
}
