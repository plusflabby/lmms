#include "AiBridgeVocalVariantAttachment.h"

#include "AiBridgeAudioContext.h"
#include "AiBridgeProjectContext.h"
#include "AiBridgeVocalAssetActions.h"
#include "AiBridgeVocalContext.h"
#include "AiBridgeVocalTrackActions.h"
#include "Engine.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "src/core/vocal/ManagedVocalAssetRoot.h"
#include "src/core/vocal/ManagedVocalPlayback.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QPointer>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <limits>

namespace lmms {
namespace {

bool applicationThread()
{
	return QCoreApplication::instance() && QThread::currentThread()==QCoreApplication::instance()->thread();
}

bool validUuid(const QString& value)
{
	const auto parsed=QUuid(value);
	return !parsed.isNull() && parsed.toString(QUuid::WithoutBraces)==value;
}

QString activeVariantId(const SampleClip& clip)
{
	return clip.managedVocalVariantAssetId();
}

QJsonObject clipStateJson(const AiBridgeAudioClipSnapshot& clip,const QString& variantId)
{
	return {{QStringLiteral("clip_id"),clip.state.id},{QStringLiteral("track_id"),clip.state.trackId},
		{QStringLiteral("audio_state_hash"),clip.stateHash},
		{QStringLiteral("position_ticks"),QString::number(clip.state.position)},
		{QStringLiteral("length_ticks"),QString::number(clip.state.length)},
		{QStringLiteral("managed_variant_id"),variantId.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(variantId)}};
}

bool collision(SampleTrack* track,qint64 start,qint64 length,const Clip* excluded)
{
	if(!track || start<0 || length<=0 || start>std::numeric_limits<qint32>::max()-length)return true;
	for(const auto* clip:track->getClips())
	{
		if(clip==excluded)continue;
		const auto clipStart=qint64(clip->startPosition().getTicks());
		const auto clipLength=qint64(clip->length().getTicks());
		if(start<clipStart+clipLength && start+length>clipStart)return true;
	}
	return false;
}

bool matchingSource(const ManagedVocalAssetInspectResult& asset,const AiBridgeAudioClipSnapshot& clip,
	const SampleClip& nativeClip)
{
	const auto digest=QString::fromLatin1(clip.state.sourceDigest.toHex());
	const auto recorded=nativeClip.recordedTakeReference();
	const auto identityBytes=recorded
		? QByteArrayLiteral("recorded-take-identity-v1:")
			+ QByteArray::number(recorded->sourceIdentity.volume) + QByteArrayLiteral(":")
			+ QByteArray::number(recorded->sourceIdentity.index)
		: clip.state.sourceIdentity;
	const auto identity=QString::fromLatin1(
		QCryptographicHash::hash(identityBytes,QCryptographicHash::Sha256).toHex());
	// Clip IDs are session-scoped provenance; rebind across reloads only by the
	// verified file identity and content digest (plus sealed-take identity).
	for(const auto& source:asset.sources)
	{
		if(digest.isEmpty() || identity.isEmpty() || source.sourceSha256!=digest
			|| source.sourceIdentity!=identity)continue;
		if(recorded)
		{
			if(source.recordedTakeProjectId==recorded->projectId
				&& source.recordedTakeId==recorded->takeId)return true;
		}
		else if(source.recordedTakeProjectId.isEmpty() && source.recordedTakeId.isEmpty())return true;
	}
	return false;
}

ManagedVocalVariantAssociation associationFor(const ManagedVocalVariantReference& reference,
	const VocalEditWorld& world,const ManagedVocalVisualExtent& extent)
{
	ManagedVocalVariantAssociation association;
	association.active=reference;
	association.playback.tempoBpm=world.tempoBpm;
	association.playback.outputRate=world.outputRate;
	association.playback.ticksPerQuarterNote=world.ticksPerQuarterNote;
	association.playback.timeSignatureNumerator=world.timeSignatureNumerator;
	association.playback.timeSignatureDenominator=world.timeSignatureDenominator;
	association.playback.visualLengthTicks=extent.visualLengthTicks;
	association.playback.nativeVisualCoverageFrames=extent.nativeCoverageFrames;
	association.playback.endpointResidualNumerator=extent.exactResidualNumerator;
	association.playback.endpointResidualDenominator=extent.exactResidualDenominator;
	association.playback.worldHash=vocalEditWorldHash(world);
	return association;
}

} // namespace

VocalVariantApplyResult AiBridgeVocalVariantAttachment::apply(const VocalVariantApplyRequest& request)
{
	auto fail=[](const QString& code){return VocalVariantApplyResult{code,{}};};
	if(!applicationThread())return fail(QStringLiteral("wrong_thread"));
	QPointer<Song> song=Engine::getSong();
	if(!song)return fail(QStringLiteral("stale_id"));
	const auto identity=song->recordingProjectIdentity().cachedIdentity();
	const auto identityRevision=song->recordingProjectIdentity().revision();
	if(!validUuid(identity.id))return fail(QStringLiteral("project_identity_unverified"));
	AiBridgeProjectContext ids;
	const auto generation=ids.projectGeneration();
	if(generation!=request.expectedProjectGeneration)return fail(QStringLiteral("plan_stale"));
	const auto world=captureVocalEditWorld();
	const auto worldHash=vocalEditWorldHash(world);
	if(worldHash.isEmpty() || worldHash!=request.expectedWorldHash)return fail(QStringLiteral("plan_stale"));
	if(world.tempoAutomationPresent || world.tempoBpm<=0 || world.ticksPerQuarterNote!=48
		|| world.timeSignatureNumerator<1 || world.timeSignatureNumerator>32
		|| world.timeSignatureDenominator<1 || world.timeSignatureDenominator>32)
		return fail(QStringLiteral("unsupported_playback_world"));
	if(request.mode!=QStringLiteral("compare") && request.mode!=QStringLiteral("replace")
		&& request.mode!=QStringLiteral("revert"))return fail(QStringLiteral("invalid_parameter"));
	auto* native=dynamic_cast<SampleClip*>(ids.resolveClip(request.clipId));
	if(!native)return fail(QStringLiteral("stale_id"));
	QPointer<SampleClip> nativeGuard(native);
	const bool recordedSourceBefore=native->isRecordedTake();
	QString recordedProjectIdBefore,recordedTakeIdBefore;
	if(const auto& reference=native->recordedTakeReference();reference)
	{
		recordedProjectIdBefore=reference->projectId;
		recordedTakeIdBefore=reference->takeId;
	}
	const auto source=AiBridgeAudioContext{}.clip(request.clipId);
	if(!source.value)return fail(source.code);
	if(source.value->stateHash!=request.expectedAudioStateHash)return fail(QStringLiteral("clip_state_mismatch"));
	const auto sourceTrack=captureVocalTrack(source.value->state.trackId);
	if(!sourceTrack.value)return fail(sourceTrack.code);
	if(sourceTrack.value->hash!=request.expectedTrackStateHash)return fail(QStringLiteral("track_state_mismatch"));
	if(native->hasAudioSourceWindow() || native->startTimeOffset().getTicks()!=0 || native->sample().reversed())
		return fail(QStringLiteral("unsupported_clip_state"));
	if(native->isRecordedTake())
	{
		if(!native->recordedTakeSourceCurrent())return fail(QStringLiteral("source_changed"));
	}
	else if(!source.value->sourceVerified)return fail(QStringLiteral("source_unavailable"));

	QPointer<SampleTrack> destinationTrack;
	std::optional<VocalTrackSnapshot> destinationBefore;
	if(request.mode==QStringLiteral("compare"))
	{
		auto* resolved=dynamic_cast<SampleTrack*>(ids.resolveTrack(request.destinationTrackId));
		if(!resolved)return fail(QStringLiteral("stale_id"));
		if(resolved==native->getTrack())return fail(QStringLiteral("destination_conflict"));
		const auto captured=captureVocalTrack(request.destinationTrackId);
		if(!captured.value)return fail(captured.code);
		if(captured.value->hash!=request.expectedDestinationTrackStateHash)
			return fail(QStringLiteral("track_state_mismatch"));
		destinationTrack=resolved;destinationBefore=*captured.value;
	}

	std::optional<ManagedVocalAssetInspectResult> variant;
	ManagedVocalVariantAssociation requestedAssociation;
	ManagedVocalVisualExtent variantExtent;
	QString beforeVariantId=activeVariantId(*native);
	qint64 finalLength=source.value->state.length;
	if(request.mode==QStringLiteral("revert"))
	{
		if(!native->hasManagedVocalVariant() || beforeVariantId!=request.variantId)
			return fail(QStringLiteral("clip_state_mismatch"));
		const auto current=native->managedVocalVariantAssociation();
		if(!current)return fail(QStringLiteral("invalid_managed_variant"));
		finalLength=current->originalLengthTicks;
		if(collision(dynamic_cast<SampleTrack*>(native->getTrack()),source.value->state.position,finalLength,native))
			return fail(QStringLiteral("destination_conflict"));
	}
	else
	{
		const auto inspected=AiBridgeVocalAssetActions::inspectVariant(request.variantId);
		if(inspected.code!=QStringLiteral("ok"))return fail(inspected.code);
		if(inspected.projectId!=identity.id)return fail(QStringLiteral("asset_project_mismatch"));
		if(!matchingSource(inspected,*source.value,*native))return fail(QStringLiteral("source_changed"));
		if(inspected.sampleRate!=world.outputRate || (inspected.sampleRate!=44100 && inspected.sampleRate!=48000))
			return fail(QStringLiteral("unsupported_playback_world"));
		variantExtent=managedVocalVisualExtent(inspected.frames,inspected.sampleRate,world.tempoBpm);
		if(variantExtent.code!=QStringLiteral("ok"))return fail(variantExtent.code);
		finalLength=variantExtent.visualLengthTicks;
		if(request.mode==QStringLiteral("replace") && beforeVariantId==request.variantId)
			return fail(QStringLiteral("no_effect"));
		if(request.mode==QStringLiteral("replace")
			&& collision(dynamic_cast<SampleTrack*>(native->getTrack()),source.value->state.position,finalLength,native))
			return fail(QStringLiteral("destination_conflict"));
		if(request.mode==QStringLiteral("compare")
			&& collision(destinationTrack,source.value->state.position,finalLength,nullptr))
			return fail(QStringLiteral("destination_conflict"));
		requestedAssociation=associationFor(inspected.reference,world,variantExtent);
		variant=inspected;
	}

	QJsonObject data{{QStringLiteral("mode"),request.mode},{QStringLiteral("variant_id"),request.variantId},
		{QStringLiteral("source_clip_id"),request.clipId},{QStringLiteral("dry_run"),request.dryRun},
		{QStringLiteral("applied"),false},{QStringLiteral("source_preserved"),true},
		{QStringLiteral("project_generation"),QString::number(generation)},{QStringLiteral("world_hash"),worldHash},
		{QStringLiteral("before"),clipStateJson(*source.value,beforeVariantId)}};
	const auto projectedTrackId=request.mode==QStringLiteral("compare")?request.destinationTrackId:source.value->state.trackId;
	QJsonObject projected{{QStringLiteral("clip_id"),request.mode==QStringLiteral("compare")?QJsonValue(QJsonValue::Null):QJsonValue(request.clipId)},
		{QStringLiteral("track_id"),projectedTrackId},{QStringLiteral("position_ticks"),QString::number(source.value->state.position)},
		{QStringLiteral("length_ticks"),QString::number(finalLength)},
		{QStringLiteral("managed_variant_id"),request.mode==QStringLiteral("revert")?QJsonValue(QJsonValue::Null):QJsonValue(request.variantId)}};
	data.insert(QStringLiteral("after"),projected);
	if(request.mode==QStringLiteral("compare"))
	{
		data.insert(QStringLiteral("before_track_state_hash"),sourceTrack.value->hash);
		data.insert(QStringLiteral("destination_track_state_hash"),destinationBefore->hash);
	}
	else data.insert(QStringLiteral("before_track_state_hash"),sourceTrack.value->hash);
	if(request.dryRun)return {QStringLiteral("ok"),data};

	if(request.mode==QStringLiteral("revert"))
	{
		const auto currentAudio=AiBridgeAudioContext{}.clip(request.clipId);
		const auto currentTrack=captureVocalTrack(source.value->state.trackId);
		if(!currentAudio.value || currentAudio.value->stateHash!=request.expectedAudioStateHash
			|| !currentTrack.value || currentTrack.value->hash!=request.expectedTrackStateHash
			|| !song || !nativeGuard || ids.projectGeneration()!=generation
			|| song->recordingProjectIdentity().cachedIdentity().id!=identity.id
			|| song->recordingProjectIdentity().revision()!=identityRevision
			|| vocalEditWorldHash(captureVocalEditWorld())!=worldHash)
			return fail(QStringLiteral("plan_stale"));
		const auto reverted=nativeGuard->revertManagedVocalVariant();
		if(reverted!=QStringLiteral("ok"))return fail(reverted);
		if(!nativeGuard)return fail(QStringLiteral("post_verification_failed"));
	}
	else
	{
		const auto loadCode=native->preloadManagedVocalVariant(variant->reference);
		if(loadCode!=QStringLiteral("ok"))return fail(loadCode);

		const auto currentAudio=AiBridgeAudioContext{}.clip(request.clipId);
		const auto currentTrack=captureVocalTrack(source.value->state.trackId);
		if(!currentAudio.value || currentAudio.value->stateHash!=request.expectedAudioStateHash
			|| !currentTrack.value || currentTrack.value->hash!=request.expectedTrackStateHash
			|| !song || !nativeGuard || ids.projectGeneration()!=generation
			|| song->recordingProjectIdentity().cachedIdentity().id!=identity.id
			|| song->recordingProjectIdentity().revision()!=identityRevision
			|| vocalEditWorldHash(captureVocalEditWorld())!=worldHash)
			return fail(QStringLiteral("plan_stale"));

		QPointer<SampleClip> destinationClip=native;
		if(request.mode==QStringLiteral("compare"))
		{
			const auto currentDestination=captureVocalTrack(request.destinationTrackId);
			if(!currentDestination.value || currentDestination.value->hash!=request.expectedDestinationTrackStateHash
				|| ids.resolveTrack(request.destinationTrackId)!=destinationTrack)
				return fail(QStringLiteral("plan_stale"));
			if(collision(destinationTrack,source.value->state.position,finalLength,nullptr))
				return fail(QStringLiteral("destination_conflict"));
			destinationTrack->addJournalCheckPoint();
			destinationClip=native->cloneToTrack(destinationTrack);
			if(!destinationClip)return fail(QStringLiteral("native_copy_failed"));
			if(!nativeGuard){delete destinationClip;return fail(QStringLiteral("stale_id"));}
		}
		const auto applied=destinationClip->applyManagedVocalVariantAssociation(requestedAssociation);
		if(applied!=QStringLiteral("ok"))
		{
			if(request.mode==QStringLiteral("compare") && destinationClip)delete destinationClip;
			return fail(applied);
		}
		if(!destinationClip)return fail(QStringLiteral("post_verification_failed"));
		if(!song)return fail(QStringLiteral("post_verification_failed"));
		song->setModified();
		if(request.mode==QStringLiteral("compare"))
			data.insert(QStringLiteral("result_clip_id"),ids.idForClip(destinationClip));
	}

	const auto resultClipId=request.mode==QStringLiteral("compare")
		?data.value(QStringLiteral("result_clip_id")).toString():request.clipId;
	const auto after=AiBridgeAudioContext{}.clip(resultClipId);
	const auto afterTrack=captureVocalTrack(projectedTrackId);
	if(!after.value || !afterTrack.value)
	{
		data.insert(QStringLiteral("applied"),true);
		data.insert(QStringLiteral("source_preserved"),false);
		return {QStringLiteral("post_verification_failed"),data};
	}
	const auto* resultNative=dynamic_cast<SampleClip*>(ids.resolveClip(resultClipId));
	const auto afterVariantId=resultNative?activeVariantId(*resultNative):QString{};
	bool originalPreserved=false;
	if(request.mode==QStringLiteral("compare"))
	{
		const auto originalAfter=AiBridgeAudioContext{}.clip(request.clipId);
		originalPreserved=originalAfter.value && originalAfter.value->stateHash==source.value->stateHash;
	}
	else
	{
		originalPreserved=after.value->state.sourceReferenceHash==source.value->state.sourceReferenceHash
			&& after.value->state.sourceIdentity==source.value->state.sourceIdentity
			&& after.value->state.sourceDigest==source.value->state.sourceDigest
			&& after.value->state.sourceFrames==source.value->state.sourceFrames
			&& after.value->state.sampleRate==source.value->state.sampleRate;
		const auto* preservedNative=dynamic_cast<SampleClip*>(ids.resolveClip(request.clipId));
		if(!preservedNative || preservedNative->isRecordedTake()!=recordedSourceBefore)originalPreserved=false;
		else if(const auto& reference=preservedNative->recordedTakeReference();reference)
			originalPreserved=originalPreserved && reference->projectId==recordedProjectIdBefore
				&& reference->takeId==recordedTakeIdBefore;
		else originalPreserved=originalPreserved && recordedProjectIdBefore.isEmpty() && recordedTakeIdBefore.isEmpty();
	}
	data.insert(QStringLiteral("source_preserved"),originalPreserved);
	if(after.value->state.position!=source.value->state.position
		|| after.value->state.length!=finalLength || after.value->state.trackId!=projectedTrackId)
	{
		data.insert(QStringLiteral("applied"),true);
		data.insert(QStringLiteral("after"),clipStateJson(*after.value,afterVariantId));
		data.insert(QStringLiteral("after_track_state_hash"),afterTrack.value->hash);
		return {QStringLiteral("post_verification_failed"),data};
	}
	if(request.mode==QStringLiteral("revert") ? !afterVariantId.isEmpty() : afterVariantId!=request.variantId)
	{
		data.insert(QStringLiteral("applied"),true);
		data.insert(QStringLiteral("after"),clipStateJson(*after.value,afterVariantId));
		data.insert(QStringLiteral("after_track_state_hash"),afterTrack.value->hash);
		return {QStringLiteral("post_verification_failed"),data};
	}
	if(!originalPreserved)
	{
		data.insert(QStringLiteral("applied"),true);
		data.insert(QStringLiteral("after"),clipStateJson(*after.value,afterVariantId));
		data.insert(QStringLiteral("after_track_state_hash"),afterTrack.value->hash);
		return {QStringLiteral("post_verification_failed"),data};
	}
	data.insert(QStringLiteral("applied"),true);
	data.insert(QStringLiteral("after"),clipStateJson(*after.value,afterVariantId));
	data.insert(QStringLiteral("after_track_state_hash"),afterTrack.value->hash);
	return {QStringLiteral("ok"),data};
}

} // namespace lmms
