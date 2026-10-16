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

#include "PatternStore.h"
#include "PathUtil.h"
#include "SampleClipView.h"
#include "SampleDecoder.h"
#include <QFile>
#include "SampleTrack.h"
#include "Song.h"

namespace lmms
{

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
	connect( Engine::getSong(), SIGNAL(timeSignatureChanged(int,int)),
					this, SLOT(updateLength()));

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

	updateTrackClips();
}




SampleClip::~SampleClip()
{
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
	Clip::changeLength(std::max(static_cast<int>(_length), 1));
}



const QString& SampleClip::sampleFile() const
{
	return m_unavailableSampleFile.isEmpty() ? m_sample.sampleFile() : m_unavailableSampleFile;
}

QString SampleClip::audioWindowStatus() const
{
	if (!hasAudioSourceWindow()) return QStringLiteral("ok");
	if (m_audioWindowStorage.kind == AudioWindowStorageKind::InvalidMetadata) return m_audioWindowStorage.parseCode;
	if (!m_sourceLoadError.isEmpty()) return m_sourceLoadError;
	if (!m_boundedSampleSource || !m_sourceInfo) return QStringLiteral("unsupported_audio_operation");
	const auto& w = *m_audioWindowStorage.value;
	if (m_sourceInfo->frames != quint64(w.sourceFrameCount) || m_sourceInfo->sampleRate != w.sourceRate)
		return QStringLiteral("audio_source_changed");
	// Persistence ships fail-closed until independent renderer/scheduler gates pass.
	return QStringLiteral("unsupported_audio_operation");
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
		m_audioWindowGuard->epoch.fetch_add(1, std::memory_order_acq_rel);
		emit audioWindowStatusChanged(audioWindowStatus());
		emit sampleChanged();
		return;
	}
	Clip::setStartTimeOffset(std::round(1.0f * m_startFrameOffset / Engine::framesPerTick()));
	updateLength();
	emit sampleChanged();
}

void SampleClip::setStartTimeOffset(const TimePos& startTimeOffset)
{
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
	{
		const auto guard = Engine::audioEngine()->requestChangesGuard();
		m_audioWindowGuard->epoch.fetch_add(1, std::memory_order_acq_rel);
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
		const auto code = audioWindowStatus();
		Engine::getSong()->collectError(tr("Audio range unavailable: %1").arg(code));
		emit audioWindowStatusChanged(code);
	}

}




gui::ClipView * SampleClip::createView( gui::TrackView * _tv )
{
	return new gui::SampleClipView( this, _tv );
}


} // namespace lmms
