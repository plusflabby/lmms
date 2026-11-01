#include <QtTest/QtTest>
#include <algorithm>

#include "MidiExportService.h"

class MidiExportPlannerTest : public QObject
{
	Q_OBJECT

private slots:
	void clipsNotesToHalfOpenRangeAndNormalizesExactly();
	void rejectsStepNotesInStrictMode();
	void skipsMutedContentByDefaultAndKeepsGaps();
	void selectedMutedTrackIsRecognizedAndExcludedByDefault();
	void emptySongProducesConductorAndWarning();
	void emptyPatternTrackRetainsItsName();
	void rejectsSamePitchOverlapsAndUnrepresentedPerNoteExpression();
	void allocatesIndependentChannelsForUnassignedTracks();
	void reservesExplicitChannelsBeforeAllocatingAutomaticChannels();
	void normalizesNonzeroFirstClipWithoutRange();
	void clipSelectionIgnoresUnrelatedUnsupportedTracks();
	void excludesMutedPatternLanesUnlessRequested();
	void rangeSkipsEarlierPatternRepetitions();
	void plannerHonorsCooperativeCancellation();
	void preservesAuthoredControllerStateAtTheSelectedRangeStart();
	void expandsRepeatedPatternInstancesWithNativeLaterClipPrecedence();
};

void MidiExportPlannerTest::clipsNotesToHalfOpenRangeAndNormalizesExactly()
{
	lmms::MidiExportSnapshot snapshot;
	snapshot.tempo=120;snapshot.songEnd=384;
	lmms::MidiExportTrackSource track;track.id="track:1";track.name="Lead";track.order=0;
	track.type=lmms::MidiExportSourceTrackType::Instrument;
	lmms::MidiExportClipSource clip;clip.id="clip:1";clip.start=96;clip.length=192;clip.offset=24;
	clip.notes.append({0,96,60,100,lmms::MidiExportNoteType::Regular});track.clips.append(clip);snapshot.tracks.append(track);
	lmms::MidiExportSelection selection;selection.hasRange=true;selection.rangeStart=144;selection.rangeEnd=192;selection.normalizeToZero=true;
	const auto result=lmms::planMidiExport(snapshot,selection);
	QVERIFY2(result.success,qPrintable(result.code));
	QCOMPARE(result.plan.tracks.size(),2);
	QCOMPARE(result.plan.tracks[1].name,QStringLiteral("Lead"));
	QCOMPARE(result.plan.tracks[1].events.size(),2);
	QCOMPARE(result.plan.tracks[1].events[0].tick,quint64(0));
	QCOMPARE(result.plan.tracks[1].events[0].data1,quint8(60));
	QCOMPARE(result.plan.tracks[1].events[0].data2,quint8(64));
	QCOMPARE(result.plan.tracks[1].events[1].tick,quint64(480));
	QCOMPARE(result.plan.tracks[1].endTick,quint64(480));
}

void MidiExportPlannerTest::rejectsStepNotesInStrictMode()
{
	lmms::MidiExportSnapshot snapshot;
	lmms::MidiExportTrackSource track;track.id="track:1";track.name="Lead";track.type=lmms::MidiExportSourceTrackType::Instrument;
	lmms::MidiExportClipSource clip;clip.id="clip:1";clip.length=192;clip.notes.append({0,0,60,100,lmms::MidiExportNoteType::Step});
	track.clips.append(clip);snapshot.tracks.append(track);
	const auto strict=lmms::planMidiExport(snapshot,{});
	QVERIFY(!strict.success);
	QVERIFY(std::any_of(strict.issues.cbegin(),strict.issues.cend(),[](const auto& issue){return issue.code=="step_note_duration_ambiguous"&&issue.blocking;}));
	lmms::MidiExportSelection best;best.fidelity=lmms::MidiExportFidelity::BestEffort;
	const auto approximate=lmms::planMidiExport(snapshot,best);
	QVERIFY(approximate.success);
	QVERIFY(std::any_of(approximate.issues.cbegin(),approximate.issues.cend(),[](const auto& issue){return issue.code=="step_note_duration_approximated"&&!issue.blocking;}));
}

