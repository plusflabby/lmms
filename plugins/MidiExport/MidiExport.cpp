/*
 * MidiExport.cpp - support for exporting Standard MIDI files
 *
 * This file is part of LMMS - https://lmms.io
 * Copyright (c) LMMS contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "MidiExport.h"

#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Note.h"
#include "PatternClip.h"
#include "PatternTrack.h"
#include "PatternStore.h"
#include "Song.h"
#include "TimePos.h"
#include "Track.h"
#include "MidiExportService.h"
#include "StandardMidiWriter.h"

#include "plugin_export.h"

#include <QFileInfo>
#include <QSaveFile>
#include <QDebug>
#include <algorithm>

namespace lmms
{

extern "C"
{
Plugin::Descriptor PLUGIN_EXPORT midiexport_plugin_descriptor =
{
	LMMS_STRINGIFY( PLUGIN_NAME ),
	"MIDI Export",
	QT_TRANSLATE_NOOP( "PluginBrowser", "Export MIDI files from LMMS" ),
	"LMMS contributors",
	0x0100,
	Plugin::Type::ExportFilter,
	nullptr,
	nullptr,
	nullptr,
};
}

MidiExport::MidiExport() : ExportFilter(&midiexport_plugin_descriptor)
{
}

bool MidiExport::tryExport(const TrackContainer::TrackList& tracks,
	const TrackContainer::TrackList& patternStoreTracks,int tempo,int masterPitch,const QString& filename)
{
	if(filename.isEmpty())return false;
	MidiExportSnapshot snapshot;
	auto* song=Engine::getSong();
	if(song)
	{
		snapshot.songName=song->displayName();
		snapshot.numerator=song->getTimeSigModel().getNumerator();
		snapshot.denominator=song->getTimeSigModel().getDenominator();
		snapshot.songEnd=TimePos(song->length(),0).getTicks();
		snapshot.hasTempoAutomation=song->tempoModel().isAutomated();
		snapshot.hasTimeSignatureAutomation=song->getTimeSigModel().numeratorModel().isAutomated()
			||song->getTimeSigModel().denominatorModel().isAutomated();
	}
	snapshot.tempo=tempo;
	QSet<int> neededPatterns;
	int order=0;
	for(auto* track:tracks)
	{
		MidiExportTrackSource source;source.id=QString::number(order);source.name=track->name();source.order=order++;
		source.muted=track->isMuted();
		if(auto* instrument=dynamic_cast<InstrumentTrack*>(track))
		{
			source.type=MidiExportSourceTrackType::Instrument;
			if(instrument->midiPort()->outputChannel()>0)source.midiChannel=instrument->midiPort()->realOutputChannel();
			if(instrument->midiPort()->outputProgram()>1)source.program=instrument->midiPort()->outputProgram()-1;
			for(auto* baseClip:track->getClips())
			{
				auto* clip=dynamic_cast<MidiClip*>(baseClip);if(!clip)continue;
				MidiExportClipSource copied;copied.id=QStringLiteral("%1:%2").arg(source.id).arg(clip->startPosition().getTicks());
				copied.start=clip->startPosition().getTicks();copied.length=clip->length().getTicks();copied.offset=clip->startTimeOffset().getTicks();copied.muted=clip->isMuted();
				for(const auto* note:clip->notes())
				{
					int key=note->key();
					key-=instrument->baseNote()-DefaultKey;
					if(instrument->useMasterPitchModel()->value())key+=masterPitch;
					const int volume=qBound(0,qRound(note->getVolume()*instrument->getVolume()/100.0),200);
					MidiExportNoteSource value{note->pos().getTicks(),note->length().getTicks(),qBound(0,key,127),volume,
						note->type()==Note::Type::Step?MidiExportNoteType::Step:MidiExportNoteType::Regular};
					value.panning=note->getPanning();value.hasDetuning=note->hasDetuningInfo();copied.notes.append(value);
				}
				source.clips.append(copied);
			}
		}
		else if(dynamic_cast<PatternTrack*>(track))
		{
			source.type=MidiExportSourceTrackType::Pattern;
			for(auto* baseClip:track->getClips())
			{
				auto* clip=dynamic_cast<PatternClip*>(baseClip);if(!clip)continue;
				MidiExportClipSource copied;copied.id=QStringLiteral("%1:%2").arg(source.id).arg(clip->startPosition().getTicks());
				copied.start=clip->startPosition().getTicks();copied.length=clip->length().getTicks();copied.offset=clip->startTimeOffset().getTicks();
				copied.muted=clip->isMuted();copied.patternIndex=dynamic_cast<PatternTrack*>(track)->patternIndex();
				copied.patternLength=Engine::patternStore()->lengthOfPattern(copied.patternIndex)*TimePos::ticksPerBar();
				neededPatterns.insert(copied.patternIndex);source.clips.append(copied);
			}
		}
		else source.type=MidiExportSourceTrackType::Unsupported;
		snapshot.tracks.append(source);
	}
	for(int patternIndex:neededPatterns)
		for(auto* track:patternStoreTracks)
			if(track->type()==Track::Type::Instrument&&patternIndex<track->numOfClips())
			{
				auto* clip=dynamic_cast<MidiClip*>(track->getClip(patternIndex));if(!clip)continue;
				MidiExportPatternLaneSource lane;lane.patternIndex=patternIndex;lane.name=track->name();
				lane.muted=track->isMuted()||clip->isMuted();
				if(auto* instrument=dynamic_cast<InstrumentTrack*>(track))
				{
					if(instrument->midiPort()->outputChannel()>0)lane.midiChannel=instrument->midiPort()->realOutputChannel();
					if(instrument->midiPort()->outputProgram()>1)lane.program=instrument->midiPort()->outputProgram()-1;
				}
				lane.clipLength=clip->length().getTicks();lane.clipOffset=clip->startTimeOffset().getTicks();
				for(const auto* note:clip->notes())
				{
					int key=note->key();int volume=note->getVolume();
					if(auto* instrument=dynamic_cast<InstrumentTrack*>(track))
					{
						key-=instrument->baseNote()-DefaultKey;
						if(instrument->useMasterPitchModel()->value())key+=masterPitch;
						volume=qBound(0,qRound(volume*instrument->getVolume()/100.0),200);
					}
					MidiExportNoteSource value{note->pos().getTicks(),note->length().getTicks(),qBound(0,key,127),volume,
						note->type()==Note::Type::Step?MidiExportNoteType::Step:MidiExportNoteType::Regular};
					value.panning=note->getPanning();value.hasDetuning=note->hasDetuningInfo();lane.notes.append(value);
				}
				snapshot.patternLanes.append(lane);
			}
	MidiExportSelection selection;selection.fidelity=MidiExportFidelity::BestEffort;
	const auto planned=planMidiExport(snapshot,selection);
	if(!planned.success)
	{
		qWarning()<<"MIDI export planning failed:"<<planned.code;
		return false;
	}
	for(const auto& issue:planned.issues)
		qWarning().noquote()<<"MIDI export warning:"<<issue.code<<issue.message<<"count="<<issue.count;
	QSaveFile output(QFileInfo(filename).absoluteFilePath());
	output.setDirectWriteFallback(false);
	if(!output.open(QIODevice::WriteOnly))return false;
	const auto written=writeStandardMidi(planned.plan,output);
	if(!written.success){output.cancelWriting();return false;}
	return output.commit();
}

extern "C"
{
PLUGIN_EXPORT Plugin* lmms_plugin_main(Model*,void*)
{
	return new MidiExport();
}
}

} // namespace lmms
