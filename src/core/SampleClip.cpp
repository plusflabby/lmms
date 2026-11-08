/*
 * SampleClip.cpp
 *
 * Copyright (c) 2005-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */
 
#include "SampleClip.h"

#include <QDomElement>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QThread>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QMetaObject>
#include <new>
#include "AudioWindowPlayback.h"
#include "src/core/vocal/ManagedVocalAssetRoot.h"
#include "src/core/vocal/ManagedVocalAssetStore.h"
#include "src/core/vocal/ManagedVocalProjectOwnerStore.h"
#include "src/core/vocal/ManagedVocalPlaybackCache.h"
#include "ConfigManager.h"

#include "PatternStore.h"
#include "PathUtil.h"
#include "SampleClipView.h"
#include "SampleDecoder.h"
#include "RecordingServiceHost.h"
#include <QFile>
#include "SampleTrack.h"
#include "Song.h"

namespace lmms
{
namespace
{
QString managedVocalProjectId(Song* song)
{
	if (!song) return {};
	const auto owner = song->managedVocalOwner();
	return owner ? owner->projectId : song->recordingProjectIdentity().cachedIdentity().id;
}

QString holdManagedVocalOwner(Song* song, const std::shared_ptr<ManagedVocalAssetRoot>& root,
	std::shared_ptr<void>& hold)
{
	const auto owner = song ? song->managedVocalOwner() : nullptr;
	if (!owner) return QStringLiteral("ok");
	if (!root || !root->isAuthorized() || root->rootId() != owner->rootId)
		return QStringLiteral("project_identity_unverified");
	ManagedVocalProjectOwnerStore owners(root);
	const auto result = owners.holdVerified(song->projectFileName(), *owner);
	if (!result.hold) return result.code;
	hold = result.hold;
	return QStringLiteral("ok");
}

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
std::shared_ptr<const AudioWindowPlaybackView> loadPublishedAudioWindowView(
	const std::shared_ptr<const AudioWindowPlaybackView>& view)
{
	return std::atomic_load_explicit(&view, std::memory_order_acquire);
}

void storePublishedAudioWindowView(std::shared_ptr<const AudioWindowPlaybackView>& view,
	std::shared_ptr<const AudioWindowPlaybackView> replacement)
{
	std::atomic_store_explicit(&view, std::move(replacement), std::memory_order_release);
}

std::shared_ptr<const ManagedVocalPlaybackView> loadPublishedManagedVocalView(
	const std::shared_ptr<const ManagedVocalPlaybackView>& view)
{
	return std::atomic_load_explicit(&view, std::memory_order_acquire);
}

void storePublishedManagedVocalView(std::shared_ptr<const ManagedVocalPlaybackView>& view,
	std::shared_ptr<const ManagedVocalPlaybackView> replacement)
{
	std::atomic_store_explicit(&view, std::move(replacement), std::memory_order_release);
}

bool sameManagedVocalReference(const ManagedVocalVariantReference& left,
	const ManagedVocalVariantReference& right)
{
	return left.assetId == right.assetId && left.assetRootId == right.assetRootId
		&& left.mediaSha256 == right.mediaSha256 && left.manifestSha256 == right.manifestSha256
		&& left.frames == right.frames && left.sampleRate == right.sampleRate
		&& left.channels == right.channels;
}

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
}

SampleClip::SampleClip(Track* _track, Sample sample, bool isPlaying):
	Clip(_track),
	m_sample(std::move(sample)),
	m_isPlaying(false),
	m_startFrameOffset(0)
{
	saveJournallingState( false );
	setSampleFile( "" );
	restoreJournallingState();

	// we need to receive bpm-change-events, because then we have to
	// change length of this Clip
	connect(Engine::getSong(), &Song::tempoChanged, this, &SampleClip::tempoChanged, Qt::DirectConnection);
	connect(Engine::getSong(), &Song::timeSignatureChanged, this, &SampleClip::updateLength);

	//playbutton clicked or space key / on Export Song set isPlaying to false
	connect( Engine::getSong(), SIGNAL(playbackStateChanged()),
			this, SLOT(playbackPositionChanged()), Qt::DirectConnection );
	//care about loops and jumps
	connect(Engine::getSong(), &Song::playbackPositionJumped,
			this, &SampleClip::playbackPositionChanged, Qt::DirectConnection);
	//care about mute Clips
	connect( this, SIGNAL(dataChanged()), this, SLOT(playbackPositionChanged()));
	//care about mute track
	connect( getTrack()->getMutedModel(), SIGNAL(dataChanged()),
			this, SLOT(playbackPositionChanged()), Qt::DirectConnection );
	//care about Clip position
	connect( this, SIGNAL(positionChanged()), this, SLOT(updateTrackClips()));

	initializeAudioWindowConnections();
	initializeManagedVocalConnections();
	updateTrackClips();
}

SampleClip::SampleClip(Track* track)
	: SampleClip(track, Sample(), false)
{
}

SampleClip::SampleClip(const SampleClip& orig) : SampleClip(orig,orig.getTrack()) {}

SampleClip::SampleClip(const SampleClip& orig, Track* target) :
	Clip(orig,target),
	m_managedVocalAssociation(orig.m_managedVocalAssociation),
	m_managedVocalVariantStatus(orig.m_managedVocalVariantStatus),
	m_sample(std::move(orig.m_sample)),
	m_unavailableSampleFile(orig.m_unavailableSampleFile),
	m_sourceLoadError(orig.m_sourceLoadError),
	m_boundedSourceDigest(orig.m_boundedSourceDigest),
	m_boundedSourceIdentity(orig.m_boundedSourceIdentity),
	m_invalidSourceIdentity(orig.m_invalidSourceIdentity),
	m_sourceInfo(orig.m_sourceInfo),
	m_boundedSampleSource(orig.m_boundedSampleSource),
	m_recordedTakeMetadataPresent(orig.m_recordedTakeMetadataPresent),
	m_recordedTakeReference(orig.m_recordedTakeReference),
	m_recordedTakeToken(orig.m_recordedTakeToken),
	m_unavailableRecordedTake(orig.m_unavailableRecordedTake),
	m_audioWindowStorage(orig.m_audioWindowStorage),
	m_isPlaying(false),
	m_startFrameOffset(orig.m_startFrameOffset)
{
	if (m_audioWindowStorage.kind == AudioWindowStorageKind::InvalidMetadata)
	{
		m_audioWindowStorage.retainedInvalidXml = orig.m_audioWindowStorage.retainedInvalidXml.cloneNode(true).toDocument();
	}
	if (!orig.m_unavailableManagedVocalVariants.isNull())
	{
		m_unavailableManagedVocalVariants =
			orig.m_unavailableManagedVocalVariants.cloneNode(true).toDocument();
	}
	// we need to receive bpm-change-events, because then we have to
	// change length of this Clip
	connect(Engine::getSong(), &Song::tempoChanged, this, &SampleClip::tempoChanged, Qt::DirectConnection);
	// Versioned clones receive synchronous meter invalidation below. The old
	// string connection targeted a non-slot; do not alter legacy clone rendering.

	//playbutton clicked or space key / on Export Song set isPlaying to false
	connect( Engine::getSong(), SIGNAL(playbackStateChanged()),
			this, SLOT(playbackPositionChanged()), Qt::DirectConnection );
	//care about loops and jumps
	connect(Engine::getSong(), &Song::playbackPositionJumped,
			this, &SampleClip::playbackPositionChanged, Qt::DirectConnection);
	//care about mute Clips
	connect( this, SIGNAL(dataChanged()), this, SLOT(playbackPositionChanged()));
	//care about mute track
	connect( getTrack()->getMutedModel(), SIGNAL(dataChanged()),
			this, SLOT(playbackPositionChanged()), Qt::DirectConnection );
	//care about Clip position
	connect( this, SIGNAL(positionChanged()), this, SLOT(updateTrackClips()));

	initializeAudioWindowConnections();
	initializeManagedVocalConnections();
	updateTrackClips();
}




SampleClip::~SampleClip()
{
	m_managedVocalPlaybackLoadTimer.stop();
	m_managedVocalPlaybackLoadTicket.reset();
	m_audioWindowGuard->epoch.fetch_add(1,std::memory_order_acq_rel);
	m_managedVocalPlaybackGuard->epoch.fetch_add(1, std::memory_order_acq_rel);
	storePublishedManagedVocalView(m_managedVocalPlaybackView, {});
	auto sampletrack = dynamic_cast<SampleTrack*>(getTrack());
	if ( sampletrack )
	{
		sampletrack->updateClips();
	}
}

SampleClip* SampleClip::cloneToTrack(SampleTrack* target) const
{
	if(!target || !getTrack() || target->trackContainer()!=getTrack()->trackContainer()) return nullptr;
	return new SampleClip(*this,target);
}




void SampleClip::changeLength( const TimePos & _length )
{
	if(!allowsGenericClipEdits()) return;
	Clip::changeLength(std::max(static_cast<int>(_length), 1));
}



const QString& SampleClip::sampleFile() const
{
	return m_unavailableSampleFile.isEmpty() ? m_sample.sampleFile() : m_unavailableSampleFile;
}

bool SampleClip::recordedTakeSourceCurrent() const
{
	if (QThread::currentThread() != thread() || !m_recordedTakeReference || !m_recordedTakeToken) return false;
	return m_recordedTakeToken->isCurrent();
}

QString SampleClip::applyAudioWindow(const AudioSourceWindowV2& window,const AudioWindowGeometry& geometry)
{
	if(QThread::currentThread()!=thread()) return QStringLiteral("wrong_thread");
	if(m_recordedTakeMetadataPresent) return QStringLiteral("unsupported_recorded_source");
	if(m_loadingAudioWindow) return QStringLiteral("unsupported_audio_operation");
	if(m_audioWindowStorage.kind==AudioWindowStorageKind::InvalidMetadata) return m_audioWindowStorage.parseCode;
	if(m_audioWindowStorage.kind==AudioWindowStorageKind::V1) return QStringLiteral("unsupported_audio_operation");
	if(!m_boundedSampleSource || !m_sourceLoadError.isEmpty() || !m_sourceInfo)
		return m_sourceLoadError.isEmpty()?QStringLiteral("unsupported_audio_operation"):m_sourceLoadError;
	AudioWindowGeometry original{startPosition().getTicks(),length().getTicks(),startTimeOffset().getTicks(),
		getAutoResize(),m_sample.reversed(),isSolo(),name(),color()?color()->name(QColor::HexArgb):QString{},
		m_sample.frequency(),m_sample.amplification()};
	// This adapter changes only geometry and native window metadata, never source or properties.
	if(geometry.autoResize!=original.autoResize || geometry.reversed!=original.reversed || geometry.solo!=original.solo
		|| geometry.nativeName!=original.nativeName || geometry.color!=original.color || geometry.frequency!=original.frequency
		|| geometry.amplification!=original.amplification) return QStringLiteral("unsupported_audio_operation");
	auto* song=Engine::getSong();
	const AudioWindowWorld world{song->getTempo(),int(Engine::audioEngine()->outputSampleRate()),
		TimePos::ticksPerBar()/song->getTimeSigModel().getNumerator(),song->tempoModel().isAutomatedOrControlled()};
	auto authorization=AudioImportPolicy::configured().prepare(sampleFile(),m_boundedSourceDigest,m_boundedSourceIdentity);
	if(!authorization.audio) return authorization.code;
	const auto info=authorization.audio->info();
	const auto prior=hasAudioSourceWindow()?evaluateAudioWindow(m_audioWindowStorage,original,world,"ok",info)
		:convertLegacyAudioWindow(original,world,"ok",info);
	if(prior.code!="ok") return prior.code;
	if(prior.windowV2->base.phaseOriginFrame!=window.base.phaseOriginFrame || prior.windowV2->referenceEndFrame!=window.referenceEndFrame)
		return QStringLiteral("invalid_audio_window");
	AudioWindowStorage storage;storage.kind=AudioWindowStorageKind::V2;storage.valueV2=window;
	const auto allowed=evaluateAudioWindow(storage,geometry,world,"ok",info);
	if(allowed.code!="ok") return allowed.code;
	const auto epoch=m_audioWindowGuard->epoch.load(std::memory_order_acquire);
	auto candidate=makeAudioWindowPlaybackView(m_sample.buffer(),window,0,1,m_audioWindowGuard,epoch);
	if(!candidate) return QStringLiteral("unsupported_audio_operation");
	auto published=std::make_shared<AudioWindowPlaybackView>(std::move(*candidate));
	if(!authorization.audio->isCurrent()) return QStringLiteral("import_path_denied");
	{
		const auto changes=Engine::audioEngine()->requestChangesGuard();
		addJournalCheckPoint();
		m_loadingAudioWindow=true;
		const auto loading=qScopeGuard([this]{m_loadingAudioWindow=false;});
		// Retire any unversioned handle before changing its legacy Sample's mode.
		if(!hasAudioSourceWindow()) Engine::audioEngine()->removePlayHandlesOfTypes(getTrack(),PlayHandle::Type::SamplePlayHandle);
		published->expectedEpoch=m_audioWindowGuard->epoch.fetch_add(1,std::memory_order_acq_rel)+1;
		storePublishedAudioWindowView(m_audioWindowView, {});
		m_audioWindowStorage=std::move(storage);
		m_sample.m_audioWindowGuard=m_audioWindowGuard;
		{
			// Synchronous observers must never see half of the compound geometry.
			const QSignalBlocker notifications(this);
			Clip::movePosition(TimePos(int(geometry.positionTicks)));
			Clip::changeLength(TimePos(int(geometry.lengthTicks)));
			Clip::setStartTimeOffset(TimePos(int(geometry.offsetTicks)));
		}
		m_startFrameOffset=-int(window.base.inFrame);
		setIsPlaying(false);
		storePublishedAudioWindowView(m_audioWindowView, std::move(published));
	}
	song->setModified();
	if(original.positionTicks!=geometry.positionTicks) emit positionChanged();
	if(original.lengthTicks!=geometry.lengthTicks) emit lengthChanged();
	emit sampleChanged();
	song->warnAudioWindowCompatibility();
	return QStringLiteral("ok");
}

QString SampleClip::audioWindowStatus() const
{
	if (!hasAudioSourceWindow()) return QStringLiteral("ok");
	if(m_audioWindowStorage.kind==AudioWindowStorageKind::V2)
	{
		const auto result=audioWindowEligibility();
		if(result.code!="ok") return result.code;
		return verifyAudioWindowReferenceProfile()?QStringLiteral("ok"):QStringLiteral("unsupported_audio_operation");
	}
	if (m_audioWindowStorage.kind == AudioWindowStorageKind::InvalidMetadata) return m_audioWindowStorage.parseCode;
	if (!m_sourceLoadError.isEmpty()) return m_sourceLoadError;
	if (!m_boundedSampleSource || !m_sourceInfo) return QStringLiteral("unsupported_audio_operation");
	const auto& w = m_audioWindowStorage.kind == AudioWindowStorageKind::V2
		? m_audioWindowStorage.valueV2->base : *m_audioWindowStorage.value;
	if (m_sourceInfo->frames != quint64(w.sourceFrameCount) || m_sourceInfo->sampleRate != w.sourceRate)
		return QStringLiteral("audio_source_changed");
	// Persistence ships fail-closed until independent renderer/scheduler gates pass.
	return QStringLiteral("unsupported_audio_operation");
}

AudioWindowEligibility SampleClip::audioWindowEligibility() const
{
	if (m_recordedTakeMetadataPresent) return {QStringLiteral("unsupported_recorded_source"), {}};
	AudioWindowGeometry geometry{startPosition().getTicks(),length().getTicks(),startTimeOffset().getTicks(),
		getAutoResize(),m_sample.reversed(),isSolo(),name(),color()?color()->name(QColor::HexArgb):QString{},
		m_sample.frequency(),m_sample.amplification()};
	auto* song=Engine::getSong();
	AudioWindowWorld world{song->getTempo(),int(Engine::audioEngine()->outputSampleRate()),
		TimePos::ticksPerBar()/song->getTimeSigModel().getNumerator(),song->tempoModel().isAutomatedOrControlled()};
	const auto sourceCode=!m_sourceLoadError.isEmpty()?m_sourceLoadError:
		!m_boundedSampleSource?QStringLiteral("unsupported_audio_operation"):QStringLiteral("ok");
	return evaluateAudioWindow(m_audioWindowStorage,geometry,world,sourceCode,m_sourceInfo);
}

std::shared_ptr<const AudioWindowPlaybackView> SampleClip::audioWindowPlaybackView() const
{
	auto view=loadPublishedAudioWindowView(m_audioWindowView);
	if(view && view->guard->epoch.load(std::memory_order_acquire)==view->expectedEpoch) return view;
	return {};
}

void SampleClip::initializeAudioWindowConnections()
{
	const auto invalidate=[this]
	{
		if(!hasAudioSourceWindow()) return;
		m_audioWindowGuard->epoch.fetch_add(1,std::memory_order_acq_rel);
		storePublishedAudioWindowView(m_audioWindowView, {});
		// Graph producers may still own an automation-clip mutex. Invalidate
		// synchronously, but traverse/requalify only after the edit completes.
		QMetaObject::invokeMethod(this,[this]{refreshAudioWindowPlayback();},Qt::QueuedConnection);
	};
	connect(Engine::getSong(),&Song::tempoAutomationTopologyChanged,this,invalidate,Qt::DirectConnection);
	connect(Engine::getSong(),&Song::timeSignatureChanged,this,invalidate,Qt::DirectConnection);
	connect(this,&SampleClip::sampleChanged,this,&SampleClip::refreshAudioWindowPlayback,Qt::DirectConnection);
	connect(this,&Clip::positionChanged,this,&SampleClip::refreshAudioWindowPlayback,Qt::DirectConnection);
	connect(this,&Clip::lengthChanged,this,&SampleClip::refreshAudioWindowPlayback,Qt::DirectConnection);
	connect(Engine::audioEngine(),&AudioEngine::sampleRateChanged,this,&SampleClip::refreshAudioWindowPlayback,Qt::DirectConnection);
	refreshAudioWindowPlayback();
}

void SampleClip::refreshAudioWindowPlayback()
{
	if(!hasAudioSourceWindow() || m_loadingAudioWindow) return;
	const auto epoch=m_audioWindowGuard->epoch.fetch_add(1,std::memory_order_acq_rel)+1;
	storePublishedAudioWindowView(m_audioWindowView, {});
	if(QThread::currentThread()!=thread())
	{
		QMetaObject::invokeMethod(this,[this]{refreshAudioWindowPlayback();},Qt::QueuedConnection);
		return;
	}
	m_sample.m_audioWindowGuard=m_audioWindowGuard;
	const auto code=audioWindowStatus();
	if(code=="ok")
	{
		auto view=makeAudioWindowPlaybackView(m_sample.buffer(),*m_audioWindowStorage.valueV2,0,
			m_sample.amplification(),m_audioWindowGuard,epoch);
		if(view)
		{
			auto published=std::make_shared<const AudioWindowPlaybackView>(std::move(*view));
			const auto changes=Engine::audioEngine()->requestChangesGuard();
			setIsPlaying(false);
			storePublishedAudioWindowView(m_audioWindowView, std::move(published));
		}
	}
	emit audioWindowStatusChanged(code);
}

std::shared_ptr<const ManagedVocalPlaybackView> SampleClip::managedVocalPlaybackView() const
{
	auto view = loadPublishedManagedVocalView(m_managedVocalPlaybackView);
	if (view && view->guard
		&& view->guard->epoch.load(std::memory_order_acquire) == view->expectedEpoch)
	{
		return view;
	}
	return {};
}

void SampleClip::setManagedVocalVariantStatus(const QString& status)
{
	if (m_managedVocalVariantStatus == status)
	{
		return;
	}
	m_managedVocalVariantStatus = status;
	emit managedVocalVariantStatusChanged(status);
}

ManagedVocalPlaybackPreparation SampleClip::prepareManagedVocalPlayback(
	const ManagedVocalVariantAssociation& association,
	std::shared_ptr<const std::vector<float>> samples,
	std::shared_ptr<const ManagedVocalPlaybackGuard> guard,
	quint64 expectedEpoch,
	bool autoResize,
	bool checkGeometry) const
{
	if (hasAudioSourceWindow() || association.playback.visualLengthTicks <= 0)
	{
		return {QStringLiteral("unsupported_clip_state"), {}};
	}
	if (QThread::currentThread() != thread())
	{
		return {QStringLiteral("wrong_thread"), {}};
	}
	auto* song = Engine::getSong();
	const auto rate = int(Engine::audioEngine()->outputSampleRate());
	const auto numerator = song->getTimeSigModel().getNumerator();
	const auto denominator = song->getTimeSigModel().getDenominator();
	if (numerator <= 0 || denominator <= 0)
	{
		return {QStringLiteral("unsupported_playback_world"), {}};
	}
	const auto ticksPerQuarterNote = DefaultTicksPerBar / 4;
	const ManagedVocalPlaybackWorld world{song->getTempo(), rate, ticksPerQuarterNote,
		song->tempoModel().isAutomatedOrControlled(), numerator, denominator};
	if (world.tempoAutomationPresent || world.tempoBpm != association.playback.tempoBpm
		|| world.outputRate != association.playback.outputRate
		|| world.ticksPerQuarterNote != association.playback.ticksPerQuarterNote
		|| world.timeSignatureNumerator != association.playback.timeSignatureNumerator
		|| world.timeSignatureDenominator != association.playback.timeSignatureDenominator)
	{
		return {QStringLiteral("unsupported_playback_world"), {}};
	}
	const auto visualExtent = managedVocalVisualExtent(
		association.active.frames, association.active.sampleRate, world.tempoBpm);
	if (visualExtent.code != QStringLiteral("ok")
		|| visualExtent.visualLengthTicks != association.playback.visualLengthTicks
		|| visualExtent.nativeCoverageFrames != association.playback.nativeVisualCoverageFrames
		|| visualExtent.exactResidualNumerator != association.playback.endpointResidualNumerator
		|| visualExtent.exactResidualDenominator != association.playback.endpointResidualDenominator)
	{
		return {QStringLiteral("unrepresentable_audio_boundary"), {}};
	}
	if (m_sample.reversed() || autoResize || m_sample.frequency() != DefaultBaseFreq
		|| startTimeOffset().getTicks() != 0)
	{
		return {QStringLiteral("unsupported_clip_state"), {}};
	}
	if (checkGeometry && length().getTicks() != association.playback.visualLengthTicks)
	{
		return {QStringLiteral("unrepresentable_audio_boundary"), {}};
	}
	if (!samples || samples->size() != std::size_t(association.active.frames)
		* std::size_t(association.active.channels))
	{
		return {QStringLiteral("asset_integrity_failed"), {}};
	}
	return makeManagedVocalPlaybackView(std::move(samples), association.active.sampleRate,
		association.active.channels, world, false, false, std::move(guard), expectedEpoch, m_sample.amplification());
}

QString SampleClip::requestManagedVocalAsset(
	const ManagedVocalVariantReference& reference,
	std::shared_ptr<const std::vector<float>>& samples)
{
	if (QThread::currentThread() != thread()) return QStringLiteral("wrong_thread");
	auto* song = Engine::getSong();
	if (!song) return QStringLiteral("stale_id");
	const auto projectId = managedVocalProjectId(song);
	if (projectId.isEmpty()) return QStringLiteral("project_identity_unverified");
	auto approval = ManagedVocalAssetRoot::authorizeConfiguredRoot(song->managedVocalOwner()
		? song->recordingProjectIdentity().managedRootLibraryForOwner() : song->recordingProjectIdentity().managedRootLibrary());
	if (!approval.root) return approval.code;
	std::shared_ptr<void> projectHold;
	const auto ownerCode = holdManagedVocalOwner(song, approval.root, projectHold);
	if (ownerCode != QStringLiteral("ok")) return ownerCode;
	if (approval.root->rootId() != reference.assetRootId)
	{
		return QStringLiteral("asset_root_mismatch");
	}
	if (m_managedVocalPreloadedSamples
		&& m_managedVocalPreloadedProjectId == projectId
		&& sameManagedVocalReference(m_managedVocalPreloadedReference, reference))
	{
		samples = m_managedVocalPreloadedSamples;
		return QStringLiteral("ok");
	}
	if (m_managedVocalPlaybackLoadTicket
		&& m_managedVocalPlaybackLoadingProjectId == projectId
		&& sameManagedVocalReference(m_managedVocalPlaybackLoadingReference, reference))
	{
		if (!m_managedVocalPlaybackLoadTicket->isComplete())
		{
			return QStringLiteral("asset_loading");
		}
		pollManagedVocalPlaybackLoad();
		if (m_managedVocalPreloadedSamples
			&& m_managedVocalPreloadedProjectId == projectId
			&& sameManagedVocalReference(m_managedVocalPreloadedReference, reference))
		{
			samples = m_managedVocalPreloadedSamples;
			return QStringLiteral("ok");
		}
		return m_managedVocalVariantStatus;
	}
	if (m_managedVocalPlaybackLoadTicket)
	{
		m_managedVocalPlaybackLoadTimer.stop();
		m_managedVocalPlaybackLoadTicket.reset();
	}
	m_managedVocalPreloadedReference = {};
	m_managedVocalPreloadedProjectId.clear();
	m_managedVocalPreloadedSamples.reset();
	m_managedVocalPlaybackLoadingReference = {};
	m_managedVocalPlaybackLoadingProjectId.clear();
	ManagedVocalAssetStore store(approval.root);
	const auto request = ManagedVocalPlaybackCache::instance().requestLoad(store, reference, projectId);
	if (request.code != QStringLiteral("ok") || !request.ticket)
	{
		if (request.code == QStringLiteral("job_busy"))
		{
			m_managedVocalPlaybackLoadingReference = reference;
			m_managedVocalPlaybackLoadingProjectId = projectId;
			if (!m_managedVocalAssociation
				|| sameManagedVocalReference(m_managedVocalAssociation->active, reference))
			{
				m_managedVocalPlaybackLoadTimer.setInterval(100);
				m_managedVocalPlaybackLoadTimer.start();
				setManagedVocalVariantStatus(request.code);
			}
		}
		return request.code;
	}
	if (request.ticket->isComplete())
	{
		const auto result = request.ticket->result();
		if (result.code != QStringLiteral("ok") || !result.samples)
		{
			return result.code;
		}
		m_managedVocalPreloadedReference = reference;
		m_managedVocalPreloadedProjectId = projectId;
		m_managedVocalPreloadedSamples = result.samples;
		samples = std::move(result.samples);
		return QStringLiteral("ok");
	}
	m_managedVocalPlaybackLoadingReference = reference;
	m_managedVocalPlaybackLoadingProjectId = projectId;
	m_managedVocalPlaybackLoadTicket = request.ticket;
	m_managedVocalPlaybackLoadTimer.setInterval(20);
	m_managedVocalPlaybackLoadTimer.start();
	if (!m_managedVocalAssociation
		|| sameManagedVocalReference(m_managedVocalAssociation->active, reference))
	{
		setManagedVocalVariantStatus(QStringLiteral("asset_loading"));
	}
	return QStringLiteral("asset_loading");
}

void SampleClip::pollManagedVocalPlaybackLoad()
{
	if (QThread::currentThread() != thread())
	{
		return;
	}
	if (!m_managedVocalPlaybackLoadTicket)
	{
		if (m_managedVocalVariantStatus != QStringLiteral("job_busy")
			|| m_managedVocalPlaybackLoadingReference.assetId.isEmpty())
		{
			return;
		}
		const auto reference = m_managedVocalPlaybackLoadingReference;
		std::shared_ptr<const std::vector<float>> samples;
		const auto code = requestManagedVocalAsset(reference, samples);
		if (code == QStringLiteral("ok"))
		{
			m_managedVocalPlaybackLoadTimer.stop();
			if (m_managedVocalAssociation
				&& sameManagedVocalReference(m_managedVocalAssociation->active, reference))
			{
				refreshManagedVocalPlayback();
			}
			else if (!m_managedVocalAssociation)
			{
				setManagedVocalVariantStatus(QStringLiteral("asset_ready"));
			}
		}
		else if (code != QStringLiteral("job_busy") && code != QStringLiteral("asset_loading"))
		{
			m_managedVocalPlaybackLoadTimer.stop();
			setManagedVocalVariantStatus(code);
		}
		return;
	}
	if (!m_managedVocalPlaybackLoadTicket->isComplete()) return;
	m_managedVocalPlaybackLoadTimer.stop();
	const auto reference = m_managedVocalPlaybackLoadingReference;
	const auto expectedProjectId = m_managedVocalPlaybackLoadingProjectId;
	auto ticket = std::move(m_managedVocalPlaybackLoadTicket);
	const auto result = ticket->result();
	auto* song = Engine::getSong();
	const auto currentProjectId = managedVocalProjectId(song);
	if (expectedProjectId.isEmpty() || currentProjectId != expectedProjectId)
	{
		m_managedVocalPreloadedReference = {};
		m_managedVocalPreloadedProjectId.clear();
		m_managedVocalPreloadedSamples.reset();
		setManagedVocalVariantStatus(QStringLiteral("asset_project_mismatch"));
		return;
	}
	std::shared_ptr<ManagedVocalAssetRoot> validatedRoot;
	std::shared_ptr<void> validatedOwner;
	if (song && song->managedVocalOwner())
	{
		auto approval = ManagedVocalAssetRoot::authorizeConfiguredRoot(song->recordingProjectIdentity().managedRootLibraryForOwner());
		const auto ownerCode = approval.root ? holdManagedVocalOwner(song, approval.root, validatedOwner) : approval.code;
		if (ownerCode != QStringLiteral("ok"))
		{
			m_managedVocalPreloadedReference = {};
			m_managedVocalPreloadedProjectId.clear();
			m_managedVocalPreloadedSamples.reset();
			setManagedVocalVariantStatus(ownerCode);
			return;
		}
		validatedRoot = std::move(approval.root);
	}
	if (result.code == QStringLiteral("ok") && result.samples
		&& result.samples->size() == std::size_t(reference.frames) * std::size_t(reference.channels))
	{
		m_managedVocalPreloadedReference = reference;
		m_managedVocalPreloadedProjectId = expectedProjectId;
		m_managedVocalPreloadedSamples = result.samples;
		if (m_managedVocalAssociation
			&& sameManagedVocalReference(m_managedVocalAssociation->active, reference))
		{
			refreshManagedVocalPlayback();
		}
		else if (!m_managedVocalAssociation
			&& m_managedVocalVariantStatus == QStringLiteral("asset_loading"))
		{
			setManagedVocalVariantStatus(QStringLiteral("asset_ready"));
		}
		return;
	}
	const auto code = result.code == QStringLiteral("ok")
		? QStringLiteral("asset_integrity_failed") : result.code;
	if (m_managedVocalAssociation
		&& sameManagedVocalReference(m_managedVocalAssociation->active, reference))
	{
		setIsPlaying(false);
		setManagedVocalVariantStatus(code);
	}
	else if (m_managedVocalVariantStatus == QStringLiteral("asset_loading"))
	{
		setManagedVocalVariantStatus(code);
	}
}

void SampleClip::refreshManagedVocalPlayback()
{
	if (m_loadingManagedVocalAssociation.load(std::memory_order_acquire))
	{
		return;
	}
	if (QThread::currentThread() != thread())
	{
		QMetaObject::invokeMethod(this, [this] { refreshManagedVocalPlayback(); }, Qt::QueuedConnection);
		return;
	}
	const auto previousView = managedVocalPlaybackView();
	const auto expectedEpoch = m_managedVocalPlaybackGuard->epoch.fetch_add(1, std::memory_order_acq_rel) + 1;
	storePublishedManagedVocalView(m_managedVocalPlaybackView, {});
	if (!m_managedVocalAssociation)
	{
		if (!m_unavailableManagedVocalVariants.isNull())
		{
			setManagedVocalVariantStatus(QStringLiteral("invalid_managed_variant"));
		}
		else if (!m_managedVocalPlaybackLoadTicket)
		{
			setManagedVocalVariantStatus(QStringLiteral("ok"));
		}
		return;
	}
	std::shared_ptr<const std::vector<float>> samples;
	std::shared_ptr<ManagedVocalAssetRoot> validatedRoot;
	std::shared_ptr<void> validatedOwner;
	auto* currentSong = Engine::getSong();
	const auto currentProjectId = managedVocalProjectId(currentSong);
	if (currentSong && previousView && m_managedVocalPreloadedSamples
		&& m_managedVocalPreloadedProjectId == currentProjectId
		&& sameManagedVocalReference(m_managedVocalAssociation->active, m_managedVocalPreloadedReference))
	{
		if (!currentSong->managedVocalOwner()) samples = previousView->interleavedSamples;
		else
		{
			auto approval = ManagedVocalAssetRoot::authorizeConfiguredRoot(currentSong->recordingProjectIdentity().managedRootLibraryForOwner());
			if (approval.root && holdManagedVocalOwner(currentSong, approval.root, validatedOwner) == QStringLiteral("ok"))
			{
				validatedRoot = std::move(approval.root);
				samples = previousView->interleavedSamples;
			}
		}
	}
	const auto loadCode = samples ? QStringLiteral("ok")
		: requestManagedVocalAsset(m_managedVocalAssociation->active, samples);
	if (loadCode == QStringLiteral("asset_loading"))
	{
		setManagedVocalVariantStatus(loadCode);
		return;
	}
	if (loadCode != QStringLiteral("ok") || !samples)
	{
		setIsPlaying(false);
		setManagedVocalVariantStatus(loadCode == QStringLiteral("ok")
			? QStringLiteral("asset_integrity_failed") : loadCode);
		return;
	}
	const auto prepared = prepareManagedVocalPlayback(*m_managedVocalAssociation,
		std::move(samples), m_managedVocalPlaybackGuard, expectedEpoch, getAutoResize(), true);
	if (prepared.code != QStringLiteral("ok") || !prepared.view
		|| m_managedVocalPlaybackGuard->epoch.load(std::memory_order_acquire) != expectedEpoch)
	{
		setIsPlaying(false);
		setManagedVocalVariantStatus(prepared.code == QStringLiteral("ok")
			? QStringLiteral("plan_stale") : prepared.code);
		return;
	}
	storePublishedManagedVocalView(m_managedVocalPlaybackView, prepared.view);
	setManagedVocalVariantStatus(QStringLiteral("ok"));
	emit sampleChanged();
}

void SampleClip::invalidateManagedVocalPlayback()
{
	if (!hasManagedVocalVariant() || m_loadingManagedVocalAssociation.load(std::memory_order_acquire))
	{
		return;
	}
	m_managedVocalPlaybackGuard->epoch.fetch_add(1, std::memory_order_acq_rel);
	storePublishedManagedVocalView(m_managedVocalPlaybackView, {});
	if (QThread::currentThread() == thread())
	{
		setIsPlaying(false);
		refreshManagedVocalPlayback();
	}
	else
	{
		QMetaObject::invokeMethod(this, [this] {
			setIsPlaying(false);
			refreshManagedVocalPlayback();
		}, Qt::QueuedConnection);
	}
}

void SampleClip::initializeManagedVocalConnections()
{
	m_managedVocalPlaybackLoadTimer.setInterval(20);
	connect(&m_managedVocalPlaybackLoadTimer, &QTimer::timeout,
		this, &SampleClip::pollManagedVocalPlaybackLoad);
	connect(Engine::getSong(), &Song::timeSignatureChanged, this,
		[this](int, int) { invalidateManagedVocalPlayback(); }, Qt::DirectConnection);
	connect(Engine::getSong(), &Song::tempoAutomationTopologyChanged, this,
		[this] { invalidateManagedVocalPlayback(); }, Qt::DirectConnection);
	connect(Engine::audioEngine(), &AudioEngine::sampleRateChanged, this,
		[this] { invalidateManagedVocalPlayback(); }, Qt::DirectConnection);
	connect(this, &Clip::lengthChanged, this, &SampleClip::invalidateManagedVocalPlayback,
		Qt::DirectConnection);
	connect(this, &SampleClip::wasReversed, this, &SampleClip::invalidateManagedVocalPlayback,
		Qt::DirectConnection);
	connect(ConfigManager::inst(), &ConfigManager::valueChanged, this,
		[this](const QString& section, const QString& key, const QString&) {
			if (section == QStringLiteral("vocal_assets") && key == QStringLiteral("managed_root"))
			{
				invalidateManagedVocalPlayback();
			}
		}, Qt::DirectConnection);
	refreshManagedVocalPlayback();
}

QString SampleClip::applyManagedVocalVariantAssociation(
	const ManagedVocalVariantAssociation& requested)
{
	return applyManagedVocalVariantAssociationInternal(requested, true);
}

QString SampleClip::managedVocalVariantAssetId() const
{
	if(m_managedVocalAssociation)return m_managedVocalAssociation->active.assetId;
	if(m_unavailableManagedVocalVariants.isNull())return {};
	const auto wrapper=m_unavailableManagedVocalVariants.documentElement();
	for(auto child=wrapper.firstChild();!child.isNull();child=child.nextSibling())
		if(child.isElement() && child.nodeName()==QStringLiteral("managed_vocal_variant"))
			return child.toElement().attribute(QStringLiteral("asset_id"));
	return {};
}

QByteArray SampleClip::managedVocalVariantStateDigest() const
{
	if(!m_managedVocalAssociation && m_unavailableManagedVocalVariants.isNull())return {};
	QDomDocument document;
	auto wrapper=document.createElement(QStringLiteral("managed_variant_state"));document.appendChild(wrapper);
	if(m_managedVocalAssociation)
	{
		const auto association=writeManagedVocalVariantAssociation(document,*m_managedVocalAssociation);
		if(association.isNull())return {};
		wrapper.appendChild(association);
	}
	else
	{
		const auto original=m_unavailableManagedVocalVariants.documentElement();
		for(auto child=original.firstChild();!child.isNull();child=child.nextSibling())
			wrapper.appendChild(document.importNode(child,true));
	}
	return QCryptographicHash::hash(document.toByteArray(-1),QCryptographicHash::Sha256);
}

QString SampleClip::preloadManagedVocalVariant(const ManagedVocalVariantReference& reference)
{
	if (QThread::currentThread() != thread()) return QStringLiteral("wrong_thread");
	std::shared_ptr<const std::vector<float>> samples;
	return requestManagedVocalAsset(reference,samples);
}

QString SampleClip::applyManagedVocalVariantAssociationInternal(
	const ManagedVocalVariantAssociation& requested,
	bool retainCurrentForRevert)
{
	if (QThread::currentThread() != thread())
	{
		return QStringLiteral("wrong_thread");
	}
	if (m_loadingManagedVocalAssociation.load(std::memory_order_acquire))
	{
		return QStringLiteral("unsupported_audio_operation");
	}
	auto association = requested;
	if (m_managedVocalAssociation)
	{
		association.originalAutoResize = m_managedVocalAssociation->originalAutoResize;
		association.originalLengthTicks = m_managedVocalAssociation->originalLengthTicks;
		association.originalOffsetTicks = m_managedVocalAssociation->originalOffsetTicks;
		if (retainCurrentForRevert && !association.previousVariant)
		{
			association.previousVariant = m_managedVocalAssociation->active;
			association.previousPlayback = m_managedVocalAssociation->playback;
		}
	}
	else
	{
		association.originalAutoResize = getAutoResize();
		association.originalLengthTicks = length().getTicks();
		association.originalOffsetTicks = startTimeOffset().getTicks();
	}
	if (const auto code = validateManagedVocalVariantAssociation(association);
		code != QStringLiteral("ok"))
	{
		return code;
	}
	if (hasAudioSourceWindow() || startTimeOffset().getTicks() != 0)
	{
		return QStringLiteral("unsupported_clip_state");
	}
	const auto startingEpoch = m_managedVocalPlaybackGuard->epoch.load(std::memory_order_acquire);
	if (startingEpoch == std::numeric_limits<quint64>::max())
	{
		return QStringLiteral("resource_limit");
	}
	std::shared_ptr<ManagedVocalPlaybackGuard> candidateGuard;
	try
	{
		candidateGuard = std::make_shared<ManagedVocalPlaybackGuard>();
	}
	catch (const std::bad_alloc&)
	{
		return QStringLiteral("resource_limit");
	}
	std::shared_ptr<const std::vector<float>> samples;
	const auto loadCode = requestManagedVocalAsset(association.active, samples);
	if (loadCode != QStringLiteral("ok") || !samples)
	{
		return loadCode;
	}
	const auto prepared = prepareManagedVocalPlayback(association, std::move(samples), candidateGuard,
		candidateGuard->epoch.load(std::memory_order_acquire), false, false);
	if (prepared.code != QStringLiteral("ok") || !prepared.view)
	{
		return prepared.code;
	}
	if (m_managedVocalPlaybackGuard->epoch.load(std::memory_order_acquire) != startingEpoch)
	{
		return QStringLiteral("plan_stale");
	}
	std::shared_ptr<ManagedVocalPlaybackView> writableView;
	try
	{
		writableView = std::make_shared<ManagedVocalPlaybackView>(*prepared.view);
	}
	catch (const std::bad_alloc&)
	{
		return QStringLiteral("resource_limit");
	}
	const auto nextEpoch = startingEpoch + 1;
	writableView->guard = m_managedVocalPlaybackGuard;
	writableView->expectedEpoch = nextEpoch;
	{
		const auto changes = Engine::audioEngine()->requestChangesGuard();
		if (m_managedVocalPlaybackGuard->epoch.load(std::memory_order_acquire) != startingEpoch)
		{
			return QStringLiteral("plan_stale");
		}
		auto expectedEpoch = startingEpoch;
		if (!m_managedVocalPlaybackGuard->epoch.compare_exchange_strong(expectedEpoch, nextEpoch,
			std::memory_order_acq_rel, std::memory_order_acquire))
		{
			return QStringLiteral("plan_stale");
		}
		addJournalCheckPoint();
		m_loadingManagedVocalAssociation.store(true, std::memory_order_release);
		Engine::audioEngine()->removePlayHandlesOfTypes(getTrack(), PlayHandle::Type::SamplePlayHandle);
		Clip::setAutoResize(false);
		Clip::changeLength(TimePos(int(association.playback.visualLengthTicks)));
		Clip::setStartTimeOffset(TimePos(0));
		m_managedVocalAssociation = std::move(association);
		m_unavailableManagedVocalVariants = QDomDocument{};
		storePublishedManagedVocalView(m_managedVocalPlaybackView, std::move(writableView));
		m_loadingManagedVocalAssociation.store(false, std::memory_order_release);
		setIsPlaying(false);
	}
	setManagedVocalVariantStatus(QStringLiteral("ok"));
	emit sampleChanged();
	Engine::getSong()->setModified();
	refreshManagedVocalPlayback();
	return managedVocalVariantStatus();
}

QString SampleClip::revertManagedVocalVariant()
{
	if (QThread::currentThread() != thread()) return QStringLiteral("wrong_thread");
	if (!m_managedVocalAssociation)
	{
		if (m_unavailableManagedVocalVariants.isNull())
		{
			return QStringLiteral("invalid_parameter");
		}
		resetManagedVocalVariant();
		return QStringLiteral("ok");
	}
	const auto current = *m_managedVocalAssociation;
	const QPointer<SampleClip> self(this);
	{
		const auto changes = Engine::audioEngine()->requestChangesGuard();
		addJournalCheckPoint();
		m_loadingManagedVocalAssociation.store(true, std::memory_order_release);
		m_managedVocalPlaybackGuard->epoch.fetch_add(1, std::memory_order_acq_rel);
		storePublishedManagedVocalView(m_managedVocalPlaybackView, {});
		m_managedVocalAssociation.reset();
		m_unavailableManagedVocalVariants = QDomDocument{};
		Clip::setAutoResize(current.originalAutoResize);
		Clip::changeLength(TimePos(int(current.originalLengthTicks)));
		Clip::setStartTimeOffset(TimePos(int(current.originalOffsetTicks)));
		m_loadingManagedVocalAssociation.store(false, std::memory_order_release);
		setIsPlaying(false);
	}
	Engine::audioEngine()->removePlayHandlesOfTypes(getTrack(), PlayHandle::Type::SamplePlayHandle);
	setManagedVocalVariantStatus(QStringLiteral("ok"));
	Engine::getSong()->setModified();
	emit sampleChanged();
	if (!self) return QStringLiteral("ok");
	refreshManagedVocalPlayback();
	return QStringLiteral("ok");
}

void SampleClip::resetManagedVocalVariant()
{
	m_managedVocalPlaybackLoadTimer.stop();
	m_managedVocalPlaybackLoadTimer.setInterval(20);
	m_managedVocalPlaybackLoadTicket.reset();
	m_managedVocalPlaybackLoadingProjectId.clear();
	m_managedVocalPreloadedReference = {};
	m_managedVocalPreloadedProjectId.clear();
	m_managedVocalPreloadedSamples.reset();
	if (m_managedVocalAssociation && !m_loadingManagedVocalAssociation.load(std::memory_order_acquire))
	{
		const auto current = *m_managedVocalAssociation;
		m_loadingManagedVocalAssociation.store(true, std::memory_order_release);
		Clip::setAutoResize(current.originalAutoResize);
		Clip::changeLength(TimePos(int(current.originalLengthTicks)));
		Clip::setStartTimeOffset(TimePos(int(current.originalOffsetTicks)));
		m_loadingManagedVocalAssociation.store(false, std::memory_order_release);
	}
	m_managedVocalPlaybackGuard->epoch.fetch_add(1, std::memory_order_acq_rel);
	storePublishedManagedVocalView(m_managedVocalPlaybackView, {});
	m_managedVocalAssociation.reset();
	m_unavailableManagedVocalVariants = QDomDocument{};
	setManagedVocalVariantStatus(QStringLiteral("ok"));
}

bool SampleClip::hasSampleFileLoaded(const QString & filename) const
{
	return m_sample.sampleFile() == filename;
}

void SampleClip::setSampleBuffer(std::shared_ptr<const SampleBuffer> sb)
{
	if (hasAudioSourceWindow()) return;
	resetManagedVocalVariant();
	resetRecordedTakeSource();
	{
		const auto guard = Engine::audioEngine()->requestChangesGuard();
		m_sample = Sample(std::move(sb));
		m_unavailableSampleFile.clear();
		m_sourceLoadError.clear();
		m_boundedSourceDigest.clear();
		m_boundedSourceIdentity.clear(); m_invalidSourceIdentity = false; m_sourceInfo.reset();
		m_boundedSampleSource = false;
	}
	updateLength();

	emit sampleChanged();
	emit playbackPositionChanged();

	Engine::getSong()->setModified();
}

void SampleClip::setSampleFile(const QString& sf)
{
	if (hasAudioSourceWindow()) return;
	resetManagedVocalVariant();
	if (!sf.isEmpty()) resetRecordedTakeSource();
	// An explicit source replacement establishes a new expected recording.
	if (!sf.isEmpty()) m_invalidSourceIdentity = false;
	setSampleFileImpl(sf, {});
}

QString SampleClip::adoptAuthorizedAudio(std::unique_ptr<AudioImportPolicy::Prepared> audio)
{
	if (hasAudioSourceWindow()) return QStringLiteral("unsupported_audio_operation");
	if (m_recordedTakeMetadataPresent) return QStringLiteral("recorded_source_requires_exact_capability");
	if (!audio || !audio->m_buffer) return QStringLiteral("unsupported_audio_operation");
	if (audio->isRecordedTake()) return QStringLiteral("recorded_source_reference_required");
	if (!audio->isCurrent()) return QStringLiteral("plan_stale");
	resetManagedVocalVariant();
	setStartTimeOffset(0);
	{
		const auto guard = Engine::audioEngine()->requestChangesGuard();
		m_sample = Sample(audio->m_buffer);
		m_boundedSampleSource = true; m_invalidSourceIdentity = false;
		m_boundedSourceDigest = audio->m_info.sourceSha256; m_boundedSourceIdentity = audio->identity();
		m_sourceInfo = audio->m_info; m_unavailableSampleFile.clear(); m_sourceLoadError.clear();
	}
	updateLength(); emit sampleChanged(); emit playbackPositionChanged();
	Engine::getSong()->setModified();
	return QStringLiteral("ok");
}

QString SampleClip::adoptRecordedTakeAudio(std::unique_ptr<AudioImportPolicy::Prepared> audio,
	const recording::RecordedTakeReference& reference, bool notify)
{
	if (hasAudioSourceWindow() || !m_recordedTakeMetadataPresent || !audio || !audio->m_buffer
		|| !audio->m_recordedTake || !audio->isRecordedTake()) return QStringLiteral("unsupported_recorded_source");
	if (!audio->isCurrent()) return QStringLiteral("recording_source_unavailable");
	if (!recording::RecordedTakeSource::matches(reference, audio->m_recordedTake->facts()))
		return QStringLiteral("recorded_source_changed");
	if (notify) resetManagedVocalVariant();
	Clip::setStartTimeOffset(0);
	{
		const auto guard = Engine::audioEngine()->requestChangesGuard();
		m_sample = Sample(audio->m_buffer);
		m_boundedSampleSource = true;
		m_invalidSourceIdentity = false;
		m_boundedSourceDigest = reference.sourceDigest;
		m_boundedSourceIdentity.clear();
		m_sourceInfo = audio->m_info;
		m_unavailableSampleFile.clear();
		m_sourceLoadError.clear();
		m_recordedTakeReference = reference;
		m_recordedTakeToken = std::move(audio->m_recordedTake);
		m_unavailableRecordedTake = {};
	}
	if (notify)
	{
		updateLength();
		emit sampleChanged();
		emit playbackPositionChanged();
		Engine::getSong()->setModified();
	}
	return QStringLiteral("ok");
}

void SampleClip::resetRecordedTakeSource()
{
	m_recordedTakeMetadataPresent = false;
	m_recordedTakeReference.reset();
	m_recordedTakeToken.reset();
	m_unavailableRecordedTake = {};
}

void SampleClip::setSampleFileImpl(const QString& sourceFile, const QByteArray& expectedDigest, const QByteArray& expectedIdentity)
{
	std::unique_ptr<AudioImportPolicy::Prepared> authorized;
	// A caller may pass sampleFile(); own the name before replacing its buffer.
	const auto sf = sourceFile;
	// Remove any prior offset in the clip
	setStartTimeOffset(0);
	if (!sf.isEmpty())
	{
		m_sourceLoadError.clear();
		m_boundedSourceDigest = expectedDigest;
		m_boundedSourceIdentity = expectedIdentity; m_sourceInfo.reset();
		auto buffer = SampleBuffer::emptyBuffer();
		if (m_boundedSampleSource)
		{
			auto prepared = AudioImportPolicy::configured().prepare(PathUtil::toAbsolute(sf), expectedDigest, expectedIdentity);
			if (prepared.audio && prepared.audio->isCurrent())
			{
				m_boundedSourceDigest = prepared.audio->m_info.sourceSha256;
				m_boundedSourceIdentity = prepared.audio->identity(); m_sourceInfo = prepared.audio->m_info;
				buffer = prepared.audio->m_buffer;
				authorized = std::move(prepared.audio);
			}
			else m_sourceLoadError = prepared.audio ? QStringLiteral("plan_stale") : prepared.code;
			if (!m_sourceLoadError.isEmpty())
			{
				Engine::getSong()->collectError(tr("Audio source unavailable: %1").arg(m_sourceLoadError));
			}
		}
		else
		{
			buffer = SampleBuffer::fromFile(sf);
			if (buffer->empty()) { m_sourceLoadError = QStringLiteral("audio_decode_failed"); }
		}
		{
			const auto guard = Engine::audioEngine()->requestChangesGuard();
			m_sample = Sample(std::move(buffer));
			m_unavailableSampleFile = m_sample.buffer()->empty() ? sf : QString{};
		}
		updateLength();
	}
	else
	{
		// If there is no sample, make the clip a bar long
		float nom = Engine::getSong()->getTimeSigModel().getNumerator();
		float den = Engine::getSong()->getTimeSigModel().getDenominator();
		changeLength(DefaultTicksPerBar * (nom / den));
	}

	emit sampleChanged();
	emit playbackPositionChanged();
}




void SampleClip::toggleRecord()
{
	m_recordModel.setValue( !m_recordModel.value() );
	emit dataChanged();
}




void SampleClip::playbackPositionChanged()
{
	if(m_audioWindowStorage.kind==AudioWindowStorageKind::V2 && Engine::getSong()->isAutomaticLoopJump())
	{
		// Already-scheduled views own the prefix before this loop seam. Their
		// exact output span ends at the loop edge; let that prefix render once.
		setIsPlaying(false);
		return;
	}
	Engine::audioEngine()->removePlayHandlesOfTypes( getTrack(), PlayHandle::Type::SamplePlayHandle );
	auto st = dynamic_cast<SampleTrack*>(getTrack());
	st->setPlayingClips( false );
}




void SampleClip::updateTrackClips()
{
	auto sampletrack = dynamic_cast<SampleTrack*>(getTrack());
	if( sampletrack)
	{
		sampletrack->updateClips();
	}
}




bool SampleClip::isPlaying() const
{
	return m_isPlaying;
}




void SampleClip::setIsPlaying(bool isPlaying)
{
	m_isPlaying = isPlaying;
}




void SampleClip::updateLength()
{
	if (hasManagedVocalVariant())
	{
		emit sampleChanged();
		return;
	}
	if (hasAudioSourceWindow()) { emit sampleChanged(); return; }
	// If the clip has already been manually resized, don't automatically resize it.
	if (getAutoResize())
	{
		if (getTrack()->trackContainer() == Engine::patternStore())
		{
			changeLength(TimePos::ticksPerBar() * Engine::patternStore()->lengthOfPattern(getTrack()->getClipNum(this)));
			return;
		}
		changeLength(sampleLength());
		setStartTimeOffset(0);
	}

	emit sampleChanged();
}


void SampleClip::tempoChanged()
{
	if (hasManagedVocalVariant())
	{
		invalidateManagedVocalPlayback();
		emit sampleChanged();
		return;
	}
	if (hasAudioSourceWindow() && !m_recordedTakeMetadataPresent)
	{
		refreshAudioWindowPlayback();
		emit sampleChanged();
		return;
	}
	Clip::setStartTimeOffset(std::round(1.0f * m_startFrameOffset / Engine::framesPerTick()));
	updateLength();
	emit sampleChanged();
}

void SampleClip::setStartTimeOffset(const TimePos& startTimeOffset)
{
	if(!allowsGenericClipEdits()) return;
	m_startFrameOffset = startTimeOffset * Engine::framesPerTick();
	Clip::setStartTimeOffset(startTimeOffset);
}


TimePos SampleClip::sampleLength() const
{
	if (m_managedVocalAssociation)
	{
		return TimePos(int(m_managedVocalAssociation->playback.visualLengthTicks));
	}
	return static_cast<int>(m_sample.sampleSize() / Engine::framesPerTick(m_sample.sampleRate()));
}




void SampleClip::setSampleStartFrame(f_cnt_t startFrame)
{
	m_sample.setStartFrame(startFrame);
}




void SampleClip::setSamplePlayLength(f_cnt_t length)
{
	m_sample.setEndFrame(length);
}




void SampleClip::saveSettings( QDomDocument & _doc, QDomElement & _this )
{
	if( _this.parentNode().nodeName() == "clipboard" )
	{
		_this.setAttribute( "pos", -1 );
	}
	else
	{
		_this.setAttribute( "pos", startPosition() );
	}
	_this.setAttribute( "len", length() );
	_this.setAttribute( "muted", isMuted() );
	_this.setAttribute( "src", m_recordedTakeMetadataPresent ? QString{} : sampleFile() );
	_this.setAttribute( "off", startTimeOffset() );
	_this.setAttribute("autoresize", QString::number(getAutoResize()));
	_this.removeAttribute("bounded_source");
	_this.removeAttribute("source_sha256");
	_this.removeAttribute("source_file_id");
	if (m_recordedTakeMetadataPresent)
	{
		_this.removeAttribute("data");
		for (auto child = _this.firstChild(); !child.isNull();)
		{
			const auto next = child.nextSibling();
			if (child.isElement() && child.nodeName() == QStringLiteral("recorded_take")) _this.removeChild(child);
			child = next;
		}
		if (m_recordedTakeReference)
		{
			const auto reference = recording::RecordedTakeSource::writeReference(_doc, *m_recordedTakeReference);
			if (!reference.isNull()) _this.appendChild(reference);
			else if (!m_unavailableRecordedTake.isNull()) _this.appendChild(_doc.importNode(m_unavailableRecordedTake, true));
		}
		else if (!m_unavailableRecordedTake.isNull()) _this.appendChild(_doc.importNode(m_unavailableRecordedTake, true));
	}
	else if (m_boundedSampleSource)
	{
		_this.setAttribute("bounded_source", "1");
		_this.removeAttribute("data");
		if (!m_boundedSourceIdentity.isEmpty()) _this.setAttribute("source_file_id",QString::fromLatin1(m_boundedSourceIdentity.toHex()));
		else if (m_invalidSourceIdentity) _this.setAttribute("source_file_id", "");
		if (!m_boundedSourceDigest.isEmpty())
		{
			_this.setAttribute("source_sha256", QString::fromLatin1(m_boundedSourceDigest.toHex()));
		}
		else if (m_sourceLoadError == QStringLiteral("unsupported_audio_operation"))
		{
			// Keep malformed persisted admission quarantined across another save/reload.
			_this.setAttribute("source_sha256", "");
		}
	}
	if (!m_recordedTakeMetadataPresent && !m_boundedSampleSource && m_sample.sampleFile().isEmpty())
	{
		QString s;
		_this.setAttribute("data", m_sample.toBase64());
	}

	_this.setAttribute( "sample_rate", m_sample.sampleRate());
	if (const auto& c = color())
	{
		_this.setAttribute("color", c->name());
	}
	if (m_sample.reversed())
	{
		_this.setAttribute("reversed", "true");
	}
	// TODO: start- and end-frame
	for (auto child = _this.firstChild(); !child.isNull();)
	{
		const auto next = child.nextSibling();
		if (child.isElement() && child.nodeName() == QStringLiteral("managed_vocal_variant"))
		{
			_this.removeChild(child);
		}
		child = next;
	}
	if (m_managedVocalAssociation)
	{
		const auto managed = writeManagedVocalVariantAssociation(_doc, *m_managedVocalAssociation);
		if (!managed.isNull()) _this.appendChild(managed);
		else if (!m_unavailableManagedVocalVariants.isNull())
		{
			const auto wrapper = m_unavailableManagedVocalVariants.documentElement();
			for (auto child = wrapper.firstChild(); !child.isNull(); child = child.nextSibling())
			{
				_this.appendChild(_doc.importNode(child, true));
			}
		}
	}
	else if (!m_unavailableManagedVocalVariants.isNull())
	{
		const auto wrapper = m_unavailableManagedVocalVariants.documentElement();
		for (auto child = wrapper.firstChild(); !child.isNull(); child = child.nextSibling())
		{
			_this.appendChild(_doc.importNode(child, true));
		}
	}
	writeAudioWindow(m_audioWindowStorage, _doc, _this);
}




void SampleClip::loadSettings( const QDomElement & _this )
{
	struct LoadingReset
	{
		std::atomic_bool& flag;
		bool previous;
		~LoadingReset(){flag.store(previous,std::memory_order_release);}
	};
	LoadingReset loading{m_loadingAudioWindow,m_loadingAudioWindow.exchange(true,std::memory_order_acq_rel)};
	LoadingReset managedLoading{m_loadingManagedVocalAssociation,
		m_loadingManagedVocalAssociation.exchange(true,std::memory_order_acq_rel)};
	m_managedVocalPlaybackLoadTimer.stop();
	m_managedVocalPlaybackLoadTimer.setInterval(20);
	m_managedVocalPlaybackLoadTicket.reset();
	m_managedVocalPlaybackLoadingProjectId.clear();
	m_managedVocalPreloadedReference = {};
	m_managedVocalPreloadedProjectId.clear();
	m_managedVocalPreloadedSamples.reset();
	{
		const auto guard = Engine::audioEngine()->requestChangesGuard();
		m_audioWindowGuard->epoch.fetch_add(1, std::memory_order_acq_rel);
		m_managedVocalPlaybackGuard->epoch.fetch_add(1, std::memory_order_acq_rel);
		storePublishedManagedVocalView(m_managedVocalPlaybackView, {});
		m_managedVocalAssociation.reset();
		m_unavailableManagedVocalVariants = QDomDocument{};
		m_managedVocalVariantStatus = QStringLiteral("ok");
		if(hasAudioSourceWindow() || !_this.firstChildElement("audio_source_window").isNull())
		{
			Engine::audioEngine()->removePlayHandlesOfTypes(getTrack(),PlayHandle::Type::SamplePlayHandle);
			setIsPlaying(false);
		}
		storePublishedAudioWindowView(m_audioWindowView, {});
		m_sample.m_audioWindowGuard.reset();
		m_audioWindowStorage = parseAudioWindow(_this);
	}
	QList<QDomElement> managedElements;
	for (auto child = _this.firstChild(); !child.isNull(); child = child.nextSibling())
	{
		if (child.isElement() && child.nodeName() == QStringLiteral("managed_vocal_variant"))
		{
			managedElements.push_back(child.toElement());
		}
	}
	if (!managedElements.isEmpty())
	{
		if (managedElements.size() == 1)
		{
			const auto parsed = readManagedVocalVariantAssociation(managedElements.front());
			if (parsed.code == QStringLiteral("ok") && parsed.value)
			{
				m_managedVocalAssociation = *parsed.value;
			}
			else
			{
				m_unavailableManagedVocalVariants = QDomDocument{};
				auto wrapper = m_unavailableManagedVocalVariants.createElement(
					QStringLiteral("unavailable_managed_variants"));
				m_unavailableManagedVocalVariants.appendChild(wrapper);
				wrapper.appendChild(m_unavailableManagedVocalVariants.importNode(managedElements.front(), true));
				setManagedVocalVariantStatus(parsed.code);
			}
		}
		else
		{
			m_unavailableManagedVocalVariants = QDomDocument{};
			auto wrapper = m_unavailableManagedVocalVariants.createElement(
				QStringLiteral("unavailable_managed_variants"));
			m_unavailableManagedVocalVariants.appendChild(wrapper);
			for (const auto& element : managedElements)
			{
				wrapper.appendChild(m_unavailableManagedVocalVariants.importNode(element, true));
			}
			setManagedVocalVariantStatus(QStringLiteral("invalid_managed_variant"));
		}
	}
	m_unavailableSampleFile.clear();
	m_sourceLoadError.clear();
	m_recordedTakeMetadataPresent = false;
	m_recordedTakeReference.reset();
	m_recordedTakeToken.reset();
	m_unavailableRecordedTake = {};
	const auto recordedNode = _this.firstChildElement(QStringLiteral("recorded_take"));
	m_recordedTakeMetadataPresent = !recordedNode.isNull();
	if (m_recordedTakeMetadataPresent) m_unavailableRecordedTake = recordedNode.cloneNode(true).toElement();
	m_boundedSampleSource = hasAudioSourceWindow() || _this.hasAttribute("bounded_source") || m_recordedTakeMetadataPresent;
	m_boundedSourceDigest.clear();
	m_boundedSourceIdentity.clear(); m_invalidSourceIdentity = false; m_sourceInfo.reset();
	if (m_boundedSampleSource && _this.hasAttribute("source_file_id"))
	{
		const auto text = _this.attribute("source_file_id");
		m_invalidSourceIdentity = text.size() != 64;
		for (const auto c : text) if (!QStringLiteral("0123456789abcdefABCDEF").contains(c)) m_invalidSourceIdentity = true;
		if (!m_invalidSourceIdentity) m_boundedSourceIdentity = QByteArray::fromHex(text.toLatin1());
	}
	const auto fingerprint = _this.attribute("source_sha256");
	bool invalidFingerprint = false;
	if (m_boundedSampleSource && _this.hasAttribute("source_sha256"))
	{
		invalidFingerprint = fingerprint.size() != 64;
		if (!invalidFingerprint)
		{
			for (const auto c : fingerprint)
			{
				if (!((c >= QChar('0') && c <= QChar('9')) || (c >= QChar('a') && c <= QChar('f'))
					|| (c >= QChar('A') && c <= QChar('F')))) { invalidFingerprint = true; break; }
			}
		}
		if (!invalidFingerprint) { m_boundedSourceDigest = QByteArray::fromHex(fingerprint.toLatin1()); }
	}
	const auto expectedDigest = m_boundedSourceDigest;
	if( _this.attribute( "pos" ).toInt() >= 0 )
	{
		movePosition( _this.attribute( "pos" ).toInt() );
	}

	if (m_recordedTakeMetadataPresent)
	{
		const auto clearSample = [&]
		{
			const auto guard = Engine::audioEngine()->requestChangesGuard();
			m_sample = Sample{};
			m_unavailableSampleFile.clear();
		};
		clearSample();
		if (hasAudioSourceWindow())
		{
			m_sourceLoadError = QStringLiteral("unsupported_recorded_source");
		}
		else
		{
			QString parseCode;
			auto reference = recording::RecordedTakeSource::readReference(recordedNode, parseCode);
			if (!reference)
			{
				m_sourceLoadError = parseCode;
			}
			else
			{
				m_recordedTakeReference = *reference;
				m_boundedSourceDigest = reference->sourceDigest;
				if (auto* host = Engine::recordingServiceHost(); !host)
				{
					m_sourceLoadError = QStringLiteral("recording_library_required");
				}
				else
				{
					auto opened = host->reopenRecordedTake(Engine::getSong()->recordingProjectIdentity(),
						{reference->projectId, 1}, reference->takeId);
					if (!opened.source)
					{
						m_sourceLoadError = opened.code;
					}
					else if (!recording::RecordedTakeSource::matches(*reference, opened.source->facts()))
					{
						m_sourceLoadError = QStringLiteral("recorded_source_changed");
					}
					else
					{
						auto prepared = AudioImportPolicy::configured().prepareRecordedTake(std::move(*opened.source));
						if (!prepared.audio) m_sourceLoadError = prepared.code;
						else
						{
							const auto adopted = adoptRecordedTakeAudio(std::move(prepared.audio), *reference, false);
						if (adopted != QStringLiteral("ok")) m_sourceLoadError = adopted;
						}
					}
				}
			}
		}
	}
	else if (const auto srcFile = _this.attribute("src"); !srcFile.isEmpty())
	{
		const bool invalidWindowAdmission = hasAudioSourceWindow()
			&& (m_audioWindowStorage.kind == AudioWindowStorageKind::InvalidMetadata
				|| !_this.hasAttribute("source_file_id") || !_this.hasAttribute("source_sha256"));
		if (invalidWindowAdmission || (m_boundedSampleSource && (_this.attribute("bounded_source") != QStringLiteral("1") || invalidFingerprint || m_invalidSourceIdentity)))
		{
			const auto guard = Engine::audioEngine()->requestChangesGuard();
			m_sample = Sample{};
			m_unavailableSampleFile = srcFile;
			m_sourceLoadError = QStringLiteral("unsupported_audio_operation");
		}
		else if (m_boundedSampleSource || QFileInfo(PathUtil::toAbsolute(srcFile)).exists())
		{
			setSampleFileImpl(srcFile, expectedDigest, m_boundedSourceIdentity);
		}
		else
		{
			{
				const auto guard = Engine::audioEngine()->requestChangesGuard();
				m_sample = Sample{};
				m_unavailableSampleFile = srcFile;
				m_sourceLoadError = QStringLiteral("audio_source_missing");
			}
			Engine::getSong()->collectError(QString("%1: %2").arg(tr("Sample not found"), srcFile));
		}
	}
	else if (m_boundedSampleSource)
	{
		const auto guard = Engine::audioEngine()->requestChangesGuard();
		m_sample = Sample{};
		m_sourceLoadError = QStringLiteral("audio_source_missing");
	}

	if (!m_recordedTakeMetadataPresent && !m_boundedSampleSource && m_sample.sampleFile().isEmpty() && _this.hasAttribute("data"))
	{
		auto sampleRate = _this.hasAttribute("sample_rate") ? _this.attribute("sample_rate").toInt() :
			Engine::audioEngine()->outputSampleRate();

		auto buffer = SampleBuffer::fromBase64(_this.attribute("data"), sampleRate);
		m_sample = Sample(std::move(buffer));
	}
	changeLength( _this.attribute( "len" ).toInt() );
	setMuted( _this.attribute( "muted" ).toInt() );
	setStartTimeOffset( _this.attribute( "off" ).toInt() );
	setAutoResize(_this.attribute("autoresize", "1").toInt());

	if (_this.hasAttribute("color"))
	{
		setColor(QColor{_this.attribute("color")});
	}

	if(_this.hasAttribute("reversed"))
	{
		m_sample.setReversed(true);
		emit wasReversed(); // tell SampleClipView to update the view
	}
	if (!m_sourceLoadError.isEmpty() || m_recordedTakeMetadataPresent || hasManagedVocalVariant())
	{
		emit sampleChanged();
		playbackPositionChanged();
	}
	if (hasAudioSourceWindow())
	{
		loading.flag.store(loading.previous, std::memory_order_release);
		m_loadingAudioWindow=false;
		refreshAudioWindowPlayback();
		const auto code = audioWindowStatus();
		if(code!="ok") Engine::getSong()->collectError(tr("Audio range unavailable: %1").arg(code));
		emit audioWindowStatusChanged(code);
	}
	managedLoading.flag.store(managedLoading.previous, std::memory_order_release);
	if (hasManagedVocalVariant() && !managedLoading.previous)
	{
		refreshManagedVocalPlayback();
		if (managedVocalVariantStatus() != QStringLiteral("ok"))
		{
			if (managedVocalVariantStatus() != QStringLiteral("asset_loading")
				&& managedVocalVariantStatus() != QStringLiteral("job_busy"))
			{
				Engine::getSong()->collectError(tr("Managed vocal variant unavailable: %1")
					.arg(managedVocalVariantStatus()));
			}
		}
	}

}




gui::ClipView * SampleClip::createView( gui::TrackView * _tv )
{
	return new gui::SampleClipView( this, _tv );
}


} // namespace lmms
