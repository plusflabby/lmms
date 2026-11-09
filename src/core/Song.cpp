/*
 * Song.cpp - root of the model tree
 *
 * Copyright (c) 2004-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#include "Song.h"
#include <QTextStream>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QMessageBox>
#include <QScopeGuard>
#include <QThread>
#include "SampleClip.h"
#include "MainWindow.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include "AutomationTrack.h"
#include "AutomationEditor.h"
#include "ConfigManager.h"
#include "DataFile.h"
#include "ControllerRackView.h"
#include "ControllerConnection.h"
#include "EnvelopeAndLfoParameters.h"
#include "Mixer.h"
#include "MixerView.h"
#include "GuiApplication.h"
#include "ExportFilter.h"
#include "InstrumentTrack.h"
#include "Keymap.h"
#include "NotePlayHandle.h"
#include "MidiClip.h"
#include "PatternEditor.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "PianoRoll.h"
#include "ProjectJournal.h"
#include "ProjectNotes.h"
#include "ProjectSavePublication.h"
#include "src/core/vocal/ManagedVocalProjectOwnerStore.h"
#include <QUuid>
#include "Scale.h"
#include "SongEditor.h"
#include "PeakController.h"


namespace lmms
{
namespace { thread_local bool automaticLoopJump=false; }
#ifdef Q_OS_WIN
namespace {
bool projectSnapshotEvidence(const QString& fileId, const QByteArray& shaHex, QByteArray& identity, QByteArray& digest)
{
	if (fileId.size() != 26 || fileId[8] != ':' || fileId[17] != ':' || shaHex.size() != 64) return false;
	for (int i = 0; i < fileId.size(); ++i)
	{
		if (i == 8 || i == 17) continue;
		const auto c = fileId[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
	}
	bool volumeOk = false, highOk = false, lowOk = false;
	const auto volume = fileId.mid(0, 8).toUInt(&volumeOk, 16);
	const auto high = fileId.mid(9, 8).toUInt(&highOk, 16);
	const auto low = fileId.mid(18, 8).toUInt(&lowOk, 16);
	digest = QByteArray::fromHex(shaHex);
	if (!volumeOk || !highOk || !lowOk || digest.size() != 32 || digest.toHex() != shaHex) return false;
	const auto material = QByteArray::number(volume) + ':' + QByteArray::number(high) + ':' + QByteArray::number(low);
	identity = QCryptographicHash::hash(material, QCryptographicHash::Sha256);
	return true;
}
QString recoverManagedVocalSave(ManagedVocalProjectOwnerStore& store, const QString& target)
{
	using Recover = QString (ManagedVocalProjectOwnerStore::*)(const QString&) const;
	const Recover operations[] = {&ManagedVocalProjectOwnerStore::recoverPublishedSnapshot,
		&ManagedVocalProjectOwnerStore::recoverUnpublishedSnapshot,
		&ManagedVocalProjectOwnerStore::recoverPublishedSaveAs,
		&ManagedVocalProjectOwnerStore::recoverUnpublishedSaveAs};
	bool incomplete = false;
	for (const auto operation : operations)
	{
		const auto code = (store.*operation)(target);
		if (code == QStringLiteral("ok")) return code;
		if (code != QStringLiteral("asset_missing")) incomplete = true;
	}
	return incomplete ? QStringLiteral("asset_incomplete") : QStringLiteral("asset_missing");
}
}
#endif
bool Song::isAutomaticLoopJump() const { return automaticLoopJump; }
bool Song::adoptManagedVocalOwner(const std::shared_ptr<ManagedVocalSaveAsSource>& source)
{
#ifdef Q_OS_WIN
	if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread()
		|| !source || !source->isAuthorized() || m_fileName.isEmpty()) return false;
	const auto path = QDir::fromNativeSeparators(QFileInfo(m_fileName).absoluteFilePath());
	QByteArray identity, digest;
	if (source->projectPath() != path || !projectSnapshotEvidence(m_projectSourceFileId, m_projectSourceSha256, identity, digest)
		|| source->projectOwner().fileIdentity.toLatin1() != identity.toHex()
		|| source->projectOwner().documentSha256.toLatin1() != digest.toHex()) return false;
	if (m_managedVocalOwner && (m_managedVocalOwner->projectId != source->projectOwner().projectId
		|| m_managedVocalOwner->rootId != source->projectOwner().rootId
		|| m_managedVocalOwner->fileIdentity != source->projectOwner().fileIdentity
		|| m_managedVocalOwner->documentSha256 != source->projectOwner().documentSha256)) return false;
	if (!m_managedVocalOwner) m_managedVocalOwner = std::make_shared<ManagedVocalProjectOwner>(source->projectOwner());
	m_managedVocalHolds.erase(std::remove_if(m_managedVocalHolds.begin(), m_managedVocalHolds.end(),
		[](const auto& hold) { return hold.expired(); }), m_managedVocalHolds.end());
	m_managedVocalHolds.push_back(source);
	return true;
#else
	Q_UNUSED(source); return false;
#endif
}
void Song::warnAudioWindowCompatibility()
{
	if(m_audioWindowCompatibilityWarned || !QCoreApplication::instance()
		|| QThread::currentThread()!=QCoreApplication::instance()->thread()) return;
	bool containsWindow=false;
	for(const auto* track:tracks())for(const auto* clip:track->getClips())
		if(const auto* sample=dynamic_cast<const SampleClip*>(clip);sample && sample->hasAudioSourceWindow()) containsWindow=true;
	if(!containsWindow) return;
	m_audioWindowCompatibilityWarned=true;
	const auto message=tr("This project contains source-frame audio windows and requires a compatible LMMS build. Older builds may play different audio.");
	emit audioWindowCompatibilityWarning(message);
	if(auto* ui=gui::getGUI();ui && ui->mainWindow())
	{
		auto* warning=new QMessageBox(QMessageBox::Warning,tr("Audio window compatibility"),message,QMessageBox::Ok,ui->mainWindow());
		warning->setAttribute(Qt::WA_DeleteOnClose);warning->setWindowModality(Qt::NonModal);warning->setModal(false);warning->show();
	}
	else qWarning().noquote()<<message;
}

tick_t TimePos::s_ticksPerBar = DefaultTicksPerBar;



Song::Song() :
	TrackContainer(),
	m_globalAutomationTrack( dynamic_cast<AutomationTrack *>(
				Track::create( Track::Type::HiddenAutomation,
								this ) ) ),
	m_tempoModel( DefaultTempo, MinTempo, MaxTempo, this, tr( "Tempo" ) ),
	m_timeSigModel( this ),
	m_oldTicksPerBar( DefaultTicksPerBar ),
	m_masterVolumeModel( 100, 0, 200, this, tr( "Master volume" ) ),
	m_masterPitchModel( 0, -12, 12, this, tr( "Master pitch" ) ),
	m_nLoadingTrack( 0 ),
	m_fileName(),
	m_oldFileName(),
	m_projectSourceSha256(),
	m_projectSourceFileId(),
	m_modified( false ),
	m_loadOnLaunch( true ),
	m_recording( false ),
	m_exporting( false ),
	m_exportLoop( false ),
	m_renderBetweenMarkers( false ),
	m_playing( false ),
	m_paused( false ),
	m_savingProject( false ),
	m_loadingProject( false ),
	m_isCancelled( false ),
	m_playMode( PlayMode::None ),
	m_length( 0 ),
	m_midiClipToPlay( nullptr ),
	m_loopMidiClip( false ),
	m_loopRenderCount(1),
	m_loopRenderRemaining(1),
	m_oldAutomatedValues()
{
	connect( &m_tempoModel, SIGNAL(dataChanged()),
			this, SLOT(setTempo()), Qt::DirectConnection );
	connect( &m_tempoModel, SIGNAL(dataUnchanged()),
			this, SLOT(setTempo()), Qt::DirectConnection );
	connect( &m_timeSigModel, SIGNAL(dataChanged()),
			this, SLOT(setTimeSignature()), Qt::DirectConnection );


	connect( Engine::audioEngine(), SIGNAL(sampleRateChanged()), this,
						SLOT(updateFramesPerTick()));

	connect( &m_masterVolumeModel, SIGNAL(dataChanged()),
			this, SLOT(masterVolumeChanged()), Qt::DirectConnection );
/*	connect( &m_masterPitchModel, SIGNAL(dataChanged()),
			this, SLOT(masterPitchChanged()));*/

	qRegisterMetaType<lmms::Note>( "lmms::Note" );
	setType( Type::Song );

	for (auto& scale : m_scales) {scale = std::make_shared<Scale>();}
	for (auto& keymap : m_keymaps) {keymap = std::make_shared<Keymap>();}

	// Aggregate the `positionJumped` signals from all the timelines into a single `playbackPositionJumped` signal for
	// other objects (sample tracks, LFOs, etc) to use.
	for (auto i = std::size_t{0}; i < static_cast<std::size_t>(PlayMode::Count); ++i)
	{
		const auto onPositionJumped = [this, playMode = static_cast<PlayMode>(i)] {
			// Only emit the signal when the song is actually playing and the active timeline jumps
			// This prevents LFOs from changing phase when the user drags the timeline while paused
			if (m_playing && m_playMode == playMode) { emit playbackPositionJumped(); }
		};

		connect(&m_timelines[i], &Timeline::positionJumped, this, onPositionJumped);
	}

	// Inform VST plugins if the user moved the play head
	connect(this, &Song::playbackPositionJumped, this, [this](){ m_vstSyncController.setPlaybackJumped(true); }, Qt::DirectConnection);
}




