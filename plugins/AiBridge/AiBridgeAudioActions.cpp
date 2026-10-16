#include "AiBridgeAudioActions.h"
#include "AudioImportPolicy.h"
#include "Engine.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "volume.h"
#include "panning.h"
#include <QCoreApplication>
#include <QThread>
#include <QFileInfo>
#include <QPointer>
#include <cmath>
#include <limits>
namespace lmms {
namespace {
bool applicationThread() {return QCoreApplication::instance() && QThread::currentThread()==QCoreApplication::instance()->thread();}
bool nativeProperty(double value,double minimum,double maximum)
{
	return std::isfinite(value) && value>=minimum && value<=maximum && std::abs(value*10-std::round(value*10))<0.000001;
}
bool collision(SampleTrack* track,qint64 start,qint64 length)
{
	for(auto* clip:track->getClips()) if(start<qint64(clip->startPosition().getTicks())+clip->length().getTicks() && start+length>clip->startPosition().getTicks()) return true;
	return false;
}
QJsonObject trackData(AiBridgeProjectContext& ids,SampleTrack* track)
{
	return {{"track_id",ids.idForTrack(track)},{"name",track->displayName()},{"volume",track->volumeModel()->value()},
		{"panning",track->panningModel()->value()},{"muted",track->isMuted()}};
}
}
AiBridgeAudioActionResult AiBridgeAudioActions::createTrack(const AiBridgeAudioTrackRequest& request) const
{
	if(!applicationThread()) return {"wrong_thread"};
	if(request.name.trimmed().isEmpty() || request.name.size()>128 || !nativeProperty(request.volume,MinVolume,MaxVolume)
		|| !nativeProperty(request.panning,PanningLeft,PanningRight)) return {"invalid_argument"};
	if(!Engine::getSong()) return {"unsupported_audio_operation"};
	AiBridgeAudioActionResult result;result.code="ok";
	result.preview={{"action","create_audio_track"},{"name",request.name},{"volume",double(float(request.volume))},{"panning",double(float(request.panning))},{"muted",request.muted}};
	if(request.dryRun) return result;
	Engine::getSong()->addJournalCheckPoint();
	auto* track=dynamic_cast<SampleTrack*>(Track::create(Track::Type::Sample,Engine::getSong()));
	if(!track) return {"partial_failure",{}, {},result.preview,{}, {},{"create_track"},{}};
	AiBridgeProjectContext ids;result.trackId=ids.idForTrack(track);result.applied<<"create_track";
	track->setName(request.name);track->volumeModel()->setValue(float(request.volume));track->panningModel()->setValue(float(request.panning));track->setMuted(request.muted);
	Engine::getSong()->setModified();result.observed=trackData(ids,track);
	if(track->displayName()!=request.name || std::abs(track->volumeModel()->value()-request.volume)>0.00001
		|| std::abs(track->panningModel()->value()-request.panning)>0.00001 || track->isMuted()!=request.muted)
	{result.code="post_verification_failed";result.failed<<"verify_track";}
	return result;
}
AiBridgeAudioActionResult AiBridgeAudioActions::importFile(const AiBridgeAudioImportRequest& request) const
{
	if(!applicationThread()) return {"wrong_thread"};
	if(request.position<0 || request.position>std::numeric_limits<qint32>::max()) return {"invalid_argument"};
	AiBridgeProjectContext ids;auto* object=ids.resolveTrack(request.trackId);if(!object) return {"stale_id"};
	QPointer<SampleTrack> track=dynamic_cast<SampleTrack*>(object);if(!track) return {"unsupported_audio_operation"};
	if(Engine::getSong()->tempoModel().isAutomatedOrControlled()) return {"unsupported_tempo_map"};
	const auto generation=ids.projectGeneration();
	const auto volume=track->volumeModel()->value(),panning=track->panningModel()->value();
	const bool trackMuted=track->isMuted();const int fxChannel=track->mixerChannelModel()->value();
	auto prepared=AudioImportPolicy::configured().prepare(request.path);if(!prepared.audio) return {prepared.code};
	const auto info=prepared.audio->info();const auto identity=prepared.audio->identity();
	const float framesPerTick=Engine::framesPerTick(info.sampleRate);
	const float nativeTicks=float(info.frames)/framesPerTick;
	if(!std::isfinite(nativeTicks) || nativeTicks>=2147483648.0f || framesPerTick<=0) return {"source_too_large"};
	const qint64 length=std::max(1,int(nativeTicks));
	if(request.position>std::numeric_limits<qint32>::max()-length) return {"invalid_argument"};
	if(collision(track,request.position,length)) return {"destination_conflict"};
	AiBridgeAudioActionResult result;result.code="ok";result.trackId=request.trackId;
	result.preview={{"action","import_audio_file"},{"track_id",request.trackId},{"source_label",QFileInfo(prepared.audio->sourceReference()).fileName()},
		{"position_ticks",double(request.position)},{"length_ticks",double(length)},{"source_frames",double(info.frames)},
		{"sample_rate",info.sampleRate},{"source_channels",info.channels},{"ticks_per_bar",TimePos::ticksPerBar()}};
	if(request.dryRun) return result;
	if(!track || ids.projectGeneration()!=generation || ids.resolveTrack(request.trackId)!=track || !prepared.audio->isCurrent()) return {"plan_stale"};
	if(collision(track,request.position,length)) return {"destination_conflict"};
	track->addJournalCheckPoint();
	auto* created=track->createClip(TimePos(int(request.position)));
	if(!created){result.code="partial_failure";result.failed<<"create_clip";result.unapplied<<"adopt_source"<<"position_clip";return result;}
	result.clipId=ids.idForClip(created);result.applied<<"create_clip";
	auto* clip=dynamic_cast<SampleClip*>(created);
	const auto adopted=clip ? clip->adoptAuthorizedAudio(std::move(prepared.audio)) : QStringLiteral("unsupported_audio_operation");
	if(adopted!="ok") {result.code="partial_failure";result.failureCode=adopted;result.failed<<"adopt_source";result.unapplied<<"position_clip";Engine::getSong()->setModified();return result;}
	result.applied<<"adopt_source";clip->setAutoResize(false);clip->movePosition(TimePos(int(request.position)));result.applied<<"position_clip";
	Engine::getSong()->setModified();const auto after=AiBridgeAudioContext{}.clip(result.clipId);
	if(after.value) result.observed=AiBridgeAudioContext::toJson(*after.value);
	if(after.code!="ok" || after.value->state.position!=request.position || after.value->state.length!=length || after.value->state.offset!=0
		|| after.value->state.sourceIdentity!=identity || after.value->state.sourceDigest!=info.sourceSha256 || !after.value->sourceVerified
		|| after.value->state.muted || after.value->state.reversed || after.value->state.autoResize
		|| after.value->state.amplification!=1.0 || after.value->state.playbackFrequency!=DefaultBaseFreq
		|| after.value->state.trackVolume!=volume || after.value->state.trackPanning!=panning
		|| after.value->state.trackMuted!=trackMuted || after.value->state.fxChannel!=fxChannel
		|| after.value->state.sourceFrames!=info.frames || after.value->state.sampleRate!=info.sampleRate || after.value->state.sourceChannels!=std::optional<int>(info.channels))
	{result.code="post_verification_failed";result.failureCode=after.code=="ok" ? QStringLiteral("clip_state_mismatch") : after.code;result.failed<<"verify_import";}
	return result;
}
}
