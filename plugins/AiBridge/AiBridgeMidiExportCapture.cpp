#include "AiBridgeMidiExportCapture.h"

#include "AudioEngine.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Note.h"
#include "PatternClip.h"
#include "PatternStore.h"
#include "PatternTrack.h"
#include "Song.h"
#include "TimePos.h"
#include "Track.h"

#include <QApplication>
#include <QSet>
#include <QThread>

namespace lmms
{
namespace
{
MidiExportNoteSource copyNote(const Note& note)
{
	MidiExportNoteSource value{note.pos().getTicks(), note.length().getTicks(), note.key(), note.getVolume(),
		note.type() == Note::Type::Step ? MidiExportNoteType::Step : MidiExportNoteType::Regular};
	value.panning=note.getPanning();value.hasDetuning=note.hasDetuningInfo();
	return value;
}

MidiExportClipSource copyMidiClip(MidiClip* clip, AiBridgeProjectContext& identities)
{
	MidiExportClipSource result;
	result.id=identities.idForClip(clip);
	result.start=clip->startPosition().getTicks();
	result.length=clip->length().getTicks();
	result.offset=clip->startTimeOffset().getTicks();
	result.muted=clip->isMuted();
	for(const auto* note:clip->notes())result.notes.append(copyNote(*note));
	return result;
}

void captureTrackMidiState(InstrumentTrack& track,const Song& song,QVector<MidiExportEvent>& events,bool& unsupportedAutomation);

MidiExportPatternLaneSource copyPatternLane(Track* track,int patternIndex)
{
	MidiExportPatternLaneSource lane;lane.patternIndex=patternIndex;lane.name=track->displayName();
	lane.muted=track->isMuted();
	if(auto* instrument=dynamic_cast<InstrumentTrack*>(track))
	{
		if(instrument->midiPort()->outputChannel()>0)lane.midiChannel=instrument->midiPort()->realOutputChannel();
		if(instrument->midiPort()->outputProgram()>1)lane.program=instrument->midiPort()->outputProgram()-1;
		captureTrackMidiState(*instrument,*Engine::getSong(),lane.initialEvents,lane.unsupportedAutomation);
	}
	if(patternIndex<0||patternIndex>=track->numOfClips())return lane;
	auto* clip=dynamic_cast<MidiClip*>(track->getClip(patternIndex));
	if(!clip)return lane;
	lane.muted=lane.muted||clip->isMuted();
	lane.clipLength=clip->length().getTicks();lane.clipOffset=clip->startTimeOffset().getTicks();
	for(const auto* note:clip->notes())lane.notes.append(copyNote(*note));
	return lane;
}

void captureTrackMidiState(InstrumentTrack& track,const Song& song,QVector<MidiExportEvent>& events,bool& unsupportedAutomation)
{
	if(track.panningModel()->initValue()!=0)unsupportedAutomation=true;
	if(track.pitchModel()->isAutomated()||track.pitchRangeModel()->isAutomated())unsupportedAutomation=true;
	if(track.baseNoteModel()->initValue()!=DefaultKey)unsupportedAutomation=true;
	if(track.useMasterPitchModel()->initValue()&&song.masterPitch()!=0)unsupportedAutomation=true;
	const auto pitch=track.pitchModel()->initValue();const auto pitchRange=track.pitchModel()->range();
	const auto bend=pitchRange>0?static_cast<int>((pitch+pitchRange/2)*MidiMaxPitchBend/pitchRange):0;
	const auto sensitivity=qBound(0,track.pitchRangeModel()->initValue(),127);
	if(pitch!=0||sensitivity!=2)
	{
		events.append({0,MidiExportEventType::ControlChange,0,101,0});
		events.append({0,MidiExportEventType::ControlChange,0,100,0});
		events.append({0,MidiExportEventType::ControlChange,0,6,static_cast<quint8>(sensitivity)});
		events.append({0,MidiExportEventType::ControlChange,0,38,0});
		events.append({0,MidiExportEventType::ControlChange,0,101,127});
		events.append({0,MidiExportEventType::ControlChange,0,100,127});
		if(pitchRange>0)events.append({0,MidiExportEventType::PitchBend,0,static_cast<quint8>(qBound(0,bend,MidiMaxPitchBend)&0x7f),static_cast<quint8>((qBound(0,bend,MidiMaxPitchBend)>>7)&0x7f)});
	}
	if(track.midiCCEnabled())
		for(int controller=0;controller<MidiControllerCount;++controller)
		{
			const auto* model=track.midiCCModel(controller);
			if(model->isAutomated())unsupportedAutomation=true;
			const auto value=qBound(0,qRound(model->initValue()),127);
			if(value>0)events.append({0,MidiExportEventType::ControlChange,0,static_cast<quint8>(controller),static_cast<quint8>(value)});
		}
}
}

MidiExportCaptureResult captureMidiExport(const MidiExportSelection& selection,AiBridgeProjectContext& identities)
{
	if(!QApplication::instance()||QThread::currentThread()!=QApplication::instance()->thread())return {"wrong_thread",{}};
	auto* song=Engine::getSong();
	if(!song)return {"no_song",{}};
	if(song->isPlaying()||song->isRecording())return {"transport_not_stopped",{}};
	auto* audio=Engine::audioEngine();
	if(!audio)return {"capture_unavailable",{}};
	const auto changes=audio->requestChangesGuard();
	if(song->isPlaying()||song->isRecording())return {"transport_not_stopped",{}};

	QSet<Track*> selectedTracks;
	QSet<Clip*> selectedClips;
	if(selection.mode==MidiExportSelectionMode::Tracks)
	{
		for(const auto& id:selection.trackIds){auto* track=identities.resolveTrack(id);if(!track)return {"track_not_found",{}};selectedTracks.insert(track);}
	}
	if(selection.mode==MidiExportSelectionMode::Clips)
	{
		for(const auto& id:selection.clipIds){auto* clip=identities.resolveClip(id);if(!clip)return {"clip_not_found",{}};selectedClips.insert(clip);}
	}

	MidiExportSnapshot snapshot;
	snapshot.projectGeneration=identities.projectGeneration();
	snapshot.songName=song->displayName();
	snapshot.tempo=song->tempoModel().initValue();
	snapshot.numerator=song->getTimeSigModel().numeratorModel().initValue();
	snapshot.denominator=song->getTimeSigModel().denominatorModel().initValue();
	snapshot.songEnd=TimePos(song->length(),0).getTicks();
	snapshot.hasTempoAutomation=song->tempoModel().isAutomated();
	snapshot.hasTimeSignatureAutomation=song->getTimeSigModel().numeratorModel().isAutomated()
		||song->getTimeSigModel().denominatorModel().isAutomated();
	QSet<int> neededPatterns;
	quint64 capturedClips=0,capturedNotes=0;
	int order=0;
	for(auto* track:song->tracks())
	{
		const int trackOrder=order++;
		const bool explicitlySelected=selection.mode==MidiExportSelectionMode::Tracks&&selectedTracks.contains(track);
		if(selection.mode==MidiExportSelectionMode::Tracks&&!explicitlySelected)continue;
		if(selection.mode==MidiExportSelectionMode::Clips)
		{
			bool ownsSelectedClip=false;
			for(auto* clip:track->getClips())if(selectedClips.contains(clip)){ownsSelectedClip=true;break;}
			if(!ownsSelectedClip)continue;
		}
		if(snapshot.tracks.size()>=256)return {"resource_limit",{}};
		MidiExportTrackSource copy;copy.id=identities.idForTrack(track);copy.name=track->displayName();copy.order=trackOrder;
		copy.muted=track->isMuted();
		if(dynamic_cast<InstrumentTrack*>(track))
		{
			auto* instrument=dynamic_cast<InstrumentTrack*>(track);
			copy.type=MidiExportSourceTrackType::Instrument;
			if(instrument->midiPort()->outputChannel()>0)copy.midiChannel=instrument->midiPort()->realOutputChannel();
			if(instrument->midiPort()->outputProgram()>1)copy.program=instrument->midiPort()->outputProgram()-1;
			captureTrackMidiState(*instrument,*song,copy.initialEvents,copy.unsupportedAutomation);
			for(auto* baseClip:track->getClips())
			{
				auto* midi=dynamic_cast<MidiClip*>(baseClip);
				if(!midi)continue;
				if(selection.mode==MidiExportSelectionMode::Clips&&!selectedClips.contains(midi))continue;
				auto copied=copyMidiClip(midi,identities);capturedNotes+=quint64(copied.notes.size());++capturedClips;
				if(capturedClips>4096||capturedNotes>262144)return {"resource_limit",{}};
				copy.clips.append(copied);
			}
		}
		else if(dynamic_cast<PatternTrack*>(track))
		{
			copy.type=MidiExportSourceTrackType::Pattern;
			for(auto* baseClip:track->getClips())
			{
				auto* pattern=dynamic_cast<PatternClip*>(baseClip);
				if(!pattern)continue;
				if(selection.mode==MidiExportSelectionMode::Clips&&!selectedClips.contains(pattern))continue;
				MidiExportClipSource instance;
				instance.id=identities.idForClip(pattern);instance.start=pattern->startPosition().getTicks();
				instance.length=pattern->length().getTicks();instance.offset=pattern->startTimeOffset().getTicks();
				instance.muted=pattern->isMuted();instance.patternIndex=dynamic_cast<PatternTrack*>(track)->patternIndex();
				instance.patternLength=Engine::patternStore()->lengthOfPattern(instance.patternIndex)*TimePos::ticksPerBar();
				neededPatterns.insert(instance.patternIndex);++capturedClips;if(capturedClips>4096)return {"resource_limit",{}};copy.clips.append(instance);
			}
		}
		else copy.type=MidiExportSourceTrackType::Unsupported;
		snapshot.tracks.append(copy);
	}
	if(selection.mode==MidiExportSelectionMode::Clips)
	{
		QSet<QString> found;
		for(const auto& track:snapshot.tracks)for(const auto& clip:track.clips)if(selectedClips.contains(identities.resolveClip(clip.id)))found.insert(clip.id);
		for(const auto& id:selection.clipIds)if(!found.contains(id))return {"unsupported_selection",{}};
	}
	if(!neededPatterns.isEmpty())
	{
		const auto& patternTracks=Engine::patternStore()->tracks();
		for(int patternIndex:neededPatterns)
			for(auto* track:patternTracks)
				if(track->type()==Track::Type::Instrument)
				{
					auto lane=copyPatternLane(track,patternIndex);capturedNotes+=quint64(lane.notes.size());
					if(capturedNotes>262144||snapshot.patternLanes.size()>=4096)return {"resource_limit",{}};
					snapshot.patternLanes.append(lane);
				}
	}
	snapshot.contentHash=hashMidiExportSnapshot(snapshot);
	return {"ok",std::move(snapshot)};
}

} // namespace lmms
