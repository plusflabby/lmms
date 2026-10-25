#ifndef LMMS_MIDI_EXPORT_SERVICE_H
#define LMMS_MIDI_EXPORT_SERVICE_H

#include "StandardMidiWriter.h"

#include <atomic>
#include <QVector>
#include "lmms_export.h"

namespace lmms
{

enum class MidiExportFidelity { Strict, BestEffort };
enum class MidiExportSelectionMode { Song, Tracks, Clips };
enum class MidiExportSourceTrackType { Instrument, Pattern, Unsupported };
enum class MidiExportNoteType { Regular, Step };

struct MidiExportSelection
{
	MidiExportSelectionMode mode = MidiExportSelectionMode::Song;
	QStringList trackIds;
	QStringList clipIds;
	bool hasRange = false;
	qint64 rangeStart = 0;
	qint64 rangeEnd = 0;
	bool normalizeToZero = false;
	bool includeMuted = false;
	MidiExportFidelity fidelity = MidiExportFidelity::Strict;
};

struct MidiExportNoteSource
{
	qint64 position = 0;
	qint64 length = 0;
	int key = 0;
	int volume = 100;
	MidiExportNoteType type = MidiExportNoteType::Regular;
	int panning = 0;
	bool hasDetuning = false;
};

struct MidiExportClipSource
{
	QString id;
	qint64 start = 0;
	qint64 length = 0;
	qint64 offset = 0;
	bool muted = false;
	int patternIndex = -1;
	qint64 patternLength = 0;
	QVector<MidiExportNoteSource> notes;
};

struct MidiExportPatternLaneSource
{
	int patternIndex = -1;
	int midiChannel = -1;
	int program = -1;
	QVector<MidiExportEvent> initialEvents;
	bool unsupportedAutomation = false;
	bool muted = false;
	QString name;
	qint64 clipLength = 0;
	qint64 clipOffset = 0;
	QVector<MidiExportNoteSource> notes;
};

struct MidiExportTrackSource
{
	QString id;
	QString name;
	int order = 0;
	MidiExportSourceTrackType type = MidiExportSourceTrackType::Unsupported;
	int midiChannel = -1;
	int program = -1;
	QVector<MidiExportEvent> initialEvents;
	bool unsupportedAutomation = false;
	bool muted = false;
	QVector<MidiExportClipSource> clips;
};

struct MidiExportSnapshot
{
	quint64 projectGeneration = 0;
	QString contentHash;
	QString songName;
	int tempo = 120;
	int numerator = 4;
	int denominator = 4;
	bool hasTempoAutomation = false;
	bool hasTimeSignatureAutomation = false;
	qint64 songEnd = 0;
	QVector<MidiExportTrackSource> tracks;
	QVector<MidiExportPatternLaneSource> patternLanes;
};

struct MidiExportIssue
{
	QString code;
	QString severity;
	bool blocking = false;
	QString trackId;
	QString clipId;
	qint64 tick = -1;
	quint64 count = 1;
	QString message;
};

struct MidiExportStatistics
{
	int musicalTracks = 0;
	int midiTracks = 0;
	quint64 notesWritten = 0;
	quint64 notesClipped = 0;
	quint64 notesSkipped = 0;
	quint64 unsupportedTracks = 0;
	quint64 tempoEvents = 0;
	quint64 timeSignatureEvents = 0;
	quint64 controllerEvents = 0;
	quint64 pitchBendEvents = 0;
	quint64 programEvents = 0;
	qint64 startTick = 0;
	qint64 endTick = 0;
};

struct MidiExportPlanResult
{
	bool success = false;
	QString code;
	MidiExportPlan plan;
	MidiExportStatistics statistics;
	QVector<MidiExportIssue> issues;
};

LMMS_EXPORT MidiExportPlanResult planMidiExport(const MidiExportSnapshot& snapshot, const MidiExportSelection& selection,
	const std::atomic_bool* cancelled = nullptr);
LMMS_EXPORT QString hashMidiExportSnapshot(const MidiExportSnapshot& snapshot);

} // namespace lmms

#endif // LMMS_MIDI_EXPORT_SERVICE_H
