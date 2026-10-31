#include "RecordingAttachment.h"
#include "AudioImportPolicy.h"
#include "AudioEngine.h"
#include "Engine.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "Track.h"
#include <QPointer>
#include <QSignalBlocker>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <limits>
#include <map>
#include <vector>

namespace lmms::recording {
namespace {
QString entryKey(const QString& projectId, const QString& takeId)
{
	return projectId + QLatin1Char('/') + takeId;
}

QString newId()
{
	return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool sameFacts(const RawTakeFacts& left, const RawTakeFacts& right)
{
	return left.projectId == right.projectId && left.takeId == right.takeId && left.sampleRate == right.sampleRate
		&& left.rawFrames == right.rawFrames && left.sourceIdentity == right.sourceIdentity && left.sourceDigest == right.sourceDigest;
}

bool samePlan(const PlacementPlan& left, const PlacementPlan& right)
{
	return left.positionTicks == right.positionTicks && left.offsetTicks == right.offsetTicks
		&& left.lengthTicks == right.lengthTicks && left.effectiveSourceInFrame == right.effectiveSourceInFrame
		&& left.nativeEndFrame == right.nativeEndFrame && left.rawEndFrame == right.rawEndFrame
		&& left.placementRevision == right.placementRevision && left.placementQuality == right.placementQuality
		&& left.driftQuality == right.driftQuality && left.sampleAccuracyVerified == right.sampleAccuracyVerified;
}

struct FoundClip
{
	SampleTrack* track = nullptr;
	SampleClip* clip = nullptr;
};

std::vector<FoundClip> findClips(Song* song, const QString& projectId, const QString& takeId)
{
	std::vector<FoundClip> matches;
	if (!song) return matches;
	for (auto* base : song->tracks())
	{
		auto* track = dynamic_cast<SampleTrack*>(base);
		if (!track) continue;
		for (int i = 0; i < track->numOfClips(); ++i)
		{
			auto* clip = dynamic_cast<SampleClip*>(track->getClip(i));
			if (!clip || !clip->recordedTakeReference()) continue;
			const auto& reference = *clip->recordedTakeReference();
			if (reference.projectId == projectId && reference.takeId == takeId) matches.push_back({track, clip});
		}
	}
	return matches;
}

AttachmentResult failure(const QString& code, const QString& cause = {}, const QString& trackId = {}, const QString& clipId = {})
{
	return {code, trackId, clipId, cause, false};
}

AttachmentResult savedUnattached(const QString& cause, const QString& trackId = {}, const QString& clipId = {})
{
	return {QStringLiteral("saved_unattached"), trackId, clipId, cause, false};
}
}

struct RecordingAttachment::Impl
{
	enum class State { Attached, Failed };
	struct Entry
	{
		State state = State::Failed;
		QPointer<SampleTrack> track;
		QPointer<SampleClip> clip;
		QString projectId, takeId, trackId, clipId;
		RawTakeFacts facts;
		PlacementPlan plan;
		RecordingTransportSnapshot geometry;
		std::shared_ptr<SealedTakeToken> token;
		AttachmentResult lastResult;
	};

	RecordingServiceHost& host;
	RecordingTransportControl& transport;
	QThread* owner = QThread::currentThread();
	std::map<QString, Entry> entries;
	bool busy = false;

	Impl(RecordingServiceHost& recordingHost, RecordingTransportControl& control)
		: host(recordingHost), transport(control)
	{
	}