void MidiExportPlannerTest::skipsMutedContentByDefaultAndKeepsGaps()
{
	lmms::MidiExportSnapshot snapshot;snapshot.songEnd=768;
	lmms::MidiExportTrackSource track;track.id="track:1";track.name="Lead";track.type=lmms::MidiExportSourceTrackType::Instrument;
	lmms::MidiExportClipSource first;first.id="clip:1";first.start=0;first.length=192;first.notes.append({0,48,60,100,lmms::MidiExportNoteType::Regular});
	lmms::MidiExportClipSource muted;muted.id="clip:2";muted.start=576;muted.length=192;muted.muted=true;muted.notes.append({0,48,62,100,lmms::MidiExportNoteType::Regular});
	track.clips.append(first);track.clips.append(muted);snapshot.tracks.append(track);
	const auto result=lmms::planMidiExport(snapshot,{});
	QVERIFY(result.success);QCOMPARE(result.plan.tracks[1].events.size(),2);QCOMPARE(result.plan.tracks[1].endTick,quint64(7680));
}

void MidiExportPlannerTest::selectedMutedTrackIsRecognizedAndExcludedByDefault()
{
	lmms::MidiExportSnapshot snapshot;
	lmms::MidiExportTrackSource track;track.id="track:muted";track.name="Muted";track.type=lmms::MidiExportSourceTrackType::Instrument;track.muted=true;
	lmms::MidiExportClipSource clip;clip.id="clip:muted";clip.length=96;clip.notes.append({0,48,60,100,lmms::MidiExportNoteType::Regular});track.clips.append(clip);snapshot.tracks.append(track);
	lmms::MidiExportSelection selection;selection.mode=lmms::MidiExportSelectionMode::Tracks;selection.trackIds=QStringList{QStringLiteral("track:muted")};
	const auto result=lmms::planMidiExport(snapshot,selection);
	QVERIFY2(result.success,qPrintable(result.code));
	QCOMPARE(result.statistics.musicalTracks,0);
	QCOMPARE(result.statistics.notesWritten,quint64(0));
}

void MidiExportPlannerTest::emptySongProducesConductorAndWarning()
{
	const auto result=lmms::planMidiExport({},{});
	QVERIFY(result.success);
	QCOMPARE(result.plan.tracks.size(),1);
	QCOMPARE(result.plan.tracks[0].name,QStringLiteral("Conductor"));
	QVERIFY(std::any_of(result.issues.cbegin(),result.issues.cend(),[](const auto& issue){return issue.code=="empty_selection"&&!issue.blocking;}));
}

void MidiExportPlannerTest::emptyPatternTrackRetainsItsName()
{
	lmms::MidiExportSnapshot snapshot;
	lmms::MidiExportTrackSource track;track.id="pattern:1";track.name="Pattern 1";track.type=lmms::MidiExportSourceTrackType::Pattern;
	snapshot.tracks.append(track);
	const auto result=lmms::planMidiExport(snapshot,{});
	QVERIFY2(result.success,qPrintable(result.code));
	QCOMPARE(result.plan.tracks.size(),2);
	QCOMPARE(result.plan.tracks[1].name,QStringLiteral("Pattern 1"));
	QCOMPARE(result.plan.tracks[1].events.size(),0);
}