Song::~Song()
{
	m_playing = false;
	delete m_globalAutomationTrack;
}




void Song::masterVolumeChanged()
{
	Engine::audioEngine()->setMasterGain( m_masterVolumeModel.value() / 100.0f );
}




void Song::setTempo()
{
	Engine::audioEngine()->requestChangeInModel();
	const auto tempo = (bpm_t)m_tempoModel.value();
	PlayHandleList & playHandles = Engine::audioEngine()->playHandles();
	for (const auto& playHandle : playHandles)
	{
		auto nph = dynamic_cast<NotePlayHandle*>(playHandle);
		if( nph && !nph->isReleased() )
		{
			nph->lock();
			nph->resize( tempo );
			nph->unlock();
		}
	}
	Engine::audioEngine()->doneChangeInModel();

	Engine::updateFramesPerTick();

	m_vstSyncController.setTempo( tempo );

	emit tempoChanged( tempo );
}




void Song::setTimeSignature()
{
	TimePos::setTicksPerBar( ticksPerBar() );
	emit timeSignatureChanged( m_oldTicksPerBar, ticksPerBar() );
	emit dataChanged();
	m_oldTicksPerBar = ticksPerBar();

	m_vstSyncController.setTimeSignature(
		getTimeSigModel().getNumerator(), getTimeSigModel().getDenominator() );
}




void Song::savePlayStartPosition()
{
	getTimeline().setPlayStartPosition(getPlayPos());
}




void Song::processNextBuffer()
{
	// If nothing is playing, there is nothing to do
	if (!m_playing) { return; }

	// At the beginning of the song, we have to reset the LFOs
	if (m_playMode == PlayMode::Song && getPlayPos() == 0)
	{
		EnvelopeAndLfoParameters::instances()->reset();
	}

	TrackList trackList;
	int clipNum = -1; // The number of the clip that will be played

	// Determine the list of tracks to play and the clip number
	switch (m_playMode)
	{
		case PlayMode::Song:
			trackList = tracks();
			break;

		case PlayMode::Pattern:
			if (Engine::patternStore()->numOfPatterns() > 0)
			{
				clipNum = Engine::patternStore()->currentPattern();
				trackList.push_back(PatternTrack::findPatternTrack(clipNum));
			}
			break;

		case PlayMode::MidiClip:
			if (m_midiClipToPlay)
			{
				clipNum = m_midiClipToPlay->getTrack()->getClipNum(m_midiClipToPlay);
				trackList.push_back(m_midiClipToPlay->getTrack());
			}
			break;

		default:
			return;
	}

	auto& timeline = getTimeline();

	// If the playback position is outside of the range [begin, end), move it to
	// begin and inform interested parties.
	// Returns true if the playback position was moved, else false.
	const auto enforceLoop = [this](const TimePos& begin, const TimePos& end)
	{
		if (getPlayPos() < begin || getPlayPos() >= end)
		{
			const auto previous=automaticLoopJump;automaticLoopJump=true;
			getTimeline().setTicks(begin.getTicks());
			automaticLoopJump=previous;
			return true;
		}
		return false;
	};

	const auto loopEnabled = !m_exporting && timeline.loopEnabled();

	// Ensure playback begins within the loop if it is enabled
	if (loopEnabled) { enforceLoop(timeline.loopBegin(), timeline.loopEnd()); }

	const auto framesPerTick = Engine::framesPerTick();
	const auto framesPerPeriod = Engine::audioEngine()->framesPerPeriod();

	f_cnt_t frameOffsetInPeriod = 0;

	while (frameOffsetInPeriod < framesPerPeriod)
	{
		auto frameOffsetInTick = timeline.frameOffset();

		// If a whole tick has elapsed, update the frame and tick count, and check any loops
		if (frameOffsetInTick >= framesPerTick)
		{
			// Transfer any whole ticks from the frame count to the tick count
			const auto elapsedTicks = static_cast<int>(frameOffsetInTick / framesPerTick);
			frameOffsetInTick -= elapsedTicks * framesPerTick;
			timeline.incrementTicks(elapsedTicks);
			timeline.setFrameOffset(frameOffsetInTick);

			// If we are playing a pattern track, or a MIDI clip with no loop enabled,
			// loop back to the beginning when we reach the end
			if (m_playMode == PlayMode::Pattern)
			{
				enforceLoop(TimePos{0}, TimePos{Engine::patternStore()->lengthOfCurrentPattern(), 0});
			}
			else if (m_playMode == PlayMode::MidiClip && m_loopMidiClip && !loopEnabled)
			{
				enforceLoop(-m_midiClipToPlay->startTimeOffset(), m_midiClipToPlay->length() - m_midiClipToPlay->startTimeOffset());
			}

			// Handle loop points, and inform VST plugins of the loop status
			if (loopEnabled || (m_loopRenderRemaining > 1 && getPlayPos() >= timeline.loopBegin()))
			{
				m_vstSyncController.startCycle(
					timeline.loopBegin().getTicks(), timeline.loopEnd().getTicks());

				// Loop if necessary, and decrement the remaining loops if we did
				if (enforceLoop(timeline.loopBegin(), timeline.loopEnd())
					&& m_loopRenderRemaining > 1)
				{
					m_loopRenderRemaining--;
				}
			}
			else
			{
				m_vstSyncController.stopCycle();
			}
		}

		const f_cnt_t framesUntilNextPeriod = framesPerPeriod - frameOffsetInPeriod;
		const auto framesUntilNextTick = static_cast<f_cnt_t>(std::ceil(framesPerTick - frameOffsetInTick));

		// We want to proceed to the next buffer or tick, whichever is closer
		const auto framesToPlay = std::min(framesUntilNextPeriod, framesUntilNextTick);

		if (frameOffsetInPeriod == 0)
		{
			// First frame of buffer: update VST sync position.
			// This must be done after we've corrected the frame/tick count,
			// but before actually playing any frames.
			m_vstSyncController.setAbsolutePosition(getPlayPos().getTicks()
				+ timeline.frameOffset() / static_cast<double>(framesPerTick));
			m_vstSyncController.update();
		}

		if (static_cast<f_cnt_t>(frameOffsetInTick) == 0)
		{
			// First frame of tick: process automation and play tracks
			processAutomations(trackList, getPlayPos(), framesToPlay);
			processMetronome(frameOffsetInPeriod);

			for (const auto track : trackList)
			{
				track->play(getPlayPos(), framesToPlay, frameOffsetInPeriod, clipNum);
			}
		}

		// Update frame counters
		frameOffsetInPeriod += framesToPlay;
		frameOffsetInTick += framesToPlay;
		timeline.setFrameOffset(frameOffsetInTick);
	}

	// Reset the jumped state after processing this buffer, since presumably it has now been handled.
	m_vstSyncController.setPlaybackJumped(false);
}


