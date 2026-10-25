#include "MidiExportService.h"

#include <QSet>
#include <QCryptographicHash>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <tuple>

namespace lmms
{
namespace
{
constexpr quint64 MaxPatternPlanningIterations=1000000;

struct PlannerControl
{
	const std::atomic_bool* cancelled=nullptr;
	quint64 patternIterations=0;

	bool checkCancellation(MidiExportPlanResult& result) const
	{
		if(cancelled&&cancelled->load(std::memory_order_acquire)){result.code=QStringLiteral("cancelled");return false;}
		return true;
	}
};

struct PlannedNote
{
	qint64 start = 0;
	qint64 end = 0;
	int key = 0;
	int velocity = 0;
	QString clipId;
};

void addIssue(MidiExportPlanResult& result, const QString& code, bool blocking,
	const QString& trackId = {}, const QString& clipId = {}, qint64 tick = -1)
{
	if(!result.issues.isEmpty())
	{
		auto& last=result.issues.last();
		if(last.code==code&&last.trackId==trackId&&last.clipId==clipId&&last.blocking==blocking){++last.count;if(blocking&&result.code.isEmpty())result.code="fidelity_loss";return;}
	}
	if(result.issues.size()>=4096)
	{
		auto& last=result.issues.last();if(last.code=="issue_list_truncated"){++last.count;return;}
		last={"issue_list_truncated","warning",false,{}, {},-1,1,"Additional export issues were omitted from the bounded report"};
		return;
	}
	QString message=code;
	message.replace(QLatin1Char('_'),QLatin1Char(' '));
	if(!message.isEmpty())message[0]=message[0].toUpper();
	message+=QLatin1Char('.');
	if(code==QStringLiteral("empty_selection"))message=QStringLiteral("No MIDI notes were exported for this selection.");
	else if(code==QStringLiteral("tempo_automation_not_captured"))message=QStringLiteral("Tempo automation is not captured; best-effort export uses the initial tempo.");
	else if(code==QStringLiteral("meter_automation_not_captured"))message=QStringLiteral("Time-signature automation is not captured; best-effort export uses the initial meter.");
	else if(code==QStringLiteral("unsupported_track_automation"))message=QStringLiteral("Automated track MIDI controls are not captured by this export.");
	result.issues.append({code, blocking ? QStringLiteral("error") : QStringLiteral("warning"), blocking,
		trackId, clipId, tick, 1, message});
	if (blocking && result.code.isEmpty()) result.code = QStringLiteral("fidelity_loss");
}

bool includesClip(const MidiExportSelection& selection, const MidiExportClipSource& clip)
{
	return selection.mode != MidiExportSelectionMode::Clips || selection.clipIds.contains(clip.id);
}

bool convertTick(qint64 tick, quint64& result)
{
	if (tick < 0) return false;
	const auto nativeTick=static_cast<quint64>(tick);
	if (nativeTick > std::numeric_limits<quint64>::max() / 10) return false;
	result = nativeTick * 10;
	return result <= 0x0fffffff;
}

quint8 velocityFor(int volume)
{
	return static_cast<quint8>(qBound(1, qRound(volume * 127.0 / 200.0), 127));
}

bool isPowerOfTwo(int value)
{
	return value > 0 && (value & (value - 1)) == 0;
}

void hashInteger(QCryptographicHash& hash,qint64 value)
{
	QByteArray bytes(8,'\0');const quint64 bits=static_cast<quint64>(value);
	for(int i=0;i<8;++i)bytes[i]=char((bits>>(56-i*8))&0xff);
	hash.addData(bytes);
}

void hashString(QCryptographicHash& hash,const QString& value)
{
	const auto bytes=value.toUtf8();hashInteger(hash,bytes.size());hash.addData(bytes);
}

void hashNotes(QCryptographicHash& hash,const QVector<MidiExportNoteSource>& notes)
{
	hashInteger(hash,notes.size());
	for(const auto& note:notes){hashInteger(hash,note.position);hashInteger(hash,note.length);hashInteger(hash,note.key);hashInteger(hash,note.volume);hashInteger(hash,int(note.type));hashInteger(hash,note.panning);hashInteger(hash,note.hasDetuning);}
}

void hashEvents(QCryptographicHash& hash,const QVector<MidiExportEvent>& events)
{
	hashInteger(hash,events.size());
	for(const auto& event:events){hashInteger(hash,qint64(event.tick));hashInteger(hash,int(event.type));hashInteger(hash,event.channel);hashInteger(hash,event.data1);hashInteger(hash,event.data2);hashInteger(hash,event.data3);}
}

quint8 denominatorPower(int value)
{
	quint8 exponent = 0;
	while (value > 1) { value >>= 1; ++exponent; }
	return exponent;
}

void appendNote(QVector<PlannedNote>& notes, const MidiExportNoteSource& note,
	qint64 noteStart, qint64 noteEnd, const QString& clipId, const QString& trackId,
	const MidiExportSelection& selection, MidiExportPlanResult& result)
{
	if(result.code=="resource_limit")return;
	if ((note.length <= 0 && note.type!=MidiExportNoteType::Step) || note.key < 0 || note.key > 127 || note.volume < 0 || note.volume > 200)
	{
		addIssue(result, QStringLiteral("invalid_note"), true, trackId, clipId, noteStart);
		return;
	}
	qint64 length = note.length;
	if (note.type == MidiExportNoteType::Step)
	{
		if (selection.fidelity == MidiExportFidelity::Strict)
		{
			addIssue(result, QStringLiteral("step_note_duration_ambiguous"), true, trackId, clipId, noteStart);
			return;
		}
		length = 1500;
		addIssue(result, QStringLiteral("step_note_duration_approximated"), false, trackId, clipId, noteStart);
	}
	if(note.panning!=0)addIssue(result,"per_note_panning_not_representable",selection.fidelity==MidiExportFidelity::Strict,trackId,clipId,noteStart);
	if(note.hasDetuning)addIssue(result,"per_note_detuning_not_representable",selection.fidelity==MidiExportFidelity::Strict,trackId,clipId,noteStart);
	const qint64 end = noteStart > std::numeric_limits<qint64>::max() - length
		? std::numeric_limits<qint64>::max() : qMin(noteEnd, noteStart + length);
	qint64 start = noteStart;
	qint64 clippedEnd = end;
	if (selection.hasRange)
	{
		start = qMax(start, selection.rangeStart);
		clippedEnd = qMin(clippedEnd, selection.rangeEnd);
	}
	if (start >= clippedEnd) return;
	if (start != noteStart || clippedEnd != end) ++result.statistics.notesClipped;
	if (note.volume == 0) { ++result.statistics.notesSkipped; return; }
	if (start > std::numeric_limits<qint64>::max() - 10 || clippedEnd > std::numeric_limits<qint64>::max() - 10)
	{
		addIssue(result, QStringLiteral("timeline_overflow"), true, trackId, clipId, start);
		return;
	}
	if(notes.size()>=500000){result.code="resource_limit";return;}
	notes.append({start, clippedEnd, note.key, velocityFor(note.volume), clipId});
}

void appendRegularClip(QVector<PlannedNote>& notes, const MidiExportClipSource& clip,
	const MidiExportTrackSource& track, const MidiExportSelection& selection, MidiExportPlanResult& result,
	const PlannerControl& control)
{
	if (!includesClip(selection, clip) || (!selection.includeMuted && clip.muted)) return;
	if (clip.length <= 0) { addIssue(result, QStringLiteral("invalid_clip_length"), true, track.id, clip.id); return; }
	const qint64 firstAllowed = qMax(clip.start, clip.start + clip.offset);
	const qint64 clipEnd = clip.start > std::numeric_limits<qint64>::max() - clip.length
		? std::numeric_limits<qint64>::max() : clip.start + clip.length;
	for (const auto& note : clip.notes)
	{
		if(!control.checkCancellation(result))return;
		if(note.position<0||note.position>8000000||(note.length<=0&&note.type!=MidiExportNoteType::Step)){addIssue(result,"invalid_note",true,track.id,clip.id,note.position);continue;}
		const qint64 duration=note.type==MidiExportNoteType::Step?1500:note.length;
		if (note.position > std::numeric_limits<qint64>::max() - clip.start - clip.offset)
		{
			addIssue(result, QStringLiteral("timeline_overflow"), true, track.id, clip.id);
			continue;
		}
		const auto rawStart = clip.start + clip.offset + note.position;
		const auto rawEnd = rawStart <= std::numeric_limits<qint64>::max() - duration
			? rawStart + duration : rawStart;
		const auto start = qMax(firstAllowed, rawStart);
		const auto end = qMin(clipEnd, rawEnd);
		if (start < end) appendNote(notes, note, start, end, clip.id, track.id, selection, result);
	}
}

QVector<MidiExportPatternLaneSource> lanesFor(const MidiExportSnapshot& snapshot, int patternIndex)
{
	QVector<MidiExportPatternLaneSource> result;
	for (const auto& lane : snapshot.patternLanes) if (lane.patternIndex == patternIndex) result.append(lane);
	return result;
}

void appendPatternNotes(QVector<PlannedNote>& notes, const MidiExportTrackSource& track,
	const MidiExportClipSource& instance, const MidiExportPatternLaneSource& lane,
	qint64 visibleEnd, const MidiExportSelection& selection, MidiExportPlanResult& result,
	PlannerControl& control)
{
	const qint64 period = instance.patternLength;
	if (period <= 0) { addIssue(result, QStringLiteral("invalid_pattern_length"), true, track.id, instance.id); return; }
	const qint64 laneEnd=lane.clipLength-lane.clipOffset;
	if(laneEnd<=0)return;
	const qint64 phase = ((instance.offset % period) + period) % period;
	for (const auto& note : lane.notes)
	{
		if(!control.checkCancellation(result))return;
		if(note.position>=laneEnd)continue;
		if (note.length <= 0 && note.type!=MidiExportNoteType::Step){addIssue(result,"invalid_note",true,track.id,instance.id,note.position);continue;}
		if(note.position<0){addIssue(result,"invalid_note",true,track.id,instance.id,note.position);continue;}
		if(note.type==MidiExportNoteType::Step&&selection.fidelity==MidiExportFidelity::Strict)
		{
			addIssue(result,"step_note_duration_ambiguous",true,track.id,instance.id,instance.start+note.position);
			continue;
		}
		const qint64 duration=qMin(note.type==MidiExportNoteType::Step?1500:note.length,laneEnd-note.position);
		qint64 delta = ((note.position + phase) % period + period) % period;
		if (instance.start > std::numeric_limits<qint64>::max() - delta)
		{
			addIssue(result, QStringLiteral("timeline_overflow"), true, track.id, instance.id);
			continue;
		}
		qint64 noteStart = instance.start + delta;
		const qint64 visibleStart=selection.hasRange?qMax(instance.start,selection.rangeStart):instance.start;
		const qint64 earliestStart=duration>visibleStart?0:visibleStart-duration+1;
		if(noteStart<earliestStart)
		{
			const qint64 distance=earliestStart-noteStart;
			const qint64 periods=distance/period+(distance%period!=0);
			if(periods>(std::numeric_limits<qint64>::max()-noteStart)/period)
			{
				addIssue(result,"timeline_overflow",true,track.id,instance.id,noteStart);
				continue;
			}
			noteStart+=periods*period;
		}
		if(note.volume==0&&noteStart<visibleEnd)
		{
			const auto occurrences=quint64(1+(visibleEnd-1-noteStart)/period);
			result.statistics.notesSkipped+=occurrences;
			continue;
		}
		while (noteStart < visibleEnd)
		{
			if(!control.checkCancellation(result))return;
			if(control.patternIterations++>=MaxPatternPlanningIterations){result.code=QStringLiteral("resource_limit");return;}
			if(result.code=="resource_limit")return;
			const auto noteEnd = noteStart > std::numeric_limits<qint64>::max() - duration
				? std::numeric_limits<qint64>::max() : qMin(noteStart + duration, visibleEnd);
			appendNote(notes, note, noteStart, noteEnd, instance.id, track.id, selection, result);
			if (noteStart > std::numeric_limits<qint64>::max() - period) break;
			noteStart += period;
		}
	}
}

void addPart(MidiExportPlan& plan, const QString& name, const QVector<PlannedNote>& notes,
	int program,const QVector<MidiExportEvent>& initialEvents,qint64 end,qint64 origin,qint64 stateTick,int channel,
	MidiExportPlanResult& result,const QString& trackId)
{
	if(result.statistics.notesWritten+quint64(notes.size())>500000){result.code="resource_limit";return;}
	MidiExportTrack output; output.name = name;
	if (!convertTick(end - origin, output.endTick)) { addIssue(result, QStringLiteral("midi_delta_out_of_range"), true, trackId); return; }
	quint64 initialTick=0;
	if(!convertTick(stateTick-origin,initialTick)){addIssue(result,"midi_delta_out_of_range",true,trackId);return;}
	for(auto event:initialEvents)
	{
		event.tick=initialTick;event.channel=static_cast<quint8>(channel);output.events.append(event);
		if(event.type==MidiExportEventType::ControlChange)++result.statistics.controllerEvents;
		if(event.type==MidiExportEventType::PitchBend)++result.statistics.pitchBendEvents;
	}
	if(program>=0&&program<=127)
	{
		output.events.append({initialTick,MidiExportEventType::ProgramChange,static_cast<quint8>(channel),static_cast<quint8>(program)});
		++result.statistics.programEvents;
	}
	for (const auto& note : notes)
	{
		const auto start = note.start - origin;
		const auto finish = note.end - origin;
		quint64 onTick=0,offTick=0;
		if (!convertTick(start,onTick) || !convertTick(finish,offTick))
		{
			addIssue(result, QStringLiteral("midi_delta_out_of_range"), true, trackId, note.clipId, note.start);
			continue;
		}
		output.events.append({onTick, MidiExportEventType::NoteOn, static_cast<quint8>(channel), static_cast<quint8>(note.key), static_cast<quint8>(note.velocity)});
		output.events.append({offTick, MidiExportEventType::NoteOff, static_cast<quint8>(channel), static_cast<quint8>(note.key), 0});
		++result.statistics.notesWritten;
	}
	plan.tracks.append(output);
	++result.statistics.musicalTracks;
}
}

MidiExportPlanResult planMidiExport(const MidiExportSnapshot& snapshot, const MidiExportSelection& selection,
	const std::atomic_bool* cancelled)
{
	MidiExportPlanResult result;
	result.plan.format = 1;
	result.plan.ppq = 480;
	PlannerControl control{cancelled};
	if(!control.checkCancellation(result))return result;
	if(snapshot.tracks.size()>256){result.code="resource_limit";return result;}
	quint64 clipCount=0,noteCount=0;
	if(snapshot.songEnd<0||snapshot.songEnd>8000000){result.code="resource_limit";return result;}
	for(const auto& track:snapshot.tracks)
	{
		clipCount+=quint64(track.clips.size());
		for(const auto& clip:track.clips)
		{
			if(clip.start<0||clip.start>8000000||clip.length<0||clip.length>8000000||clip.patternLength<0||clip.patternLength>8000000
				||clip.start>8000000-clip.length||clip.offset< -8000000||clip.offset>8000000){result.code="resource_limit";return result;}
			noteCount+=quint64(clip.notes.size());
		}
	}
	for(const auto& lane:snapshot.patternLanes)
	{
		if(lane.clipLength<0||lane.clipLength>8000000||lane.clipOffset<0||lane.clipOffset>8000000){result.code="resource_limit";return result;}
		noteCount+=quint64(lane.notes.size());
	}
	if(clipCount>4096||noteCount>262144||snapshot.patternLanes.size()>4096){result.code="resource_limit";return result;}
	if (snapshot.tempo < 1 || snapshot.tempo > 999 || snapshot.numerator < 1 || snapshot.numerator > 255
		|| !isPowerOfTwo(snapshot.denominator) || snapshot.denominator > 128)
	{
		addIssue(result, QStringLiteral("unsupported_conductor_value"), selection.fidelity == MidiExportFidelity::Strict);
	}
	if(snapshot.hasTempoAutomation)addIssue(result,"tempo_automation_not_captured",selection.fidelity==MidiExportFidelity::Strict);
	if(snapshot.hasTimeSignatureAutomation)addIssue(result,"meter_automation_not_captured",selection.fidelity==MidiExportFidelity::Strict);
	if(selection.fidelity==MidiExportFidelity::Strict && !result.code.isEmpty())return result;
	if (selection.hasRange && (selection.rangeStart < 0 || selection.rangeEnd <= selection.rangeStart || selection.rangeEnd > 8000000))
	{
		result.code = QStringLiteral("invalid_parameter");
		return result;
	}
	QSet<QString> requestedTracks,requestedClips;
	for(const auto& id:selection.trackIds){if(id.isEmpty()||requestedTracks.contains(id)){result.code="invalid_parameter";return result;}requestedTracks.insert(id);}
	for(const auto& id:selection.clipIds){if(id.isEmpty()||requestedClips.contains(id)){result.code="invalid_parameter";return result;}requestedClips.insert(id);}
	if ((selection.mode==MidiExportSelectionMode::Tracks && requestedTracks.isEmpty())
		|| (selection.mode==MidiExportSelectionMode::Clips && requestedClips.isEmpty())){result.code="invalid_parameter";return result;}

	QSet<QString> foundTracks,foundClips;
	QVector<const MidiExportTrackSource*> sourceTracks;
	for(const auto& track:snapshot.tracks)
	{
		if(!control.checkCancellation(result))return result;
		if(selection.mode==MidiExportSelectionMode::Tracks && !requestedTracks.contains(track.id))continue;
		if(selection.mode==MidiExportSelectionMode::Clips)
		{
			bool ownsSelectedClip=false;
			for(const auto& clip:track.clips)if(requestedClips.contains(clip.id)){foundClips.insert(clip.id);ownsSelectedClip=true;}
			if(!ownsSelectedClip)continue;
		}
		foundTracks.insert(track.id);
		if(track.type==MidiExportSourceTrackType::Unsupported)
		{
			if(selection.mode!=MidiExportSelectionMode::Song){result.code="unsupported_selection";return result;}
			if(!selection.includeMuted&&track.muted)continue;
			++result.statistics.unsupportedTracks;addIssue(result,"unsupported_track",false,track.id);continue;
		}
		if(!selection.includeMuted&&track.muted)
		{
			if(selection.mode==MidiExportSelectionMode::Tracks)addIssue(result,"selected_muted_track_skipped",false,track.id);
			continue;
		}
		sourceTracks.append(&track);
	}
	if(selection.mode==MidiExportSelectionMode::Tracks && foundTracks.size()!=requestedTracks.size()){result.code="track_not_found";return result;}
	if(selection.mode==MidiExportSelectionMode::Clips && foundClips.size()!=requestedClips.size()){result.code="clip_not_found";return result;}
	std::stable_sort(sourceTracks.begin(),sourceTracks.end(),[](auto* a,auto* b){return a->order<b->order;});
	QSet<int> assignedChannels,reservedChannels;
	QSet<int> selectedPatternIndexes;
	for(const auto* track:sourceTracks)
	{
		if(track->type==MidiExportSourceTrackType::Instrument)
		{
			if(track->midiChannel>=0&&track->midiChannel<16)reservedChannels.insert(track->midiChannel);
		}
		else
		{
			for(const auto& clip:track->clips)
				if(clip.patternIndex>=0&&includesClip(selection,clip)&&(selection.includeMuted||!clip.muted))selectedPatternIndexes.insert(clip.patternIndex);
		}
	}
	for(const auto& lane:snapshot.patternLanes)
		if(selectedPatternIndexes.contains(lane.patternIndex)&&lane.midiChannel>=0&&lane.midiChannel<16)reservedChannels.insert(lane.midiChannel);
	auto allocateChannel=[&](int requested,const QString& trackId){
		if(requested>=0&&requested<16){if(assignedChannels.contains(requested))addIssue(result,"midi_channel_conflict",selection.fidelity==MidiExportFidelity::Strict,trackId);assignedChannels.insert(requested);return requested;}
		for(int channel=0;channel<16;++channel)if(!assignedChannels.contains(channel)&&!reservedChannels.contains(channel)){assignedChannels.insert(channel);return channel;}
		addIssue(result,"midi_channel_capacity_exceeded",selection.fidelity==MidiExportFidelity::Strict,trackId);return 15;
	};
	qint64 firstContent=std::numeric_limits<qint64>::max();
	qint64 endContent=qMax<qint64>(0,snapshot.songEnd);
	for(const auto* track:sourceTracks)
		for(const auto& clip:track->clips)
			if((selection.mode!=MidiExportSelectionMode::Clips||requestedClips.contains(clip.id)) && (selection.includeMuted||!clip.muted))
			{firstContent=qMin(firstContent,clip.start);if(clip.start<=std::numeric_limits<qint64>::max()-clip.length)endContent=qMax(endContent,clip.start+clip.length);}
	if(firstContent==std::numeric_limits<qint64>::max())firstContent=0;
	const qint64 origin=selection.normalizeToZero?(selection.hasRange?selection.rangeStart:firstContent):0;
	const qint64 stateTick=selection.hasRange?selection.rangeStart:(selection.normalizeToZero?origin:0);
	const qint64 effectiveEnd=selection.hasRange?selection.rangeEnd:qMax(endContent,origin);
	result.statistics.startTick=selection.hasRange?selection.rangeStart:firstContent;
	result.statistics.endTick=effectiveEnd;

	MidiExportTrack conductor;conductor.name=QStringLiteral("Conductor");
	const auto conductorEnd=qMax<qint64>(effectiveEnd-origin,0);
	if(!convertTick(conductorEnd,conductor.endTick))addIssue(result,"midi_delta_out_of_range",true);
	const auto tempo=qBound(1,snapshot.tempo,999);
	const auto us=60000000/tempo;
	quint64 conductorTick=0;
	if(!convertTick(stateTick-origin,conductorTick))addIssue(result,"midi_delta_out_of_range",true);
	conductor.events.append({conductorTick,MidiExportEventType::Tempo,0,static_cast<quint8>((us>>16)&255),static_cast<quint8>((us>>8)&255),static_cast<quint8>(us&255)});
	const auto numerator=qBound(1,snapshot.numerator,255);
	const auto denominator=isPowerOfTwo(snapshot.denominator)&&snapshot.denominator<=128?snapshot.denominator:4;
	conductor.events.append({conductorTick,MidiExportEventType::TimeSignature,0,static_cast<quint8>(numerator),denominatorPower(denominator)});
	result.statistics.tempoEvents=1;result.statistics.timeSignatureEvents=1;
	result.plan.tracks.append(conductor);

	for(const auto* track:sourceTracks)
	{
		if(!control.checkCancellation(result))return result;
		if(track->unsupportedAutomation)addIssue(result,"unsupported_track_automation",selection.fidelity==MidiExportFidelity::Strict,track->id);
		if(track->type==MidiExportSourceTrackType::Instrument)
		{
			QVector<PlannedNote> notes;
			for(const auto& clip:track->clips)appendRegularClip(notes,clip,*track,selection,result,control);
			const auto channel=allocateChannel(track->midiChannel,track->id);
			addPart(result.plan,track->name,notes,track->program,track->initialEvents,effectiveEnd,origin,stateTick,channel,result,track->id);
			if(result.code=="resource_limit")return result;
			continue;
		}
		QVector<MidiExportClipSource> instances;
		for(const auto& clip:track->clips)if(clip.patternIndex>=0&&includesClip(selection,clip)&&(selection.includeMuted||!clip.muted))instances.append(clip);
		std::stable_sort(instances.begin(),instances.end(),[](const auto& a,const auto& b){return a.start<b.start;});
		QVector<int> patternIndexes;
		for(const auto& instance:instances)if(!patternIndexes.contains(instance.patternIndex))patternIndexes.append(instance.patternIndex);
		bool emittedLane=false;
		for(const auto patternIndex:patternIndexes)
		{
			auto lanes=lanesFor(snapshot,patternIndex);
			if(lanes.isEmpty()){addIssue(result,"empty_pattern",false,track->id);continue;}
			for(const auto& lane:lanes)
			{
				if(lane.muted&&!selection.includeMuted)
				{
					addIssue(result,"muted_pattern_lane_skipped",false,track->id);
					continue;
				}
				emittedLane=true;
				if(lane.unsupportedAutomation)addIssue(result,"unsupported_track_automation",selection.fidelity==MidiExportFidelity::Strict,track->id);
				QVector<PlannedNote> notes;
				for(int i=0;i<instances.size();++i)
				{
					const auto& instance=instances[i];if(instance.patternIndex!=patternIndex)continue;
					const qint64 clipEnd=instance.start>std::numeric_limits<qint64>::max()-instance.length?std::numeric_limits<qint64>::max():instance.start+instance.length;
					qint64 visibleEnd=clipEnd;
					for(int next=i+1;next<instances.size();++next)if(instances[next].start>instance.start){visibleEnd=qMin(visibleEnd,instances[next].start);break;}
					if(selection.hasRange){if(visibleEnd<=selection.rangeStart||instance.start>=selection.rangeEnd)continue;visibleEnd=qMin(visibleEnd,selection.rangeEnd);}
					appendPatternNotes(notes,*track,instance,lane,visibleEnd,selection,result,control);
					if(result.code=="cancelled"||result.code=="resource_limit")return result;
				}
				const auto channel=allocateChannel(lane.midiChannel,track->id);
				addPart(result.plan,QStringLiteral("%1 / %2").arg(track->name,lane.name),notes,lane.program,lane.initialEvents,effectiveEnd,origin,stateTick,channel,result,track->id);
				if(result.code=="resource_limit")return result;
			}
		}
		if(!emittedLane)
		{
			const auto channel=allocateChannel(-1,track->id);
			addPart(result.plan,track->name,{},-1,{},effectiveEnd,origin,stateTick,channel,result,track->id);
		}
	}
	if(result.statistics.notesWritten==0)addIssue(result,"empty_selection",false);
	result.statistics.midiTracks=result.plan.tracks.size();
	std::array<std::array<std::vector<quint64>,128>,16> onsets,offsets;
	for(const auto& track:result.plan.tracks)
		for(const auto& event:track.events)
			if(event.type==MidiExportEventType::NoteOn)onsets[event.channel][event.data1].push_back(event.tick);
			else if(event.type==MidiExportEventType::NoteOff)offsets[event.channel][event.data1].push_back(event.tick);
	for(int channel=0;channel<16;++channel)
	{
		for(int key=0;key<128;++key)
		{
			auto& starts=onsets[size_t(channel)][size_t(key)];auto& ends=offsets[size_t(channel)][size_t(key)];
			std::sort(starts.begin(),starts.end());std::sort(ends.begin(),ends.end());
			size_t endIndex=0;
			for(size_t i=0;i+1<starts.size();++i)
			{
				while(endIndex<ends.size()&&ends[endIndex]<=starts[i])++endIndex;
				if(endIndex<ends.size()&&starts[i+1]<ends[endIndex])
				{addIssue(result,"same_pitch_overlap",selection.fidelity==MidiExportFidelity::Strict,{}, {},static_cast<qint64>(starts[i]/10));break;}
			}
		}
	}
	result.success=result.code.isEmpty();
	if(!result.success&&result.code.isEmpty())result.code="fidelity_loss";
	if(result.success)result.code="ok";
	return result;
}

QString hashMidiExportSnapshot(const MidiExportSnapshot& snapshot)
{
	QCryptographicHash hash(QCryptographicHash::Sha256);
	hashInteger(hash,qint64(snapshot.projectGeneration));hashString(hash,snapshot.songName);hashInteger(hash,snapshot.tempo);
	hashInteger(hash,snapshot.numerator);hashInteger(hash,snapshot.denominator);hashInteger(hash,snapshot.songEnd);
	hashInteger(hash,snapshot.hasTempoAutomation);hashInteger(hash,snapshot.hasTimeSignatureAutomation);
	hashInteger(hash,snapshot.tracks.size());
	for(const auto& track:snapshot.tracks)
	{
		hashString(hash,track.id);hashString(hash,track.name);hashInteger(hash,track.order);hashInteger(hash,int(track.type));
		hashInteger(hash,track.muted);hashInteger(hash,track.midiChannel);hashInteger(hash,track.program);hashInteger(hash,track.unsupportedAutomation);hashEvents(hash,track.initialEvents);
		hashInteger(hash,track.clips.size());
		for(const auto& clip:track.clips)
		{hashString(hash,clip.id);hashInteger(hash,clip.start);hashInteger(hash,clip.length);hashInteger(hash,clip.offset);hashInteger(hash,clip.muted);hashInteger(hash,clip.patternIndex);hashInteger(hash,clip.patternLength);hashNotes(hash,clip.notes);}
	}
	hashInteger(hash,snapshot.patternLanes.size());
	for(const auto& lane:snapshot.patternLanes)
	{hashInteger(hash,lane.patternIndex);hashInteger(hash,lane.midiChannel);hashInteger(hash,lane.program);hashString(hash,lane.name);hashInteger(hash,lane.clipLength);hashInteger(hash,lane.clipOffset);hashInteger(hash,lane.unsupportedAutomation);hashInteger(hash,lane.muted);hashEvents(hash,lane.initialEvents);hashNotes(hash,lane.notes);}
	return QString::fromLatin1(hash.result().toHex());
}

} // namespace lmms