void MidiExportPlannerTest::rejectsSamePitchOverlapsAndUnrepresentedPerNoteExpression()
{
	lmms::MidiExportSnapshot snapshot;
	lmms::MidiExportTrackSource track;track.id="track:1";track.name="Lead";track.type=lmms::MidiExportSourceTrackType::Instrument;
	lmms::MidiExportClipSource clip;clip.id="clip:1";clip.length=192;
	clip.notes.append({0,96,60,100,lmms::MidiExportNoteType::Regular});
	clip.notes.append({48,96,60,100,lmms::MidiExportNoteType::Regular});
	clip.notes[0].panning=12;
	track.clips.append(clip);snapshot.tracks.append(track);
	const auto strict=lmms::planMidiExport(snapshot,{});
	QVERIFY(!strict.success);
	QVERIFY(std::any_of(strict.issues.cbegin(),strict.issues.cend(),[](const auto& issue){return issue.code=="same_pitch_overlap"&&issue.blocking;}));
	QVERIFY(std::any_of(strict.issues.cbegin(),strict.issues.cend(),[](const auto& issue){return issue.code=="per_note_panning_not_representable"&&issue.blocking;}));
	lmms::MidiExportSelection best;best.fidelity=lmms::MidiExportFidelity::BestEffort;
	const auto partial=lmms::planMidiExport(snapshot,best);
	QVERIFY(partial.success);
	QVERIFY(std::any_of(partial.issues.cbegin(),partial.issues.cend(),[](const auto& issue){return issue.code=="same_pitch_overlap"&&!issue.blocking;}));
}

void MidiExportPlannerTest::allocatesIndependentChannelsForUnassignedTracks()
{
	lmms::MidiExportSnapshot snapshot;
	for(int i=0;i<2;++i)
	{
		lmms::MidiExportTrackSource track;track.id=QStringLiteral("track:%1").arg(i);track.name=QStringLiteral("Part %1").arg(i);track.order=i;track.type=lmms::MidiExportSourceTrackType::Instrument;
		lmms::MidiExportClipSource clip;clip.id=QStringLiteral("clip:%1").arg(i);clip.length=96;clip.notes.append({0,48,60+i,100,lmms::MidiExportNoteType::Regular});
		track.clips.append(clip);snapshot.tracks.append(track);
	}
	const auto result=lmms::planMidiExport(snapshot,{});QVERIFY(result.success);
	QCOMPARE(result.plan.tracks.size(),3);
	QCOMPARE(result.plan.tracks[1].events[0].channel,quint8(0));
	QCOMPARE(result.plan.tracks[2].events[0].channel,quint8(1));
}

void MidiExportPlannerTest::reservesExplicitChannelsBeforeAllocatingAutomaticChannels()
{
	lmms::MidiExportSnapshot snapshot;
	for(int i=0;i<2;++i)
	{
		lmms::MidiExportTrackSource track;track.id=QStringLiteral("track:%1").arg(i);track.name=QStringLiteral("Part %1").arg(i);track.order=i;track.type=lmms::MidiExportSourceTrackType::Instrument;
		if(i==1)track.midiChannel=0;
		lmms::MidiExportClipSource clip;clip.id=QStringLiteral("clip:%1").arg(i);clip.length=96;clip.notes.append({0,48,60+i,100,lmms::MidiExportNoteType::Regular});
		track.clips.append(clip);snapshot.tracks.append(track);
	}
	const auto result=lmms::planMidiExport(snapshot,{});
	QVERIFY2(result.success,qPrintable(result.code));
	QCOMPARE(result.plan.tracks[1].events[0].channel,quint8(1));
	QCOMPARE(result.plan.tracks[2].events[0].channel,quint8(0));
}

void MidiExportPlannerTest::normalizesNonzeroFirstClipWithoutRange()
{
	lmms::MidiExportSnapshot snapshot;
	lmms::MidiExportTrackSource track;track.id="track:1";track.name="Later";track.type=lmms::MidiExportSourceTrackType::Instrument;
	lmms::MidiExportClipSource clip;clip.id="clip:1";clip.start=192;clip.length=96;clip.notes.append({0,24,60,100,lmms::MidiExportNoteType::Regular});
	track.clips.append(clip);snapshot.tracks.append(track);snapshot.songEnd=288;
	lmms::MidiExportSelection selection;selection.normalizeToZero=true;
	const auto result=lmms::planMidiExport(snapshot,selection);
	QVERIFY2(result.success,qPrintable(result.code));
	QCOMPARE(result.plan.tracks[0].events[0].tick,quint64(0));
	QCOMPARE(result.plan.tracks[1].events[0].tick,quint64(0));
	QCOMPARE(result.plan.tracks[1].events[1].tick,quint64(240));
}