void Song::processAutomations(const TrackList &tracklist, TimePos timeStart, f_cnt_t)
{
	AutomatedValueMap values;

	QSet<const AutomatableModel*> recordedModels;

	TrackContainer* container = this;
	int clipNum = -1;

	switch (m_playMode)
	{
	case PlayMode::Song:
		break;
	case PlayMode::Pattern:
	{
		if (tracklist.empty()) { return; }
		Q_ASSERT(tracklist.at(0)->type() == Track::Type::Pattern);
		auto patternTrack = dynamic_cast<PatternTrack*>(tracklist.at(0));
		container = Engine::patternStore();
		clipNum = patternTrack->patternIndex();
	}
		break;
	default:
		return;
	}

	values = container->automatedValuesAt(timeStart, clipNum);
	const TrackList& tracks = container->tracks();

	Track::clipVector clips;
	for (Track* track : tracks)
	{
		if (track->type() == Track::Type::Automation) {
			track->getClipsInRange(clips, 0, timeStart);
		}
	}

	// Process recording
	for (Clip* clip : clips)
	{
		auto p = dynamic_cast<AutomationClip *>(clip);
		TimePos relTime = timeStart - p->startPosition();
		if (p->isRecording() && relTime >= 0 && relTime < p->length())
		{
			const AutomatableModel* recordedModel = p->firstObject();
			// The automation system really needs to be reworked.
			// For whatever reason, the values in an automation clip are stored in un-un-scaled format, so if you
			// are automating a log knob, when you draw an curve, the values being stored are not the actual values the
			// knob will take, but instead the unscaled version of the unscaled numbers. The tooltip shows the number you expect, but if you double-click,
			// you can see that the true values are stored by their inverse scaled value....which is wrong, since they weren't scaled in the first place...?
			// Anyhow, in the meantime before we redo the automation system, when recording automations, we have to get the inverseScaledValue
			// and store that so that when playing it back, it scales the value correctly.
			p->recordValue(relTime, recordedModel->inverseScaledValue(recordedModel->value<float>()));

			recordedModels << recordedModel;
		}
	}

	// Checks if an automated model stopped being automated by automation clip
	// so we can move the control back to any connected controller again
	for (auto it = m_oldAutomatedValues.begin(); it != m_oldAutomatedValues.end(); it++)
	{
		AutomatableModel * am = it.key();
		if (!values.contains(am))
		{
			am->setUseControllerValue(true);
		}
	}
	m_oldAutomatedValues = values;

	// Apply values
	for (auto it = values.begin(); it != values.end(); it++)
	{
		AutomatableModel* model = it.key();
		bool isRecording = recordedModels.contains(model);
		model->setUseControllerValue(isRecording);

		if (!isRecording)
		{
			/* TODO
			 * Remove scaleValue() from here when automation editor's
			 * Y axis can be set to logarithmic, and automation clips store
			 * the actual values, and not the invertedScaledValue.
			 */
			model->setValue(model->scaledValue(it.value()), true);
		}
	}
}

void Song::processMetronome(size_t bufferOffset)
{
	const auto currentPlayMode = playMode();
	const auto supported = currentPlayMode == PlayMode::MidiClip
		|| currentPlayMode == PlayMode::Song
		|| currentPlayMode == PlayMode::Pattern;

	if (!supported || m_exporting) { return; } 
	m_metronome.processTick(currentTick(), ticksPerBar(), m_timeSigModel.getNumerator(), bufferOffset);
}

void Song::setModified(bool value)
{
	if( !m_loadingProject && m_modified != value)
	{
		m_modified = value;
		emit modified();
	}
}

bool Song::isExportDone() const
{
	return !isExporting() || getPlayPos() >= m_exportSongEnd;
}

int Song::getExportProgress() const
{
	TimePos pos = getPlayPos();
    
	if (pos >= m_exportSongEnd)
	{
		return 100;
	}
	else if (pos <= m_exportSongBegin)
	{
		return 0;
	}
	else if (pos >= m_exportLoopEnd)
	{
		pos = (m_exportLoopBegin-m_exportSongBegin) + (m_exportLoopEnd - m_exportLoopBegin) *
			m_loopRenderCount + (pos - m_exportLoopEnd);
	}
	else if ( pos >= m_exportLoopBegin )
	{
		pos = (m_exportLoopBegin-m_exportSongBegin) + ((m_exportLoopEnd - m_exportLoopBegin) *
			(m_loopRenderCount - m_loopRenderRemaining)) + (pos - m_exportLoopBegin);
	}
	else
	{
		pos = (pos - m_exportSongBegin);
	}

	return (float)pos/(float)m_exportEffectiveLength*100.0f;
}

void Song::playSong()
{
	m_recording = false;

	if( isStopped() == false )
	{
		stop();
	}

	m_playMode = PlayMode::Song;
	m_playing = true;
	m_paused = false;

	m_vstSyncController.setPlaybackState( true );

	savePlayStartPosition();

	emit playbackStateChanged();
}




void Song::record()
{
	m_recording = true;
	// TODO: Implement
}




void Song::playAndRecord()
{
	playSong();
	m_recording = true;
}




void Song::playPattern()
{
	if( isStopped() == false )
	{
		stop();
	}

	m_playMode = PlayMode::Pattern;
	m_playing = true;
	m_paused = false;

	m_vstSyncController.setPlaybackState( true );

	savePlayStartPosition();

	emit playbackStateChanged();
}




void Song::playMidiClip( const MidiClip* midiClipToPlay, bool loop )
{
	if( isStopped() == false )
	{
		stop();
	}

	m_midiClipToPlay = midiClipToPlay;
	m_loopMidiClip = loop;

	if( m_midiClipToPlay != nullptr )
	{
		m_playMode = PlayMode::MidiClip;
		m_playing = true;
		m_paused = false;
	}

	savePlayStartPosition();

	emit playbackStateChanged();
}




void Song::updateLength()
{
	if (m_loadingProject) { return; }

	m_length = 0;
	m_tracksMutex.lockForRead();
	for (auto track : tracks())
	{
		if (m_exporting && track->isMuted())
		{
			continue;
		}

		const bar_t cur = track->length();
		if( cur > m_length )
		{
			m_length = cur;
		}
	}
	m_tracksMutex.unlock();

	emit lengthChanged( m_length );
}




void Song::togglePause()
{
	// Pause/unpause only works when something is actually playing
	if (m_playMode == PlayMode::None)
	{
		return;
	}

	if( m_paused == true )
	{
		m_playing = true;
		m_paused = false;
	}
	else
	{
		m_playing = false;
		m_paused = true;
		Engine::audioEngine()->clear();
	}

	m_vstSyncController.setPlaybackState( m_playing );

	emit playbackStateChanged();
}




void Song::stop()
{
	// do not stop/reset things again if we're stopped already
	if( m_playMode == PlayMode::None )
	{
		return;
	}

	using gui::TimeLineWidget;

	// To avoid race conditions with the processing threads
	Engine::audioEngine()->requestChangeInModel();

	auto& timeline = getTimeline();
	m_paused = false;
	m_recording = true;
	m_playing = false;

	switch (timeline.stopBehaviour())
	{
		case Timeline::StopBehaviour::BackToZero:
			if (m_playMode == PlayMode::MidiClip)
			{
				timeline.setTicks(std::max(0, -m_midiClipToPlay->startTimeOffset()));
			}
			else
			{
				timeline.setTicks(0);
			}
			break;

		case Timeline::StopBehaviour::BackToStart:
			if (timeline.playStartPosition() >= 0)
			{
				timeline.setTicks(timeline.playStartPosition().getTicks());

				timeline.setPlayStartPosition(-1);
			}
			break;

		case Timeline::StopBehaviour::KeepPosition:
			break;
	}

	getTimeline(PlayMode::None).setTicks(getPlayPos().getTicks());

	m_vstSyncController.setPlaybackState( m_exporting );
	m_vstSyncController.setAbsolutePosition(
		getPlayPos().getTicks()
		+ timeline.frameOffset()
		/ (double) Engine::framesPerTick() );

	// remove all note-play-handles that are active
	Engine::audioEngine()->clear();

	// Moves the control of the models that were processed on the last frame
	// back to their controllers.
	for (auto it = m_oldAutomatedValues.begin(); it != m_oldAutomatedValues.end(); it++)
	{
		AutomatableModel * am = it.key();
		am->setUseControllerValue(true);
	}
	m_oldAutomatedValues.clear();

	m_playMode = PlayMode::None;

	Engine::audioEngine()->doneChangeInModel();

	emit stopped();
	emit playbackStateChanged();
}




