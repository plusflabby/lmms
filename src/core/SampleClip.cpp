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
#include <QFileInfo>
#include <QThread>
#include <QScopeGuard>
#include <QSignalBlocker>
#include "AudioWindowPlayback.h"

#include "PatternStore.h"
#include "PathUtil.h"
#include "SampleClipView.h"
#include "SampleDecoder.h"
#include <QFile>
#include "SampleTrack.h"
#include "Song.h"

namespace lmms
{
namespace
{
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
	updateTrackClips();
}

SampleClip::SampleClip(Track* track)
	: SampleClip(track, Sample(), false)
{
}

SampleClip::SampleClip(const SampleClip& orig) : SampleClip(orig,orig.getTrack()) {}

SampleClip::SampleClip(const SampleClip& orig, Track* target) :
	Clip(orig,target),
	m_sample(std::move(orig.m_sample)),
	m_unavailableSampleFile(orig.m_unavailableSampleFile),
	m_sourceLoadError(orig.m_sourceLoadError),
	m_boundedSourceDigest(orig.m_boundedSourceDigest),
	m_boundedSourceIdentity(orig.m_boundedSourceIdentity),
	m_invalidSourceIdentity(orig.m_invalidSourceIdentity),
	m_sourceInfo(orig.m_sourceInfo),
	m_boundedSampleSource(orig.m_boundedSampleSource),
	m_audioWindowStorage(orig.m_audioWindowStorage),
	m_isPlaying(false),
	m_startFrameOffset(orig.m_startFrameOffset)
{
	if (m_audioWindowStorage.kind == AudioWindowStorageKind::InvalidMetadata)
	{
		m_audioWindowStorage.retainedInvalidXml = orig.m_audioWindowStorage.retainedInvalidXml.cloneNode(true).toDocument();
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
	updateTrackClips();
}




SampleClip::~SampleClip()
{
	m_audioWindowGuard->epoch.fetch_add(1,std::memory_order_acq_rel);
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

QString SampleClip::applyAudioWindow(const AudioSourceWindowV2& window,const AudioWindowGeometry& geometry)
{
	if(QThread::currentThread()!=thread()) return QStringLiteral("wrong_thread");
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

bool SampleClip::hasSampleFileLoaded(const QString & filename) const
{
	return m_sample.sampleFile() == filename;
}

void SampleClip::setSampleBuffer(std::shared_ptr<const SampleBuffer> sb)
{
	if (hasAudioSourceWindow()) return;
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

	Engine::getSong()->setModified();
}

void SampleClip::setSampleFile(const QString& sf)
{
	if (hasAudioSourceWindow()) return;
	// An explicit source replacement establishes a new expected recording.
	if (!sf.isEmpty()) m_invalidSourceIdentity = false;
	setSampleFileImpl(sf, {});
}

QString SampleClip::adoptAuthorizedAudio(std::unique_ptr<AudioImportPolicy::Prepared> audio)
{
	if (hasAudioSourceWindow()) return QStringLiteral("unsupported_audio_operation");
	if (!audio || !audio->m_buffer) return QStringLiteral("unsupported_audio_operation");
	if (!audio->isCurrent()) return QStringLiteral("plan_stale");
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
	if (hasAudioSourceWindow())
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
	_this.setAttribute( "src", sampleFile() );
	_this.setAttribute( "off", startTimeOffset() );
	_this.setAttribute("autoresize", QString::number(getAutoResize()));
	_this.removeAttribute("bounded_source");
	_this.removeAttribute("source_sha256");
	_this.removeAttribute("source_file_id");
	if (m_boundedSampleSource)
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
	if (!m_boundedSampleSource && m_sample.sampleFile().isEmpty())
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
	writeAudioWindow(m_audioWindowStorage, _doc, _this);
}




void SampleClip::loadSettings( const QDomElement & _this )
{
	struct LoadingReset { std::atomic_bool& flag; bool previous; ~LoadingReset(){flag.store(previous,std::memory_order_release);} };
	LoadingReset loading{m_loadingAudioWindow,m_loadingAudioWindow.exchange(true,std::memory_order_acq_rel)};
	{
		const auto guard = Engine::audioEngine()->requestChangesGuard();
		m_audioWindowGuard->epoch.fetch_add(1, std::memory_order_acq_rel);
		if(hasAudioSourceWindow() || !_this.firstChildElement("audio_source_window").isNull())
		{
			Engine::audioEngine()->removePlayHandlesOfTypes(getTrack(),PlayHandle::Type::SamplePlayHandle);
			setIsPlaying(false);
		}
		storePublishedAudioWindowView(m_audioWindowView, {});
		m_sample.m_audioWindowGuard.reset();
		m_audioWindowStorage = parseAudioWindow(_this);
	}
	m_unavailableSampleFile.clear();
	m_sourceLoadError.clear();
	m_boundedSampleSource = hasAudioSourceWindow() || _this.hasAttribute("bounded_source");
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

	if (const auto srcFile = _this.attribute("src"); !srcFile.isEmpty())
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

	if (!m_boundedSampleSource && m_sample.sampleFile().isEmpty() && _this.hasAttribute("data"))
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
	if (!m_sourceLoadError.isEmpty())
	{
		emit sampleChanged();
		playbackPositionChanged();
	}
	if (hasAudioSourceWindow())
	{
		m_loadingAudioWindow=false;
		refreshAudioWindowPlayback();
		const auto code = audioWindowStatus();
		if(code!="ok") Engine::getSong()->collectError(tr("Audio range unavailable: %1").arg(code));
		emit audioWindowStatusChanged(code);
	}

}




gui::ClipView * SampleClip::createView( gui::TrackView * _tv )
{
	return new gui::SampleClipView( this, _tv );
}


} // namespace lmms