void MidiExportPlannerTest::clipSelectionIgnoresUnrelatedUnsupportedTracks()
{
	lmms::MidiExportSnapshot snapshot;
	lmms::MidiExportTrackSource selected;selected.id="track:selected";selected.name="Selected";selected.type=lmms::MidiExportSourceTrackType::Instrument;
	lmms::MidiExportClipSource clip;clip.id="clip:selected";clip.length=96;clip.notes.append({0,24,60,100,lmms::MidiExportNoteType::Regular});selected.clips.append(clip);snapshot.tracks.append(selected);
	lmms::MidiExportTrackSource unrelated;unrelated.id="track:automation";unrelated.name="Automation";unrelated.type=lmms::MidiExportSourceTrackType::Unsupported;snapshot.tracks.append(unrelated);
	lmms::MidiExportSelection selection;selection.mode=lmms::MidiExportSelectionMode::Clips;selection.clipIds=QStringList{QStringLiteral("clip:selected")};
	const auto result=lmms::planMidiExport(snapshot,selection);
	QVERIFY2(result.success,qPrintable(result.code));
	QCOMPARE(result.statistics.musicalTracks,1);
	QCOMPARE(result.statistics.notesWritten,quint64(1));
}

void MidiExportPlannerTest::excludesMutedPatternLanesUnlessRequested()
{
	lmms::MidiExportSnapshot snapshot;
	lmms::MidiExportTrackSource track;track.id="pattern:1";track.name="Pattern 1";track.type=lmms::MidiExportSourceTrackType::Pattern;
	lmms::MidiExportClipSource instance;instance.id="clip:pattern";instance.length=96;instance.patternIndex=0;instance.patternLength=96;track.clips.append(instance);snapshot.tracks.append(track);
	lmms::MidiExportPatternLaneSource lane;lane.patternIndex=0;lane.name="Muted Piano";lane.clipLength=96;lane.muted=true;lane.notes.append({0,24,60,100,lmms::MidiExportNoteType::Regular});snapshot.patternLanes.append(lane);
	const auto muted=lmms::planMidiExport(snapshot,{});
	QVERIFY(muted.success);QCOMPARE(muted.statistics.notesWritten,quint64(0));
	QVERIFY(std::any_of(muted.issues.cbegin(),muted.issues.cend(),[](const auto& issue){return issue.code=="muted_pattern_lane_skipped";}));
	lmms::MidiExportSelection include;include.includeMuted=true;
	const auto included=lmms::planMidiExport(snapshot,include);
	QVERIFY(included.success);QCOMPARE(included.statistics.notesWritten,quint64(1));
}

void MidiExportPlannerTest::rangeSkipsEarlierPatternRepetitions()
{
	lmms::MidiExportSnapshot snapshot;snapshot.songEnd=8000000;
	lmms::MidiExportTrackSource track;track.id="pattern:long";track.name="Long Pattern";track.type=lmms::MidiExportSourceTrackType::Pattern;
	lmms::MidiExportClipSource instance;instance.id="clip:long";instance.length=8000000;instance.patternIndex=0;instance.patternLength=24;track.clips.append(instance);snapshot.tracks.append(track);
	lmms::MidiExportPatternLaneSource lane;lane.patternIndex=0;lane.name="Pulse";lane.clipLength=24;lane.notes.append({0,12,60,100,lmms::MidiExportNoteType::Regular});snapshot.patternLanes.append(lane);
	lmms::MidiExportSelection selection;selection.hasRange=true;selection.rangeStart=7999900;selection.rangeEnd=8000000;
	const auto result=lmms::planMidiExport(snapshot,selection);
	QVERIFY2(result.success,qPrintable(result.code));
	QVERIFY(result.statistics.notesWritten>0);QVERIFY(result.statistics.notesWritten<10);
}