void Song::startExport()
{
	stop();

	m_exporting = true;
	updateLength();

	auto& timeline = getTimeline(PlayMode::Song);

	if (m_renderBetweenMarkers)
	{
		m_exportSongBegin = m_exportLoopBegin = timeline.loopBegin();
		m_exportSongEnd = m_exportLoopEnd = timeline.loopEnd();

		timeline.setTicks(timeline.loopBegin().getTicks());
	}
	else
	{
		m_exportSongEnd = TimePos(m_length, 0);
        
		// Handle potentially ridiculous loop points gracefully.
		if (m_loopRenderCount > 1 && timeline.loopEnd() > m_exportSongEnd) 
		{
			m_exportSongEnd = timeline.loopEnd();
		}

		if (!m_exportLoop) 
			m_exportSongEnd += TimePos(1,0);
        
		m_exportSongBegin = TimePos(0,0);
		m_exportLoopBegin = timeline.loopBegin() < m_exportSongEnd && timeline.loopEnd() <= m_exportSongEnd
			? timeline.loopBegin()
			: TimePos{0};
		m_exportLoopEnd = timeline.loopBegin() < m_exportSongEnd && timeline.loopEnd() <= m_exportSongEnd
			? timeline.loopEnd()
			: TimePos{0};

		getTimeline(PlayMode::Song).setTicks(0);
	}

	m_exportEffectiveLength = (m_exportLoopBegin - m_exportSongBegin) + (m_exportLoopEnd - m_exportLoopBegin) 
		* m_loopRenderCount + (m_exportSongEnd - m_exportLoopEnd);
	m_loopRenderRemaining = m_loopRenderCount;

	playSong();

	m_vstSyncController.setPlaybackState( true );
}




void Song::stopExport()
{
	stop();
	m_exporting = false;

	m_vstSyncController.setPlaybackState( m_playing );
}




void Song::insertBar()
{
	m_tracksMutex.lockForRead();
	for (Track* track: tracks())
	{
		// FIXME journal batch of tracks instead of each track individually
		if (track->numOfClips() > 0) { track->addJournalCheckPoint(); }
		track->insertBar(getPlayPos(PlayMode::Song));
	}
	m_tracksMutex.unlock();
}




void Song::removeBar()
{
	m_tracksMutex.lockForRead();
	for (Track* track: tracks())
	{
		// FIXME journal batch of tracks instead of each track individually
		if (track->numOfClips() > 0) { track->addJournalCheckPoint(); }
		track->removeBar(getPlayPos(PlayMode::Song));
	}
	m_tracksMutex.unlock();
}




void Song::addPatternTrack()
{
	Track * t = Track::create(Track::Type::Pattern, this);
	Engine::patternStore()->setCurrentPattern(dynamic_cast<PatternTrack*>(t)->patternIndex());
}




void Song::addSampleTrack()
{
	( void )Track::create( Track::Type::Sample, this );
}




void Song::addAutomationTrack()
{
	( void )Track::create( Track::Type::Automation, this );
}




bpm_t Song::getTempo()
{
	return ( bpm_t )m_tempoModel.value();
}


AutomatedValueMap Song::automatedValuesAt(TimePos time, int clipNum) const
{
	auto trackList = TrackList{m_globalAutomationTrack};
	trackList.insert(trackList.end(), tracks().begin(), tracks().end());
	return TrackContainer::automatedValuesFromTracks(trackList, time, clipNum);
}




void Song::clearProject()
{
	m_audioWindowCompatibilityWarned=false;
	using gui::getGUI;
	emit projectAboutToChange();
	// Session owners consume the old copied project identity while revoking their
	// work. Retire it only after every direct invalidation observer has returned.
	m_recordingProjectIdentity.resetForNewProject();
	m_managedVocalOwner.reset();
	m_pendingManagedSaveAsOwner.reset();
	m_pendingManagedSaveAsPath.clear();
	m_managedVocalHolds.clear();
	m_projectHasRecordingMetadata = false;

	Engine::projectJournal()->setJournalling( false );

	if( m_playing )
	{
		stop();
	}

	for (auto i = std::size_t{0}; i < PlayModeCount; i++)
	{
		setPlayPos( 0, ( PlayMode )i );
	}


	Engine::audioEngine()->requestChangeInModel();

	if( getGUI() != nullptr && getGUI()->patternEditor() )
	{
		getGUI()->patternEditor()->m_editor->clearAllTracks();
	}
	if( getGUI() != nullptr && getGUI()->songEditor() )
	{
		getGUI()->songEditor()->m_editor->clearAllTracks();
	}
	if( getGUI() != nullptr && getGUI()->mixerView() )
	{
		getGUI()->mixerView()->clear();
	}
	QCoreApplication::sendPostedEvents();
	Engine::patternStore()->clearAllTracks();
	clearAllTracks();

	Engine::mixer()->clear();

	if( getGUI() != nullptr && getGUI()->automationEditor() )
	{
		getGUI()->automationEditor()->setCurrentClip( nullptr );
	}

	if( getGUI() != nullptr && getGUI()->pianoRoll() )
	{
		getGUI()->pianoRoll()->reset();
	}

	m_tempoModel.reset();
	m_masterVolumeModel.reset();
	m_masterPitchModel.reset();
	m_timeSigModel.reset();

	// Clear the m_oldAutomatedValues AutomatedValueMap
	m_oldAutomatedValues.clear();

	AutomationClip::globalAutomationClip( &m_tempoModel )->clear();
	AutomationClip::globalAutomationClip( &m_masterVolumeModel )->
									clear();
	AutomationClip::globalAutomationClip( &m_masterPitchModel )->
									clear();

	Engine::audioEngine()->doneChangeInModel();

	if( getGUI() != nullptr && getGUI()->getProjectNotes() )
	{
		getGUI()->getProjectNotes()->clear();
	}

	removeAllControllers();

	emit dataChanged();

	Engine::projectJournal()->clearJournal();

	Engine::projectJournal()->setJournalling( true );
}




// create new file
void Song::createNewProject()
{

	QString defaultTemplate = ConfigManager::inst()->userTemplateDir()
						+ "default.mpt";


	if( QFile::exists( defaultTemplate ) )
	{
		createNewProjectFromTemplate( defaultTemplate );
		return;
	}

	defaultTemplate = ConfigManager::inst()->factoryProjectsDir()
						+ "templates/default.mpt";
	if( QFile::exists( defaultTemplate ) )
	{
		createNewProjectFromTemplate( defaultTemplate );
		return;
	}

	m_loadingProject = true;

	clearProject();

	Engine::projectJournal()->setJournalling( false );

	m_oldFileName = "";
	setProjectFileName("");
	m_projectSourceSha256.clear();
	m_projectSourceFileId.clear();

	auto tripleOscTrack = Track::create(Track::Type::Instrument, this);
	dynamic_cast<InstrumentTrack*>(tripleOscTrack)->loadInstrument("tripleoscillator");

	auto kickerTrack = Track::create(Track::Type::Instrument, Engine::patternStore());
	dynamic_cast<InstrumentTrack*>(kickerTrack)->loadInstrument("kicker");

	Track::create( Track::Type::Sample, this );
	Track::create( Track::Type::Pattern, this );
	Track::create( Track::Type::Automation, this );

	m_tempoModel.setInitValue( DefaultTempo );
	m_timeSigModel.reset();
	m_masterVolumeModel.setInitValue( 100 );
	m_masterPitchModel.setInitValue( 0 );

	QCoreApplication::instance()->processEvents();

	m_loadingProject = false;
	updateLength();
	Engine::patternStore()->updateAfterTrackAdd();

	Engine::projectJournal()->setJournalling( true );

	QCoreApplication::sendPostedEvents();

	setModified(false);
	m_loadOnLaunch = false;
}




void Song::createNewProjectFromTemplate( const QString & templ )
{
	loadProject( templ );
	// clear file-name so that user doesn't overwrite template when
	// saving...
	m_oldFileName = "";
	setProjectFileName("");
	// update window title
	m_loadOnLaunch = false;
}




