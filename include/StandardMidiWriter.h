#ifndef LMMS_STANDARD_MIDI_WRITER_H
#define LMMS_STANDARD_MIDI_WRITER_H

#include <QIODevice>
#include <atomic>
#include <QString>
#include <QVector>
#include <QtGlobal>
#include "lmms_export.h"

namespace lmms
{

enum class MidiExportEventType
{
	NoteOff,
	ControlChange,
	ProgramChange,
	PitchBend,
	NoteOn,
	Tempo,
	TimeSignature
};

struct MidiExportEvent
{
	quint64 tick = 0;
	MidiExportEventType type = MidiExportEventType::NoteOn;
	quint8 channel = 0;
	quint8 data1 = 0;
	quint8 data2 = 0;
	quint8 data3 = 0;
};

struct MidiExportTrack
{
	QString name;
	QVector<MidiExportEvent> events;
	quint64 endTick = 0;
};

struct MidiExportPlan
{
	quint16 format = 1;
	quint16 ppq = 480;
	QVector<MidiExportTrack> tracks;
};

struct MidiExportWriteResult
{
	bool success = false;
	QString code;
	qint64 bytesWritten = 0;
};

LMMS_EXPORT MidiExportWriteResult writeStandardMidi(const MidiExportPlan& plan, QIODevice& destination,
	const std::atomic_bool* cancelled = nullptr);

} // namespace lmms

#endif // LMMS_STANDARD_MIDI_WRITER_H