void MidiExportPlannerTest::plannerHonorsCooperativeCancellation()
{
	std::atomic_bool cancelled{true};
	const auto result=lmms::planMidiExport({}, {}, &cancelled);
	QVERIFY(!result.success);QCOMPARE(result.code,QStringLiteral("cancelled"));
}

void MidiExportPlannerTest::preservesAuthoredControllerStateAtTheSelectedRangeStart()
{
	lmms::MidiExportSnapshot snapshot;
	lmms::MidiExportTrackSource track;track.id="track:1";track.name="Bass";track.type=lmms::MidiExportSourceTrackType::Instrument;track.midiChannel=2;
	track.initialEvents.append({0,lmms::MidiExportEventType::ControlChange,0,64,127});
	lmms::MidiExportClipSource clip;clip.id="clip:1";clip.start=96;clip.length=96;clip.notes.append({0,48,36,100,lmms::MidiExportNoteType::Regular});
	track.clips.append(clip);snapshot.tracks.append(track);
	lmms::MidiExportSelection selection;selection.hasRange=true;selection.rangeStart=96;selection.rangeEnd=192;selection.normalizeToZero=true;
	const auto result=lmms::planMidiExport(snapshot,selection);QVERIFY(result.success);
	QCOMPARE(result.plan.tracks[1].events[0].type,lmms::MidiExportEventType::ControlChange);
	QCOMPARE(result.plan.tracks[1].events[0].tick,quint64(0));
	QCOMPARE(result.plan.tracks[1].events[0].channel,quint8(2));
	QCOMPARE(result.plan.tracks[1].events[0].data1,quint8(64));
	QCOMPARE(result.plan.tracks[1].events[0].data2,quint8(127));
}

void MidiExportPlannerTest::expandsRepeatedPatternInstancesWithNativeLaterClipPrecedence()
{
	lmms::MidiExportSnapshot snapshot;snapshot.songEnd=288;
	lmms::MidiExportTrackSource track;track.id="pattern-track";track.name="Pattern 1";track.order=0;track.type=lmms::MidiExportSourceTrackType::Pattern;
	lmms::MidiExportClipSource first;first.id="pattern-clip-1";first.start=0;first.length=288;first.patternIndex=0;first.patternLength=96;
	lmms::MidiExportClipSource later;later.id="pattern-clip-2";later.start=192;later.length=96;later.patternIndex=0;later.patternLength=96;
	track.clips.append(first);track.clips.append(later);snapshot.tracks.append(track);
	lmms::MidiExportPatternLaneSource lane;lane.patternIndex=0;lane.name="Piano";lane.clipLength=96;
	lane.notes.append({0,24,60,100,lmms::MidiExportNoteType::Regular});snapshot.patternLanes.append(lane);
	const auto result=lmms::planMidiExport(snapshot,{});QVERIFY(result.success);
	QCOMPARE(result.plan.tracks.size(),2);QCOMPARE(result.plan.tracks[1].name,QStringLiteral("Pattern 1 / Piano"));
	QCOMPARE(result.plan.tracks[1].events.size(),6);
	QCOMPARE(result.plan.tracks[1].events[0].tick,quint64(0));
	QCOMPARE(result.plan.tracks[1].events[2].tick,quint64(960));
	QCOMPARE(result.plan.tracks[1].events[4].tick,quint64(1920));
}

QTEST_APPLESS_MAIN(MidiExportPlannerTest)
#include "MidiExportPlannerTest.moc"