// load given song
void Song::loadProject( const QString & fileName )
{
	using gui::getGUI;

#ifdef Q_OS_WIN
	// Resolve a coordinated save before changing the current project. In the
	// pathname gap the file cannot be opened until its original is restored.
	const auto recoveryTarget = QDir::fromNativeSeparators(QFileInfo(fileName).absoluteFilePath());
	bool abortedNewSave = false;
	if (recoveryTarget.size() >= 3 && recoveryTarget[1] == ':' && recoveryTarget[2] == '/'
		&& ProjectSavePublication::nativeJournalStatus(recoveryTarget) == QStringLiteral("pending"))
	{
		const auto recovered = ProjectSavePublication::resolveNativeJournal(recoveryTarget);
		abortedNewSave = recovered == QStringLiteral("aborted");
		if (recovered != QStringLiteral("restored") && recovered != QStringLiteral("committed") && !abortedNewSave)
		{
			const auto message = tr("The project has an incomplete save that requires recovery. The current project has not been changed.");
			if (getGUI() != nullptr) QMessageBox::warning(nullptr, tr("Project recovery required"), message);
			else qWarning().noquote() << message;
			return;
		}
	}
	// A managed-owner receipt can remain after the native publication has been
	// recovered. Reconcile it before replacing the current in-memory project.
	{
		auto approval = ManagedVocalAssetRoot::authorizeConfiguredRoot(m_recordingProjectIdentity.managedRootLibraryForOwner());
		if (approval.root)
		{
			ManagedVocalProjectOwnerStore store(approval.root);
			const auto recovered = recoverManagedVocalSave(store, recoveryTarget);
			if (recovered != QStringLiteral("ok") && recovered != QStringLiteral("asset_missing"))
			{
				const auto message = tr("The project has an incomplete managed vocal save that requires recovery. The current project has not been changed.");
				if (getGUI() != nullptr) QMessageBox::warning(nullptr, tr("Project recovery required"), message);
				else qWarning().noquote() << message;
				return;
			}
		}
	}
	// An aborted new destination has no document to load. Its managed journal
	// must be reconciled first, while the current in-memory project stays intact.
	if (abortedNewSave) return;
#endif

	QDomNode node;
	const bool journallingBeforeLoad = Engine::projectJournal()->isJournalling();

	m_loadingProject = true;

	Engine::projectJournal()->setJournalling( false );

	m_oldFileName = m_fileName;
	setProjectFileName(fileName);

	DataFile dataFile( m_fileName );
	if (!recording::RecordingProjectIdentity::metadataSupported(dataFile.head()))
	{
		m_loadingProject=false;
		Engine::projectJournal()->setJournalling(journallingBeforeLoad);
		setProjectFileName(m_oldFileName);
		return;
	}

	bool cantLoadProject = false;
	// if file could not be opened, head-node is null and we create
	// new project
	if( dataFile.head().isNull() )
	{
		cantLoadProject = true;
	}
	else
	{
		// We check if plugins contain local paths to prevent malicious code being
		// added to project bundles and loaded with "local:" paths
		if (dataFile.hasLocalPlugins())
		{
			cantLoadProject = true;

			if (getGUI() != nullptr)
			{
				QMessageBox::critical(nullptr, tr("Aborting project load"),
					tr("Project file contains local paths to plugins, which could be used to "
						"run malicious code."));
			}
			else
			{
				QTextStream(stderr) << tr("Can't load project: "
					"Project file contains local paths to plugins.")
					<< Qt::endl;
			}
		}
	}

	if (cantLoadProject)
	{
		if( m_loadOnLaunch )
		{
			createNewProject();
		}
		setProjectFileName(m_oldFileName);
		return;
	}

	m_oldFileName = m_fileName;

	clearProject();
	m_recordingProjectIdentity.acceptLoad(dataFile.head(),m_fileName);
	m_projectHasRecordingMetadata = !dataFile.head().firstChildElement(QStringLiteral("recording_project")).isNull();
	// Resolve an already registered independent owner before clips restore their
	// managed associations. The pin retains the loaded snapshot through load.
	std::shared_ptr<ManagedVocalSaveAsSource> loadedManagedHold;
#ifdef Q_OS_WIN
	{
		auto approval = ManagedVocalAssetRoot::authorizeConfiguredRoot(m_recordingProjectIdentity.managedRootLibraryForOwner());
		if (approval.root)
		{
			ManagedVocalProjectOwnerStore store(approval.root);
			const auto owner = store.lookup(m_fileName);
			QByteArray identity, digest;
			if (owner.owner && projectSnapshotEvidence(dataFile.sourceFileId(), dataFile.sourceSha256(), identity, digest)
				&& owner.owner->fileIdentity.toLatin1() == identity.toHex()
				&& owner.owner->documentSha256.toLatin1() == digest.toHex())
			{
				const auto held = store.holdSaveAsSource(m_fileName, *owner.owner);
				if (held.source) { loadedManagedHold = held.source; m_managedVocalOwner = std::make_shared<ManagedVocalProjectOwner>(*owner.owner); }
			}
		}
	}
#endif

	clearErrors();

	Engine::audioEngine()->requestChangeInModel();

	// get the header information from the DOM
	m_tempoModel.loadSettings( dataFile.head(), "bpm" );
	m_timeSigModel.loadSettings( dataFile.head(), "timesig" );
	m_masterVolumeModel.loadSettings( dataFile.head(), "mastervol" );
	m_masterPitchModel.loadSettings( dataFile.head(), "masterpitch" );

	getTimeline(PlayMode::Song).setLoopEnabled(false);

	//Backward compatibility for LMMS <= 0.4.15
	PeakController::initGetControllerBySetting();

	// Load mixer first to be able to set the correct range for mixer channels
	node = dataFile.content().firstChildElement( Engine::mixer()->nodeName() );
	if( !node.isNull() )
	{
		Engine::mixer()->restoreState( node.toElement() );
		if( getGUI() != nullptr )
		{
			// refresh MixerView
			getGUI()->mixerView()->refreshDisplay();
		}
	}

	node = dataFile.content().firstChild();

	QDomNodeList tclist=dataFile.content().elementsByTagName("trackcontainer");
	m_nLoadingTrack=0;
	for( int i=0,n=tclist.count(); i<n; ++i )
	{
		QDomNode nd=tclist.at(i).firstChild();
		while(!nd.isNull())
		{
			if( nd.isElement() && nd.nodeName() == "track" )
			{
				++m_nLoadingTrack;
				if (static_cast<Track::Type>(nd.toElement().attribute("type").toInt()) == Track::Type::Pattern)
				{
					n += nd.toElement().elementsByTagName("patterntrack").at(0)
						.toElement().firstChildElement().childNodes().count();
				}
				nd=nd.nextSibling();
			}
		}
	}

	while( !node.isNull() && !isCancelled() )
	{
		if( node.isElement() )
		{
			if( node.nodeName() == "trackcontainer" )
			{
				( (JournallingObject *)( this ) )->restoreState( node.toElement() );
			}
			else if( node.nodeName() == "controllers" )
			{
				restoreControllerStates( node.toElement() );
			}
			else if (node.nodeName() == "scales")
			{
				restoreScaleStates(node.toElement());
			}
			else if (node.nodeName() == "keymaps")
			{
				restoreKeymapStates(node.toElement());
			}
			else if( getGUI() != nullptr )
			{
				if( node.nodeName() == getGUI()->getControllerRackView()->nodeName() )
				{
					getGUI()->getControllerRackView()->restoreState( node.toElement() );
				}
				else if( node.nodeName() == getGUI()->pianoRoll()->nodeName() )
				{
					getGUI()->pianoRoll()->restoreState( node.toElement() );
				}
				else if( node.nodeName() == getGUI()->automationEditor()->m_editor->nodeName() )
				{
					getGUI()->automationEditor()->m_editor->restoreState( node.toElement() );
				}
				else if( node.nodeName() == getGUI()->getProjectNotes()->nodeName() )
				{
					 getGUI()->getProjectNotes()->SerializingObject::restoreState( node.toElement() );
				}
				else if (node.nodeName() == getTimeline(PlayMode::Song).nodeName())
				{
					getTimeline(PlayMode::Song).restoreState(node.toElement());
				}
			}
		}
		node = node.nextSibling();
	}

	// quirk for fixing projects with broken positions of Clips inside pattern tracks
	Engine::patternStore()->fixIncorrectPositions();

	// Connect controller links to their controllers
	// now that everything is loaded
	ControllerConnection::finalizeConnections();

	// Remove dummy controllers that was added for correct connections
	m_controllers.erase(std::remove_if(m_controllers.begin(), m_controllers.end(),
		[](Controller* c){return c->type() == Controller::ControllerType::Dummy;}),
		m_controllers.end());

	// resolve all IDs so that autoModels are automated
	AutomationClip::resolveAllIDs();


	Engine::audioEngine()->doneChangeInModel();

	ConfigManager::inst()->addRecentlyOpenedProject( fileName );

	Engine::projectJournal()->setJournalling( true );
	m_projectSourceSha256 = dataFile.sourceSha256();
	m_projectSourceFileId = dataFile.sourceFileId();

	emit projectLoaded();

	if( isCancelled() )
	{
		m_isCancelled = false;
		createNewProject();
		return;
	}

	if ( hasErrors())
	{
		if ( getGUI() != nullptr )
		{
			QMessageBox::warning( nullptr, tr("LMMS Error report"), errorSummary(),
							QMessageBox::Ok );
		}
		else
		{
			QTextStream(stderr) << Engine::getSong()->errorSummary() << Qt::endl;
		}
	}

	m_loadingProject = false;
	updateLength();
	setModified(false);
	m_loadOnLaunch = false;
}


