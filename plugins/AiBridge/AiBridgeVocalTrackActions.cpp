#include "AiBridgeVocalTrackActions.h"
#include "AiBridgeVocalContext.h"
#include "AiBridgeMixerRegistry.h"
#include "Engine.h"
#include "Mixer.h"
#include "SampleTrack.h"
#include "Song.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QPointer>
#include <QThread>
#include <algorithm>
#include <cmath>
namespace lmms {
namespace {
bool qt(){return QCoreApplication::instance() && QThread::currentThread()==QCoreApplication::instance()->thread();}
bool validValue(const std::optional<double>& value,double minimum,double maximum)
{
	return !value || (std::isfinite(*value) && *value>=minimum && *value<=maximum
		&& std::abs(*value*10-std::round(*value*10))<1e-7);
}
bool representable(FloatModel* model,const std::optional<double>& value)
{
	return !value || (float(*value)>=model->minValue() && float(*value)<=model->maxValue());
}
}
VocalTrackResult captureVocalTrack(const QString& id)
{
	if(!qt())return {"wrong_thread",{}};
	AiBridgeProjectContext ids;auto* track=dynamic_cast<SampleTrack*>(ids.resolveTrack(id));
	if(!track)return {"stale_id",{}};
	if(track->numOfClips()>VocalLimits::tracks)return {"resource_limit",{}};
	QStringList members;for(auto* clip:track->getClips())members.append(ids.idForClip(clip));
	std::sort(members.begin(),members.end());
	QByteArray data;QDataStream stream(&data,QIODevice::WriteOnly);stream.setVersion(QDataStream::Qt_5_15);
	stream<<QString("vocal-membership-v1")<<members;
	VocalTrackSnapshot s;s.trackId=id;s.generation=ids.projectGeneration();s.name=track->displayName();
	s.volume=track->volumeModel()->value();s.panning=track->panningModel()->value();s.muted=track->isMuted();s.solo=track->isSolo();
	s.membershipRevision=QString::fromLatin1(QCryptographicHash::hash(data,QCryptographicHash::Sha256).toHex());
	const auto index=track->mixerChannelModel()->value();
	if(index<0 || size_t(index)>=Engine::mixer()->mixerChannels().size())return {"stale_mixer_channel",{}};
	auto* channel=Engine::mixer()->mixerChannel(index);
	s.mixerId=aiBridgeMixerRegistry().channelId(channel->lifetimeToken(),channel->isMaster());
	s.hash=vocalTrackHash(s);if(s.hash.isEmpty())return {"resource_limit",{}};
	return {"ok",s};
}
VocalTrackActionResult setAudioTrackProperties(const VocalTrackRequest& request)
{
	VocalTrackActionResult result;
	auto fail=[&](const QString& code){result.code=code;return result;};
	if(!qt())return fail("wrong_thread");
	if((!request.volume && !request.panning) || !validValue(request.volume,0,200) || !validValue(request.panning,-100,100))return fail("invalid_parameter");
	const auto captured=captureVocalTrack(request.trackId);if(!captured.value)return fail(captured.code);
	if((request.expectedTrackHash.isEmpty() && !request.dryRun)
		|| (!request.expectedTrackHash.isEmpty() && request.expectedTrackHash!=captured.value->hash))return fail("track_state_mismatch");
	AiBridgeProjectContext ids;QPointer<SampleTrack> owner=dynamic_cast<SampleTrack*>(ids.resolveTrack(request.trackId));
	QPointer<Song> song=Engine::getSong();
	if(!owner)return fail("stale_id");
	if(!representable(owner->volumeModel(),request.volume) || !representable(owner->panningModel(),request.panning))return fail("invalid_parameter");
	std::vector<VocalAffectedClip> affected;affected.reserve(size_t(owner->numOfClips()));
	for(auto* clip:owner->getClips())
	{
		const auto id=ids.idForClip(clip);const auto hash=vocalClipStateHash(id);
		if(hash.isEmpty())return fail("unsupported_playback_world");
		affected.push_back({id,hash});
	}
	result.before=captured.value;result.proposedVolume=request.volume;result.proposedPanning=request.panning;
	const bool volumeChanges=request.volume && owner->volumeModel()->value()!=float(*request.volume);
	const bool panChanges=request.panning && owner->panningModel()->value()!=float(*request.panning);
	if(request.dryRun || (!volumeChanges && !panChanges))
	{
		result.after=result.before;result.affectedClips=std::move(affected);
		return fail(request.dryRun ? "ok" : "no_effect");
	}
	owner->addJournalCheckPoint();
	const auto write=[&](const char* field,const std::optional<double>& value,bool changes,bool volume){
		if(!changes)return true;
		if(!owner || ids.projectGeneration()!=result.before->generation)
		{result.failed.append(field);result.failureCode="stale_id";return false;}
		const auto fresh=captureVocalTrack(request.trackId);
		if(!fresh.value || fresh.value->membershipRevision!=result.before->membershipRevision)
		{result.failed.append(field);result.failureCode="track_state_mismatch";return false;}
		auto* model=volume ? owner->volumeModel() : owner->panningModel();
		if(!representable(model,value)){result.failed.append(field);result.failureCode="native_readback_mismatch";return false;}
		model->setValue(float(*value));
		if(!owner){result.failed.append(field);result.failureCode="stale_id";return false;}
		model=volume ? owner->volumeModel() : owner->panningModel();
		if(model->value()!=float(*value)){result.failed.append(field);result.failureCode="native_readback_mismatch";return false;}
		result.applied.append(field);return true;
	};
	bool success=write("volume",request.volume,volumeChanges,true);
	if(success)success=write("panning",request.panning,panChanges,false);
	else if(panChanges)result.unapplied.append("panning");
	if(song && ids.projectGeneration()==result.before->generation)song->setModified();
	const auto after=captureVocalTrack(request.trackId);
	if(after.value)result.after=after.value;
	if(!after.value || after.value->membershipRevision!=result.before->membershipRevision)
	{success=false;result.failureCode=after.value ? QString("track_state_mismatch") : after.code;}
	if(owner)for(auto* clip:owner->getClips())
	{
		if(result.affectedClips.size()>=size_t(VocalLimits::tracks)){success=false;result.failureCode="resource_limit";break;}
		const auto id=ids.idForClip(clip);const auto hash=vocalClipStateHash(id);
		if(hash.isEmpty()){success=false;result.failureCode="unsupported_playback_world";}
		result.affectedClips.push_back({id,hash});
	}
	return fail(success ? "ok" : "partial_failure");
}
}