	void assertOwner() const
	{
		Q_ASSERT(QThread::currentThread() == owner);
	}
};

RecordingAttachment::RecordingAttachment(RecordingServiceHost& host, RecordingTransportControl& transport)
	: m_impl(std::make_unique<Impl>(host, transport))
{
}

RecordingAttachment::~RecordingAttachment() = default;

AttachmentResult RecordingAttachment::attach(const SealedTakeToken& source, const PlacementPlan& plan, quint64 expectedGeneration)
{
	auto& state = *m_impl;
	state.assertOwner();
	if (state.busy) return failure(QStringLiteral("recording_busy"));
	struct BusyReset
	{
		bool& busy;
		~BusyReset() { busy = false; }
	} reset{state.busy};
	state.busy = true;

	auto* song = Engine::getSong();
	auto* audio = Engine::audioEngine();
	if (!song || !audio) return failure(QStringLiteral("engine_unavailable"));
	const auto facts = source.facts();
	const auto current = state.transport.snapshot();
	const auto projectIdentity = song->recordingProjectIdentity().cachedIdentity();
	if (!expectedGeneration || current.projectGeneration != expectedGeneration || !current.available
		|| current.projectId != facts.projectId || projectIdentity.id != facts.projectId
		|| state.host.projectIdentity().id != facts.projectId)
		return failure(QStringLiteral("stale_project"));
	if (current.playing || current.paused || current.exporting) return failure(QStringLiteral("transport_busy"));
	if (current.looping) return failure(QStringLiteral("unsupported_loop"));
	if (current.metronomeActive) return failure(QStringLiteral("unsupported_metronome"));
	if (!current.constantTempo || !current.stableMeter) return failure(QStringLiteral("unsupported_transport_automation"));
	if (!state.host.quiescent()) return failure(QStringLiteral("recording_busy"));
	if ((facts.sampleRate != 44100 && facts.sampleRate != 48000) || current.sampleRate != facts.sampleRate
		|| audio->outputSampleRate() != facts.sampleRate) return failure(QStringLiteral("unsupported_rate"));
	if (plan.positionTicks < 0 || plan.offsetTicks > 0 || plan.offsetTicks == std::numeric_limits<qint64>::min()
		|| plan.lengthTicks <= 0 || !plan.placementRevision || plan.placementQuality != QStringLiteral("uncalibrated")
		|| plan.driftQuality != QStringLiteral("unmeasured") || plan.sampleAccuracyVerified)
		return failure(QStringLiteral("invalid_placement"));
	if (!source.isCurrent()) return savedUnattached(QStringLiteral("recorded_source_unavailable"));
	if (!facts.rawFrames || facts.rawFrames > std::uint64_t(facts.sampleRate) * 120 || facts.sourceDigest.size() != 32
		|| !facts.sourceIdentity.index) return savedUnattached(QStringLiteral("invalid_recorded_source"));

	NativeGeometrySnapshot geometry;
	geometry.current = current;
	geometry.reviewed = current;
	geometry.reviewedSource = facts;
	geometry.placementRevision = plan.placementRevision;
	geometry.sourceFramesPerTick = Engine::framesPerTick(facts.sampleRate);
	const auto checked = RecordingPlacement::revalidatePlan(facts, plan, geometry);
	if (!checked.plan)
		return failure(checked.code == QStringLiteral("placement_review_stale") ? checked.code : QStringLiteral("invalid_placement"), checked.code);
	if (!samePlan(*checked.plan, plan)) return failure(QStringLiteral("placement_review_stale"));

	const auto key = entryKey(facts.projectId, facts.takeId);
	auto existing = state.entries.find(key);
	if (existing == state.entries.end())
	{
		const auto matches = findClips(song, facts.projectId, facts.takeId);
		if (matches.size() > 1) return savedUnattached(QStringLiteral("duplicate_attachment_conflict"));
		if (matches.size() == 1)
		{
			auto* foundTrack = matches.front().track;
			auto* foundClip = matches.front().clip;
			const auto* reference = foundClip->recordedTakeReference() ? &*foundClip->recordedTakeReference() : nullptr;
			const auto info = foundClip->authorizedSourceInfo();
			if (!reference || !RecordedTakeSource::matches(*reference, facts) || !info || info->frames != facts.rawFrames
				|| foundClip->sourceLoadError().size() || foundClip->hasAudioSourceWindow() || foundClip->getAutoResize()
				|| foundTrack->trackContainer() != song || foundTrack->numOfClips() != 1)
				return failure(QStringLiteral("stale_owned_clip"), QStringLiteral("saved_unattached"),
					reference ? reference->trackId : QString{}, reference ? reference->clipId : QString{});
			NativeGeometrySnapshot existingGeometry;
			existingGeometry.current = current;
			existingGeometry.reviewed = current;
			existingGeometry.reviewedSource = facts;
			existingGeometry.placementRevision = reference->placementRevision;
			existingGeometry.sourceFramesPerTick = Engine::framesPerTick(facts.sampleRate);
		const auto existingPlan = RecordingPlacement::restorePersistedPlan(facts, foundClip->startPosition().getTicks(),
			foundClip->startTimeOffset().getTicks(), foundClip->length().getTicks(), reference->placementRevision, existingGeometry);
			if (!existingPlan.plan || existingPlan.plan->lengthTicks != foundClip->length().getTicks()
				|| existingPlan.plan->offsetTicks != foundClip->startTimeOffset().getTicks())
				return failure(QStringLiteral("stale_owned_clip"), QStringLiteral("saved_unattached"), reference->trackId, reference->clipId);
			auto opened = state.host.openSealedTake({facts.projectId, 1}, facts.takeId);
			if (!opened.source || !sameFacts(opened.source->facts(), facts)) return savedUnattached(QStringLiteral("recorded_source_changed"));
			Impl::Entry entry;
			entry.state = Impl::State::Attached;
			entry.track = foundTrack;
			entry.clip = foundClip;
			entry.projectId = facts.projectId;
			entry.takeId = facts.takeId;
			entry.trackId = reference->trackId;
			entry.clipId = reference->clipId;
			entry.facts = facts;
			entry.plan = *existingPlan.plan;
			entry.geometry = current;
			entry.token = std::make_shared<SealedTakeToken>(std::move(*opened.source));
			state.entries.emplace(key, std::move(entry));
			existing = state.entries.find(key);
		}
	}
	if (existing != state.entries.end())
	{
		auto& entry = existing->second;
		if (entry.state == Impl::State::Failed) return entry.lastResult;
		if (!sameFacts(entry.facts, facts) || !entry.token || !entry.token->isCurrent())
			return savedUnattached(QStringLiteral("recorded_source_changed"), entry.trackId, entry.clipId);
		if (!entry.track || !entry.clip)
		{
			const auto matches = findClips(song, facts.projectId, facts.takeId);
			if (matches.size() != 1) return failure(QStringLiteral("stale_owned_clip"), QStringLiteral("saved_unattached"), entry.trackId, entry.clipId);
			const auto* reference = matches.front().clip->recordedTakeReference() ? &*matches.front().clip->recordedTakeReference() : nullptr;
			if (!reference || reference->trackId != entry.trackId || reference->clipId != entry.clipId
				|| !RecordedTakeSource::matches(*reference, facts))
				return failure(QStringLiteral("stale_owned_clip"), QStringLiteral("saved_unattached"), entry.trackId, entry.clipId);
			entry.track = matches.front().track;
			entry.clip = matches.front().clip;
		}
		auto* track = entry.track.data();
		auto* clip = entry.clip.data();
		const auto* reference = clip->recordedTakeReference() ? &*clip->recordedTakeReference() : nullptr;
		const auto info = clip->authorizedSourceInfo();
		if (!reference || !RecordedTakeSource::matches(*reference, facts) || reference->trackId != entry.trackId
			|| reference->clipId != entry.clipId || reference->placementRevision != entry.plan.placementRevision
			|| !info || info->frames != facts.rawFrames || !clip->sourceLoadError().isEmpty()
			|| clip->hasAudioSourceWindow() || clip->getAutoResize() || track->trackContainer() != song
			|| track->numOfClips() != 1 || clip->startPosition().getTicks() != entry.plan.positionTicks
			|| clip->startTimeOffset().getTicks() != entry.plan.offsetTicks || clip->length().getTicks() != entry.plan.lengthTicks)
			return failure(QStringLiteral("stale_owned_clip"), QStringLiteral("saved_unattached"), entry.trackId, entry.clipId);
		if (plan.placementRevision == entry.plan.placementRevision)
		{
			if (!samePlan(entry.plan, plan)) return failure(QStringLiteral("conflicting_revision"));
			return {QStringLiteral("already_attached"), entry.trackId, entry.clipId, {}, true};
		}
		if (plan.placementRevision < entry.plan.placementRevision)
			return failure(QStringLiteral("stale_placement"));
		clip->addJournalCheckPoint();
		QPointer<SampleTrack> trackGuard(track);
		QPointer<SampleClip> clipGuard(clip);
		{
			QSignalBlocker songSignals(song);
			QSignalBlocker trackSignals(track);
			QSignalBlocker clipSignals(clip);
			clip->setAutoResize(false);
			clip->movePosition(TimePos(int(plan.positionTicks)));
			clip->changeLength(TimePos(int(plan.lengthTicks)));
			clip->setStartTimeOffset(TimePos(int(plan.offsetTicks)));
			clip->m_recordedTakeReference->placementRevision = plan.placementRevision;
			clip->updateTrackClips();
			song->setModified();
		}
		if (!trackGuard || !clipGuard) return savedUnattached(QStringLiteral("native_owner_invalidated"), entry.trackId, entry.clipId);
		song->updateLength();
		emit clip->positionChanged();
		if (!clipGuard) return savedUnattached(QStringLiteral("native_owner_invalidated"), entry.trackId, entry.clipId);
		emit clip->lengthChanged();
		if (!clipGuard) return savedUnattached(QStringLiteral("native_owner_invalidated"), entry.trackId, entry.clipId);
		emit clip->dataChanged();
		if (!clipGuard) return savedUnattached(QStringLiteral("native_owner_invalidated"), entry.trackId, entry.clipId);
		emit clip->sampleChanged();
		if (!clipGuard) return savedUnattached(QStringLiteral("native_owner_invalidated"), entry.trackId, entry.clipId);
		entry.plan = plan;
		entry.geometry = current;
		return {QStringLiteral("updated"), entry.trackId, entry.clipId, {}, true};
	}

	if (state.entries.size() >= 64) return failure(QStringLiteral("resource_limit"));
	auto opened = state.host.openSealedTake({facts.projectId, 1}, facts.takeId);
	if (!opened.source) return savedUnattached(opened.code);
	if (!sameFacts(opened.source->facts(), facts)) return savedUnattached(QStringLiteral("recorded_source_changed"));
	auto imported = AudioImportPolicy::configured().prepareRecordedTake(std::move(*opened.source));
	if (!imported.audio) return savedUnattached(imported.code);
	auto pinnedSource = imported.audio->m_recordedTake;
	if (!pinnedSource || !pinnedSource->isCurrent()) return savedUnattached(QStringLiteral("recorded_source_unavailable"));
	const auto trackId = newId();
	const auto clipId = newId();
	auto reference = RecordedTakeSource::fromFacts(facts);
	if (!reference) return savedUnattached(QStringLiteral("invalid_recorded_source"));
	reference->trackId = trackId;
	reference->clipId = clipId;
	reference->placementRevision = plan.placementRevision;
	if (!RecordedTakeSource::validate(*reference).isEmpty()) return savedUnattached(QStringLiteral("invalid_attachment_identity"));

	QPointer<SampleTrack> trackGuard;
	QPointer<SampleClip> clipGuard;
	QString constructionError;
	{
		QSignalBlocker songSignals(song);
		auto* track = dynamic_cast<SampleTrack*>(Track::create(Track::Type::Sample, song));
		if (!track) constructionError = QStringLiteral("track_creation_failed");
		else
		{
			trackGuard = track;
			track->setName(QStringLiteral("Vocal take ") + facts.takeId);
			// Track::create registers the empty model under its own audio-engine change guard.
			// Detach it while its clip/source graph is built, then publish it only when complete.
			song->removeTrack(track);
			{
				QSignalBlocker trackSignals(track);
				try
				{
					auto* clip = new SampleClip(track);
					clipGuard = clip;
					QSignalBlocker clipSignals(clip);
					clip->setAutoResize(false);
					clip->m_recordedTakeMetadataPresent = true;
					constructionError = clip->adoptRecordedTakeAudio(std::move(imported.audio), *reference, false);
					if (constructionError == QStringLiteral("ok"))
					{
						clip->movePosition(TimePos(int(plan.positionTicks)));
						clip->changeLength(TimePos(int(plan.lengthTicks)));
						clip->setStartTimeOffset(TimePos(int(plan.offsetTicks)));
						clip->updateTrackClips();
					}
				}
				catch (const std::bad_alloc&) { constructionError = QStringLiteral("source_too_large"); }
				if (constructionError != QStringLiteral("ok"))
				{
					trackSignals.unblock();
					delete track;
					trackGuard.clear();
					clipGuard.clear();
				}
			}
			if (constructionError == QStringLiteral("ok"))
			{
				{
					const auto changes = audio->requestChangesGuard();
					song->addTrack(track);
				}
				song->updateLength();
				song->setModified();
			}
		}
	}
	if (!constructionError.isEmpty() && constructionError != QStringLiteral("ok"))
		return savedUnattached(constructionError, trackId, clipId);
	if (!trackGuard || !clipGuard) return savedUnattached(QStringLiteral("native_owner_invalidated"), trackId, clipId);
	Impl::Entry entry;
	entry.state = Impl::State::Attached;
	entry.track = trackGuard;
	entry.clip = clipGuard;
	entry.projectId = facts.projectId;
	entry.takeId = facts.takeId;
	entry.trackId = trackId;
	entry.clipId = clipId;
	entry.facts = facts;
	entry.plan = plan;
	entry.geometry = current;
	entry.token = std::move(pinnedSource);
	state.entries.emplace(key, std::move(entry));
	song->setModified();
	emit song->trackAdded(trackGuard.data());
	if (!trackGuard || !clipGuard)
	{
		auto& failed = state.entries[key];
		failed.state = Impl::State::Failed;
		failed.lastResult = savedUnattached(QStringLiteral("native_owner_invalidated"), trackId, clipId);
		return failed.lastResult;
	}
	emit trackGuard->clipAdded(clipGuard.data());
	if (!trackGuard || !clipGuard)
	{
		auto& failed = state.entries[key];
		failed.state = Impl::State::Failed;
		failed.lastResult = savedUnattached(QStringLiteral("native_owner_invalidated"), trackId, clipId);
		return failed.lastResult;
	}
	return {QStringLiteral("ok"), trackId, clipId, {}, true};
}
}