// Populate the project document shared by saving and in-memory identity hashing.
void Song::populateProjectDataFile(DataFile & dataFile)
{
	using gui::getGUI;
	m_tempoModel.saveSettings( dataFile, dataFile.head(), "bpm" );
	m_timeSigModel.saveSettings( dataFile, dataFile.head(), "timesig" );
	m_masterVolumeModel.saveSettings( dataFile, dataFile.head(), "mastervol" );
	m_masterPitchModel.saveSettings( dataFile, dataFile.head(), "masterpitch" );

	saveState( dataFile, dataFile.content() );

	Engine::mixer()->saveState( dataFile, dataFile.content() );
	if( getGUI() != nullptr )
	{
		getGUI()->getControllerRackView()->saveState( dataFile, dataFile.content() );
		getGUI()->pianoRoll()->saveState( dataFile, dataFile.content() );
		getGUI()->automationEditor()->m_editor->saveState( dataFile, dataFile.content() );
		getGUI()->getProjectNotes()->SerializingObject::saveState( dataFile, dataFile.content() );
		getTimeline(PlayMode::Song).saveState(dataFile, dataFile.content());
	}

	saveControllerStates( dataFile, dataFile.content() );

	saveScaleStates(dataFile, dataFile.content());
	saveKeymapStates(dataFile, dataFile.content());
}

QByteArray Song::projectDocumentHash()
{
	DataFile dataFile(DataFile::Type::SongProject);
	const bool wasSavingProject = m_savingProject;
	const bool wasDiscardingMidiConnections = m_saveOptions.discardMIDIConnections.value();
	m_savingProject = true;
	m_saveOptions.discardMIDIConnections.setValue(true);
	const auto restoreOptions = qScopeGuard([this, wasSavingProject, wasDiscardingMidiConnections] {
		m_saveOptions.discardMIDIConnections.setValue(wasDiscardingMidiConnections);
		m_savingProject = wasSavingProject;
	});
	populateProjectDataFile(dataFile);
	return QCryptographicHash::hash(dataFile.toByteArray(-1), QCryptographicHash::Sha256).toHex();
}

QByteArray Song::projectObjectStateHash(SerializingObject &object)
{
	const bool wasSavingProject = m_savingProject;
	const bool wasDiscardingMidiConnections = m_saveOptions.discardMIDIConnections.value();
	m_savingProject = true;
	m_saveOptions.discardMIDIConnections.setValue(true);
	const auto restoreOptions = qScopeGuard([this, wasSavingProject, wasDiscardingMidiConnections] {
		m_saveOptions.discardMIDIConnections.setValue(wasDiscardingMidiConnections);
		m_savingProject = wasSavingProject;
	});

	QDomDocument document;
	auto root = document.createElement(QStringLiteral("target"));
	document.appendChild(root);
	object.saveState(document, root);
	return QCryptographicHash::hash(document.toByteArray(-1), QCryptographicHash::Sha256).toHex();
}

