#include <QBuffer>
#include <QCoreApplication>
#include <QFile>

#include "MidiExportService.h"

#include <cstdio>

namespace
{
int fail(const QString& message)
{
	fprintf(stderr, "MidiExportFixtureTest: %s\n", qPrintable(message));
	return 1;
}
}

int main(int argc, char** argv)
{
	QCoreApplication application(argc, argv);
	lmms::MidiExportSnapshot snapshot;
	snapshot.tempo = 120;
	snapshot.numerator = 3;
	snapshot.denominator = 4;
	snapshot.songEnd = 576;

	lmms::MidiExportTrackSource lead;
	lead.id = "track:lead";
	lead.name = "Lead";
	lead.order = 0;
	lead.type = lmms::MidiExportSourceTrackType::Instrument;
	lmms::MidiExportClipSource leadClip;
	leadClip.id = "clip:lead";
	leadClip.start = 96;
	leadClip.offset = 24;
	leadClip.length = 192;
	leadClip.notes.append({0, 48, 60, 100, lmms::MidiExportNoteType::Regular});
	leadClip.notes.append({48, 48, 64, 100, lmms::MidiExportNoteType::Regular});
	leadClip.notes.append({48, 48, 67, 100, lmms::MidiExportNoteType::Regular});
	lead.clips.append(leadClip);
	snapshot.tracks.append(lead);

	lmms::MidiExportTrackSource bass;
	bass.id = "track:bass";
	bass.name = "Bass";
	bass.order = 1;
	bass.type = lmms::MidiExportSourceTrackType::Instrument;
	lmms::MidiExportClipSource bassClip;
	bassClip.id = "clip:bass";
	bassClip.start = 384;
	bassClip.length = 192;
	bassClip.notes.append({0, 96, 36, 200, lmms::MidiExportNoteType::Regular});
	bass.clips.append(bassClip);
	snapshot.tracks.append(bass);

	lmms::MidiExportTrackSource patternTrack;
	patternTrack.id = "track:pattern";
	patternTrack.name = "Pattern 1";
	patternTrack.order = 2;
	patternTrack.type = lmms::MidiExportSourceTrackType::Pattern;
	lmms::MidiExportClipSource firstPattern;
	firstPattern.id = "clip:pattern-1";
	firstPattern.start = 0;
	firstPattern.length = 192;
	firstPattern.patternIndex = 0;
	firstPattern.patternLength = 96;
	lmms::MidiExportClipSource repeatedPattern = firstPattern;
	repeatedPattern.id = "clip:pattern-2";
	repeatedPattern.start = 192;
	repeatedPattern.length = 96;
	patternTrack.clips = {firstPattern, repeatedPattern};
	snapshot.tracks.append(patternTrack);

	lmms::MidiExportPatternLaneSource bell;
	bell.patternIndex = 0;
	bell.name = "Bell";
	bell.clipLength = 96;
	bell.notes.append({0, 24, 72, 100, lmms::MidiExportNoteType::Regular});
	snapshot.patternLanes.append(bell);

	const auto planned = lmms::planMidiExport(snapshot, {});
	if (!planned.success) { return fail(QStringLiteral("planner rejected fixture: %1").arg(planned.code)); }
	if (planned.plan.tracks.size() != 4) { return fail(QStringLiteral("expected four planned tracks")); }

	QByteArray bytes;
	QBuffer buffer(&bytes);
	if (!buffer.open(QIODevice::WriteOnly)) { return fail(QStringLiteral("unable to open in-memory MIDI destination")); }
	const auto written = lmms::writeStandardMidi(planned.plan, buffer);
	if (!written.success) { return fail(QStringLiteral("writer rejected fixture: %1").arg(written.code)); }
	if (written.bytesWritten != bytes.size()) { return fail(QStringLiteral("writer byte count differs from generated MIDI")); }

	const auto path = QCoreApplication::applicationDirPath() + QStringLiteral("/midi-export-validation.mid");
	QFile fixture(path);
	if (!fixture.open(QIODevice::WriteOnly | QIODevice::Truncate))
	{
		return fail(QStringLiteral("unable to create fixture: %1").arg(fixture.errorString()));
	}
	if (fixture.write(bytes) != bytes.size() || !fixture.flush()) { return fail(QStringLiteral("unable to write complete fixture")); }
	fprintf(stdout, "Wrote %lld bytes to %s\n", static_cast<long long>(bytes.size()), qPrintable(path));
	return 0;
}
