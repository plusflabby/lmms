#include "AiBridgeVocalContext.h"
#include "AiBridgeAudioTypes.h"
#include "AudioEngine.h"
#include "AudioWindowPlayback.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "PatternStore.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "PathUtil.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
namespace lmms {
namespace {
bool qt(){return QCoreApplication::instance() && QThread::currentThread()==QCoreApplication::instance()->thread();}
struct CopiedSource {QString code;AiBridgeAudioClipState state;VocalSourceTicket ticket;};
CopiedSource copySource(const QString& id,bool requireAuthorized=true)
{
	AiBridgeProjectContext ids;auto* clip=dynamic_cast<SampleClip*>(ids.resolveClip(id));
	if(!clip)return {"stale_id",{}, {}};
	auto* track=dynamic_cast<SampleTrack*>(clip->getTrack());auto* song=Engine::getSong();
	if(!track || !song)return {"source_unavailable",{}, {}};
	if(requireAuthorized && (!clip->usesBoundedSampleSource() || !clip->authorizedSourceInfo()
		|| !clip->sourceLoadError().isEmpty()))return {"source_unavailable",{}, {}};
	CopiedSource result;result.code="ok";auto& t=result.ticket;auto& s=result.state;
	t.clipId=id;t.expectedIdentity=clip->sourceFileIdentity();
	if(clip->authorizedSourceInfo())t.expectedInfo=*clip->authorizedSourceInfo();
	t.expectedInfo.sourceSha256=clip->expectedSourceDigest();
	t.reference=PathUtil::toAbsolute(clip->sampleFile());
	t.sourceStatus=clip->sample().sampleSize() ? "available" : "available_unloaded";
	t.geometry={clip->startPosition().getTicks(),clip->length().getTicks(),clip->startTimeOffset().getTicks(),
		clip->getAutoResize(),clip->sample().reversed(),clip->isSolo(),clip->name(),
		clip->color()?clip->color()->name(QColor::HexArgb):QString{},clip->sample().frequency(),clip->sample().amplification()};
	t.world={song->getTempo(),int(Engine::audioEngine()->outputSampleRate()),
		TimePos::ticksPerBar()/song->getTimeSigModel().getNumerator(),song->tempoModel().isAutomatedOrControlled()};
	const auto& stored=clip->audioWindowStorage();
	if(stored.kind!=AudioWindowStorageKind::Legacy)
	{
		// Pure native eligibility plus already-qualified playback handle; no profile/file probe on Qt.
		const auto allowed=evaluateAudioWindow(stored,t.geometry,t.world,"ok",t.expectedInfo);
		if(stored.kind!=AudioWindowStorageKind::V2 || allowed.code!="ok" || !clip->audioWindowPlaybackView())
			return {"unsupported_playback_world",{}, {}};
		t.window=stored.valueV2;s.rangeWindowV2=stored.valueV2;s.rangeStatus="ok";
	}
	s.id=id;s.trackId=ids.idForTrack(track);s.nativeName=t.geometry.nativeName;s.solo=t.geometry.solo;
	s.position=t.geometry.positionTicks;s.length=t.geometry.lengthTicks;s.offset=t.geometry.offsetTicks;
	s.muted=clip->isMuted();s.reversed=t.geometry.reversed;s.autoResize=t.geometry.autoResize;s.color=t.geometry.color;
	s.trackVolume=track->volumeModel()->value();s.trackPanning=track->panningModel()->value();
	s.trackMuted=track->isMuted();s.fxChannel=track->mixerChannelModel()->value();
	s.playbackFrequency=t.geometry.frequency;s.amplification=t.geometry.amplification;
	s.sourceReferenceHash=QCryptographicHash::hash(clip->sampleFile().toUtf8(),QCryptographicHash::Sha256);
	s.sourceIdentity=t.expectedIdentity;s.sourceDigest=t.expectedInfo.sourceSha256;
	s.sourceFrames=t.expectedInfo.frames;s.sampleRate=t.expectedInfo.sampleRate;s.sourceChannels=t.expectedInfo.channels;s.sourceStatus=t.sourceStatus;
	if(!requireAuthorized && !clip->usesBoundedSampleSource())
	{
		s.sourceFrames=clip->sample().sampleSize();s.sampleRate=clip->sample().sampleRate();s.sourceChannels.reset();
		s.sourceIdentity=QCryptographicHash::hash(QByteArray("native-buffer-v1:")
			+QByteArray::number(clip->sample().buffer()->contentRevision()),QCryptographicHash::Sha256).toHex();
		s.sourceStatus=clip->sampleFile().isEmpty() ? (s.sourceFrames ? "embedded" : "empty")
			: (clip->sourceLoadError().isEmpty() ? QString("unverified_legacy") : clip->sourceLoadError());
	}
	t.sourceRevision=QString::fromLatin1(QCryptographicHash::hash(QByteArray("vocal-source-v1:")+t.expectedIdentity
		+t.expectedInfo.sourceSha256+QByteArray::number(ids.projectGeneration()),QCryptographicHash::Sha256).toHex());
	return result;
}
}
QString validateVocalProjectBounds()
{
	if(!qt())return "wrong_thread";
	auto* song=Engine::getSong();auto* patterns=Engine::patternStore();
	if(!song || !patterns)return "source_unavailable";
	if(song->isLoadingProject())return "job_invalidated";
	const auto& songTracks=song->tracks();const auto& patternTracks=patterns->tracks();
	if(songTracks.size()>VocalLimits::projectTracks || patternTracks.size()>VocalLimits::projectTracks
		|| songTracks.size()+patternTracks.size()+1>VocalLimits::projectTracks)return "resource_limit";
	size_t clips=0,bindings=0;
	const auto inspect=[&](const Track* track){
		if(!track)return true;
		const auto& items=track->getClips();
		if(items.size()>VocalLimits::projectClips-clips)return false;
		clips+=items.size();
		for(const auto* item:items)if(const auto* automation=dynamic_cast<const AutomationClip*>(item))
		{
			const auto count=automation->objects().size();
			if(count>VocalLimits::automationBindings-bindings)return false;
			bindings+=count;
		}
		return true;
	};
	for(const auto* track:songTracks)if(!inspect(track))return "resource_limit";
	for(const auto* track:patternTracks)if(!inspect(track))return "resource_limit";
	return inspect(song->globalAutomationTrack()) ? "ok" : "resource_limit";
}
VocalSnapshotResult captureVocalInputs(const VocalAnalyzeRequest& request)
{
	if(!qt())return {"wrong_thread",{}};
	const auto valid=validateVocalRequest(request);if(valid!="ok")return {valid,{}};
	const auto bounded=validateVocalProjectBounds();if(bounded!="ok")return {bounded,{}};
	VocalInputSnapshot s;AiBridgeProjectContext ids;s.generation=ids.projectGeneration();s.request=request;
	auto* config=ConfigManager::inst();s.policyRevision=config->audioImportPolicyRevision();
	const auto text=config->value("aibridge","audio_import_roots");
	if(text.isEmpty())return {"import_root_not_configured",{}};
	if(text.size()>65536)return {"resource_limit",{}};
	const auto document=QJsonDocument::fromJson(text.toUtf8());
	if(!document.isArray() || document.array().isEmpty())return {"import_root_not_configured",{}};
	if(document.array().size()>VocalLimits::rootCount)return {"resource_limit",{}};
	QStringList roots;
	for(const auto& value:document.array())
	{
		if(!value.isString() || value.toString().isEmpty() || value.toString().size()>VocalLimits::pathChars)
			return {"import_path_denied",{}};
		roots.append(value.toString());
	}
	const auto limits=SampleDecoder::configuredImportLimits();
	s.sources.reserve(request.selections.size());
	for(const auto& selection:request.selections)
	{
		auto copy=copySource(selection.clipId);if(copy.code!="ok")return {copy.code,{}};
		if(audioStateHash({copy.state})!=selection.expectedAudioHash)return {"clip_state_mismatch",{}};
		if(copy.ticket.window && (selection.frames.inFrame<copy.ticket.window->base.inFrame
			|| selection.frames.outFrame>copy.ticket.window->base.outFrame))return {"invalid_parameter",{}};
		copy.ticket.roots=roots;copy.ticket.limits=limits;copy.ticket.policyRevision=s.policyRevision;
		s.sources.push_back(std::move(copy.ticket));
	}
	const auto budget=estimateVocalBudget(s);if(budget.code!="ok")return {budget.code,{}};
	s.inputHash=vocalInputHash(s);if(s.inputHash.isEmpty())return {"invalid_parameter",{}};
	return {"ok",std::move(s)};
}
QString validateVocalSnapshot(const VocalInputSnapshot& s)
{
	if(!qt())return "wrong_thread";
	AiBridgeProjectContext ids;
	if(ids.projectGeneration()!=s.generation || ConfigManager::inst()->audioImportPolicyRevision()!=s.policyRevision)
		return "job_invalidated";
	const auto current=captureVocalInputs(s.request);
	if(!current.value)return current.code;
	return current.value->inputHash==s.inputHash ? "ok" : "clip_state_mismatch";
}
QString vocalClipStateHash(const QString& id)
{
	if(!qt())return {};
	if(validateVocalProjectBounds()!="ok")return {};
	const auto copy=copySource(id,false);return copy.code=="ok" ? audioStateHash({copy.state}) : QString{};
}
}