bool Song::saveProjectFile(const QString & filename, bool withResources, ProjectSavePurpose purpose)
{
	warnAudioWindowCompatibility();
	m_pendingManagedSaveAsOwner.reset();
	m_pendingManagedSaveAsPath.clear();
	m_managedVocalHolds.erase(std::remove_if(m_managedVocalHolds.begin(), m_managedVocalHolds.end(),
		[](const auto& hold) { return hold.expired(); }), m_managedVocalHolds.end());
	if (m_managedVocalOwner && !m_managedVocalHolds.empty()) { qWarning() << "Managed project is in use"; return false; }
	DataFile dataFile(DataFile::Type::SongProject);
#ifdef Q_OS_WIN
	const auto coordinatedTarget = QDir::fromNativeSeparators(QFileInfo(dataFile.nameWithExtension(filename)).absoluteFilePath());
	const auto sourcePath = m_fileName.isEmpty() ? QString{} : QDir::fromNativeSeparators(QFileInfo(m_fileName).absoluteFilePath());
	const bool sameSource = !sourcePath.isEmpty() && sourcePath.compare(coordinatedTarget, Qt::CaseInsensitive) == 0;
	const auto originalManagedOwner = m_managedVocalOwner;
	if (originalManagedOwner && (withResources || sourcePath.isEmpty())) return false;
	const bool coordinateRecordingSave = !withResources && !m_recordingProjectIdentity.cachedIdentity().id.isEmpty()
		&& coordinatedTarget.size() >= 3 && coordinatedTarget[1] == ':' && coordinatedTarget[2] == '/';
	const bool coordinateManagedSave = originalManagedOwner && !withResources
		&& coordinatedTarget.size() >= 3 && coordinatedTarget[1] == ':' && coordinatedTarget[2] == '/';
	if (originalManagedOwner && !coordinateManagedSave) return false;
	std::shared_ptr<ManagedVocalAssetRoot> managedRoot;
	std::unique_ptr<ManagedVocalProjectOwnerStore> managedStore;
	std::shared_ptr<ManagedVocalSaveAsSource> managedSaveAsSource;
	if (coordinateManagedSave)
	{
		auto approval = ManagedVocalAssetRoot::authorizeConfiguredRoot(m_recordingProjectIdentity.managedRootLibraryForOwner());
		if (!approval.root || approval.root->rootId() != originalManagedOwner->rootId) { qWarning() << "Managed project root unavailable:" << approval.code; return false; }
		managedRoot = std::move(approval.root);
		managedStore = std::make_unique<ManagedVocalProjectOwnerStore>(managedRoot);
	}
	if (coordinateRecordingSave || coordinateManagedSave)
	{
		const auto status = ProjectSavePublication::nativeJournalStatus(coordinatedTarget);
		if (status == QStringLiteral("pending"))
		{
			const auto recovered = ProjectSavePublication::resolveNativeJournal(coordinatedTarget);
			if (recovered != QStringLiteral("restored") && recovered != QStringLiteral("committed")
				&& recovered != QStringLiteral("aborted")) return false;
		}
		else if (status != QStringLiteral("none")) { qWarning() << "Native project journal unavailable:" << status; return false; }
		if (managedStore)
		{
			const auto recovered = recoverManagedVocalSave(*managedStore, coordinatedTarget);
			if (recovered != QStringLiteral("ok") && recovered != QStringLiteral("asset_missing"))
				{ qWarning() << "Managed project recovery required:" << recovered; return false; }
		}
		if (coordinateManagedSave && !sameSource)
		{
			const auto held = managedStore->holdSaveAsSource(sourcePath, *originalManagedOwner);
			if (!held.source) { qWarning() << "Managed Save As source authorization failed:" << held.code; return false; }
			managedSaveAsSource = held.source;
		}
	}
#endif
	// Recovery can restore a temporarily absent target; determine whether this
	// is the current project only after that reconciliation has completed.
	const auto destinationPath = QFileInfo(filename).canonicalFilePath();
	const auto currentPath = QFileInfo(m_fileName).canonicalFilePath();
	const bool recoverySnapshotOfCurrentProject = purpose == ProjectSavePurpose::RecoverySnapshot
		&& !destinationPath.isEmpty() && !currentPath.isEmpty()
		&& destinationPath.compare(currentPath, Qt::CaseInsensitive) == 0;
	const bool updateRecordingIdentity = purpose == ProjectSavePurpose::UserProject || recoverySnapshotOfCurrentProject;
	m_savingProject = true;
	populateProjectDataFile(dataFile);
	m_savingProject = false;

	if (updateRecordingIdentity && !m_recordingProjectIdentity.beginSave(dataFile,filename,withResources)) { qWarning() << "Recording identity save preparation failed"; return false; }
	const auto recordingRevision=m_recordingProjectIdentity.revision();
	QPointer<Song> saveOwner(this);
#ifdef Q_OS_WIN
	if (coordinateRecordingSave || coordinateManagedSave)
	{
		const auto fail = [&]() {
			if (saveOwner && m_recordingProjectIdentity.revision() == recordingRevision && updateRecordingIdentity)
				m_recordingProjectIdentity.finishSave(coordinatedTarget, false);
			return false;
		};
		const auto uncertain = [&]() {
			if (saveOwner && m_recordingProjectIdentity.revision() == recordingRevision && updateRecordingIdentity
				&& !m_recordingProjectIdentity.cachedIdentity().id.isEmpty())
				m_recordingProjectIdentity.suspendUncertainSave(coordinatedTarget);
			return false;
		};
		ProjectSavePublication::Result publication;
		if (sameSource)
		{
			QByteArray expectedIdentity, expectedDigest;
			if (!projectSnapshotEvidence(m_projectSourceFileId, m_projectSourceSha256, expectedIdentity, expectedDigest)) return fail();
			publication = ProjectSavePublication::acquire(coordinatedTarget, expectedIdentity, expectedDigest);
		}
		else
		{
			publication = ProjectSavePublication::acquireSelectedTarget(coordinatedTarget);
			if (!publication.publication) publication = ProjectSavePublication::acquireNewTarget(coordinatedTarget);
		}
		if (!publication.publication) { qWarning() << "Project publication acquisition failed:" << publication.code; return fail(); }
		QByteArray encoded;
		if (!dataFile.serializeProjectForSave(coordinatedTarget, encoded)) { qWarning() << "Project serialization failed"; return fail(); }
		const auto transactionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		if (const auto code = publication.publication->createStage(transactionId, encoded); code != QStringLiteral("ok")) { qWarning() << "Project staging failed:" << code; return fail(); }
		if (coordinateManagedSave)
		{
			const auto code = sameSource
				? managedStore->prepareSaveJournal(*publication.publication, *originalManagedOwner).code
				: managedStore->prepareSaveAsJournal(*managedSaveAsSource, *publication.publication).code;
			if (code != QStringLiteral("ok")) { qWarning() << "Managed project journal preparation failed:" << code; return fail(); }
		}
		if (const auto code = publication.publication->prepareNativeJournal(); code != QStringLiteral("ok")) { qWarning() << "Native project journal preparation failed:" << code; return fail(); }
		if (const auto code = publication.publication->publishNativePending(); code != QStringLiteral("ok"))
			{ qWarning() << "Native project publication failed:" << code; return uncertain(); }
		if (!saveOwner || m_recordingProjectIdentity.revision() != recordingRevision) return false;
		std::optional<ManagedVocalProjectOwner> rebound;
		if (coordinateManagedSave)
		{
			const auto bound = sameSource
				? managedStore->bindPublishedSnapshot(*publication.publication, *originalManagedOwner)
				: managedStore->bindPublishedSaveAs(*managedSaveAsSource, *publication.publication);
			if (!bound.owner) { qWarning() << "Managed project owner publication failed:" << bound.code; return uncertain(); }
			rebound = *bound.owner;
			const auto finished = sameSource
				? managedStore->finishPublishedSnapshot(*publication.publication, *originalManagedOwner)
				: managedStore->finishPublishedSaveAs(*managedSaveAsSource, *publication.publication);
			if (finished != QStringLiteral("ok")) { qWarning() << "Managed project journal finalization failed:" << finished; return uncertain(); }
		}
		// The recording protocol has already recorded its pending nonce and
		// digest. If interrupted after native finalization, loading the new
		// document can reconcile that pending record against its actual bytes.
		if (publication.publication->finalizeNativePublication() != QStringLiteral("ok"))
			return uncertain();
		if (!updateRecordingIdentity)
		{
			if (rebound && sameSource) m_managedVocalOwner = std::make_shared<ManagedVocalProjectOwner>(*rebound);
			return true;
		}
		ProjectSavePublication::Evidence published;
		if (!publication.publication->publishedEvidence(published))
			return uncertain();
		const bool recorded = m_recordingProjectIdentity.finishSave(coordinatedTarget, true, *publication.publication);
		if (recorded && !sourcePath.isEmpty() && sourcePath.compare(coordinatedTarget, Qt::CaseInsensitive) == 0)
		{
			m_projectSourceFileId = published.stageNativeFileId;
			m_projectSourceSha256 = published.stageSha256.toHex();
			if (rebound) m_managedVocalOwner = std::make_shared<ManagedVocalProjectOwner>(*rebound);
		}
		else if (recorded && rebound && purpose == ProjectSavePurpose::UserProject)
		{
			m_pendingManagedSaveAsOwner = std::make_shared<ManagedVocalProjectOwner>(*rebound);
			m_pendingManagedSaveAsPath = coordinatedTarget;
		}
		if (recorded && m_guiSaveInProgress && purpose == ProjectSavePurpose::UserProject)
		{
			m_guiSavedHasRecordingMetadata = !dataFile.head().firstChildElement(QStringLiteral("recording_project")).isNull();
			m_guiSavePublication = std::shared_ptr<ProjectSavePublication>(std::move(publication.publication));
		}
		return recorded;
	}
#endif
	const bool nativeSaved=dataFile.writeFile(filename,withResources);
	if (!saveOwner || m_recordingProjectIdentity.revision()!=recordingRevision) { return false; }
	if (!updateRecordingIdentity) return nativeSaved;
	return m_recordingProjectIdentity.finishSave(dataFile.nameWithExtension(filename),nativeSaved);
}



// Save the current song
bool Song::guiSaveProject()
{
	return guiSaveProjectAs(m_fileName);
}




// Save the current song with the given filename
bool Song::guiSaveProjectAs(const QString & filename)
{
	if (m_guiSaveInProgress) return false;
	m_guiSaveInProgress = true;
	QPointer<Song> guiSaveOwner(this);
	const auto finishGuiSave = qScopeGuard([guiSaveOwner] {
		if (guiSaveOwner) { guiSaveOwner->m_guiSavePublication.reset(); guiSaveOwner->m_guiSaveInProgress = false; }
	});
	DataFile dataFile(DataFile::Type::SongProject);
	QString fileNameWithExtension = dataFile.nameWithExtension(filename);

	bool withResources = m_saveOptions.saveAsProjectBundle.value();

	bool const saveResult = saveProjectFile(fileNameWithExtension, withResources, ProjectSavePurpose::UserProject);
	if (!guiSaveOwner) return false;
	const auto retainedPublication = m_guiSavePublication;
	const auto pendingOwner = m_pendingManagedSaveAsOwner;
	const auto pendingPath = m_pendingManagedSaveAsPath;
	const bool savedHasRecordingMetadata = m_guiSavedHasRecordingMetadata;
	const auto previousPath = m_fileName;
	const auto previousOwner = m_managedVocalOwner;
	const auto previousIdentity = m_recordingProjectIdentity.cachedIdentity().id;
	const auto previousRevision = m_recordingProjectIdentity.revision();
	const auto previousFileId = m_projectSourceFileId;
	const auto previousSha = m_projectSourceSha256;

	// After saving, restore default save options.
	m_saveOptions.setDefaultOptions();

	// If we saved a bundle, we keep the project on the original
	// file and still keep it as modified
	if (saveResult && !withResources)
	{
		setModified(false);
		if (!guiSaveOwner) return false;
		if (retainedPublication && (m_fileName != previousPath || m_managedVocalOwner != previousOwner
			|| m_recordingProjectIdentity.cachedIdentity().id != previousIdentity
			|| m_recordingProjectIdentity.revision() != previousRevision
			|| m_projectSourceFileId != previousFileId || m_projectSourceSha256 != previousSha)) return false;
#ifdef Q_OS_WIN
		if (retainedPublication)
		{
			ProjectSavePublication::Evidence evidence;
			if (!retainedPublication->publishedEvidence(evidence)) return false;
			m_projectSourceFileId = evidence.stageNativeFileId;
			m_projectSourceSha256 = evidence.stageSha256.toHex();
			m_projectHasRecordingMetadata = savedHasRecordingMetadata;
			if (pendingOwner && pendingPath == QDir::fromNativeSeparators(QFileInfo(fileNameWithExtension).absoluteFilePath()))
				m_managedVocalOwner = pendingOwner;
		}
#endif
		const auto expectedOwner = m_managedVocalOwner;
		setProjectFileName(fileNameWithExtension);
		if (!guiSaveOwner) return false;
		if (retainedPublication && (m_fileName != fileNameWithExtension || m_managedVocalOwner != expectedOwner
			|| m_recordingProjectIdentity.cachedIdentity().id != previousIdentity
			|| m_recordingProjectIdentity.revision() != previousRevision)) return false;
#ifdef Q_OS_WIN
		if (retainedPublication)
		{
			ProjectSavePublication::Evidence evidence;
			if (!retainedPublication->publishedEvidence(evidence)
				|| m_projectSourceFileId != evidence.stageNativeFileId
				|| m_projectSourceSha256 != evidence.stageSha256.toHex()) return false;
		}
		else
#endif
		{
			DataFile savedIdentity(fileNameWithExtension);
			m_projectSourceSha256 = savedIdentity.sourceSha256();
			m_projectSourceFileId = savedIdentity.sourceFileId();
			m_projectHasRecordingMetadata = !savedIdentity.head().firstChildElement(QStringLiteral("recording_project")).isNull();
		}
#ifdef Q_OS_WIN
		m_pendingManagedSaveAsOwner.reset();
		m_pendingManagedSaveAsPath.clear();
#endif
	}

	return saveResult;
}




void Song::saveControllerStates( QDomDocument & doc, QDomElement & element )
{
	// save settings of controllers
	QDomElement controllersNode = doc.createElement( "controllers" );
	element.appendChild( controllersNode );
	for (const auto& controller : m_controllers)
	{
		controller->saveState(doc, controllersNode);
	}
}




void Song::restoreControllerStates( const QDomElement & element )
{
	QDomNode node = element.firstChild();
	while( !node.isNull() && !isCancelled() )
	{
		Controller * c = Controller::create( node.toElement(), this );
		if (c) {addController(c);}
		else
		{
			// Fix indices to ensure correct connections
			m_controllers.push_back(Controller::create(Controller::ControllerType::Dummy, this));
		}

		node = node.nextSibling();
	}
}



void Song::removeAllControllers()
{
	while (m_controllers.size() != 0)
	{
		removeController(m_controllers.at(0));
	}

	m_controllers.clear();
}



void Song::saveScaleStates(QDomDocument &doc, QDomElement &element)
{
	QDomElement scalesNode = doc.createElement("scales");
	element.appendChild(scalesNode);

	for (const auto& scale : m_scales)
	{
		scale->saveState(doc, scalesNode);
	}
}


void Song::restoreScaleStates(const QDomElement &element)
{
	QDomNode node = element.firstChild();

	for (auto i = std::size_t{0}; i < MaxScaleCount && !node.isNull() && !isCancelled(); i++)
	{
		m_scales[i]->restoreState(node.toElement());
		node = node.nextSibling();
	}
	emit scaleListChanged(-1);
}


void Song::saveKeymapStates(QDomDocument &doc, QDomElement &element)
{
	QDomElement keymapsNode = doc.createElement("keymaps");
	element.appendChild(keymapsNode);

	for (const auto& keymap : m_keymaps)
	{
		keymap->saveState(doc, keymapsNode);
	}
}


void Song::restoreKeymapStates(const QDomElement &element)
{
	QDomNode node = element.firstChild();

	for (auto i = std::size_t{0}; i < MaxKeymapCount && !node.isNull() && !isCancelled(); i++)
	{
		m_keymaps[i]->restoreState(node.toElement());
		node = node.nextSibling();
	}
	emit keymapListChanged(-1);
}


void Song::exportProjectMidi(QString const & exportFileName) const
{
	// instantiate midi export plugin
	TrackContainer::TrackList const & tracks = this->tracks();
	TrackContainer::TrackList const & patternStoreTracks = Engine::patternStore()->tracks();

	ExportFilter *exf = dynamic_cast<ExportFilter *> (Plugin::instantiate("midiexport", nullptr, nullptr));
	if (exf)
	{
		exf->tryExport(tracks, patternStoreTracks, getTempo(), m_masterPitchModel.value(), exportFileName);
	}
	else
	{
		qDebug() << "failed to load midi export filter!";
	}

}



void Song::updateFramesPerTick()
{
	Engine::updateFramesPerTick();
}




void Song::setModified()
{
	setModified(true);
}

void Song::setProjectFileName(QString const & projectFileName)
{
	if (m_fileName != projectFileName)
	{
		m_fileName = projectFileName;
		if (m_fileName.isEmpty())
		{
			m_projectSourceSha256.clear();
			m_projectSourceFileId.clear();
			m_projectHasRecordingMetadata = false;
		}
		emit projectFileNameChanged();
	}
}




void Song::addController( Controller * controller )
{
	bool containsController = std::find(m_controllers.begin(), m_controllers.end(), controller) != m_controllers.end();
	if (controller && !containsController)
	{
		m_controllers.push_back(controller);
		emit controllerAdded( controller );

		this->setModified();
	}
}




void Song::removeController( Controller * controller )
{
	auto it = std::find(m_controllers.begin(), m_controllers.end(), controller);
	if (it != m_controllers.end())
	{
		m_controllers.erase(it);

		emit controllerRemoved( controller );
		delete controller;

		this->setModified();
	}
}




void Song::clearErrors()
{
	m_errors.clear();
}



void Song::collectError( const QString error )
{
	if (!m_errors.contains(error)) { m_errors[error] = 1; }
	else { m_errors[ error ]++; }
}



bool Song::hasErrors()
{
	return !(m_errors.isEmpty());
}



QString Song::errorSummary()
{
	QString errors;

	auto i = m_errors.constBegin();
	while (i != m_errors.constEnd())
	{
		errors.append( i.key() );
		if( i.value() > 1 )
		{
			errors.append( tr(" (repeated %1 times)").arg( i.value() ) );
		}
		errors.append("\n");
		++i;
	}

	errors.prepend( "\n\n" );
	errors.prepend( tr( "The following errors occurred while loading: " ) );

	return errors;
}

bool Song::isSavingProject() const {
	return m_savingProject;
}


std::shared_ptr<const Scale> Song::getScale(unsigned int index) const
{
	if (index >= MaxScaleCount) {index = 0;}

	return std::atomic_load(&m_scales[index]);
}


std::shared_ptr<const Keymap> Song::getKeymap(unsigned int index) const
{
	if (index >= MaxKeymapCount) {index = 0;}

	return std::atomic_load(&m_keymaps[index]);
}


void Song::setScale(unsigned int index, std::shared_ptr<Scale> newScale)
{
	if (index >= MaxScaleCount) {index = 0;}

	Engine::audioEngine()->requestChangeInModel();
	std::atomic_store(&m_scales[index], newScale);
	emit scaleListChanged(index);
	Engine::audioEngine()->doneChangeInModel();
}


void Song::setKeymap(unsigned int index, std::shared_ptr<Keymap> newMap)
{
	if (index >= MaxKeymapCount) {index = 0;}

	Engine::audioEngine()->requestChangeInModel();
	std::atomic_store(&m_keymaps[index], newMap);
	emit keymapListChanged(index);
	Engine::audioEngine()->doneChangeInModel();
}
} // namespace lmms
