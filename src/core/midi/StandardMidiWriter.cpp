#include "StandardMidiWriter.h"

#include <QByteArray>
#include <algorithm>
#include <tuple>

namespace lmms
{
namespace
{
constexpr quint64 MaxVlq = 0x0fffffff;
constexpr qint64 MaxOutputBytes = 128LL * 1024 * 1024;
constexpr int MaxTrackBytes = 64 * 1024 * 1024;

void appendU16(QByteArray& out, quint16 value)
{
	out.append(static_cast<char>((value >> 8) & 0xff));
	out.append(static_cast<char>(value & 0xff));
}

void appendU32(QByteArray& out, quint32 value)
{
	out.append(static_cast<char>((value >> 24) & 0xff));
	out.append(static_cast<char>((value >> 16) & 0xff));
	out.append(static_cast<char>((value >> 8) & 0xff));
	out.append(static_cast<char>(value & 0xff));
}

bool appendVlq(QByteArray& out, quint64 value)
{
	if (value > MaxVlq) { return false; }
	char bytes[4];
	int count = 1;
	bytes[3] = static_cast<char>(value & 0x7f);
	while ((value >>= 7) != 0)
	{
		bytes[3 - count] = static_cast<char>((value & 0x7f) | 0x80);
		++count;
	}
	out.append(bytes + 4 - count, count);
	return true;
}

int eventPriority(MidiExportEventType type)
{
	switch (type)
	{
	case MidiExportEventType::Tempo:
	case MidiExportEventType::TimeSignature: return 0;
	case MidiExportEventType::NoteOff: return 1;
	case MidiExportEventType::ProgramChange:
	case MidiExportEventType::ControlChange:
	case MidiExportEventType::PitchBend: return 2;
	case MidiExportEventType::NoteOn: return 3;
	}
	return 4;
}

bool appendEvent(QByteArray& out, const MidiExportEvent& event, quint64 delta)
{
	if (!appendVlq(out, delta)) { return false; }
	const auto channel = event.channel;
	switch (event.type)
	{
	case MidiExportEventType::NoteOn:
	case MidiExportEventType::NoteOff:
		if (event.data1 > 127 || event.data2 > 127) { return false; }
		out.append(static_cast<char>((event.type == MidiExportEventType::NoteOn ? 0x90 : 0x80) | channel));
		out.append(static_cast<char>(event.data1));
		out.append(static_cast<char>(event.data2));
		return true;
	case MidiExportEventType::ProgramChange:
		if (event.data1 > 127) { return false; }
		out.append(static_cast<char>(0xc0 | channel));
		out.append(static_cast<char>(event.data1));
		return true;
	case MidiExportEventType::ControlChange:
		if (event.data1 > 127 || event.data2 > 127) { return false; }
		out.append(static_cast<char>(0xb0 | channel));
		out.append(static_cast<char>(event.data1));
		out.append(static_cast<char>(event.data2));
		return true;
	case MidiExportEventType::PitchBend:
		if (event.data1 > 127 || event.data2 > 127) { return false; }
		out.append(static_cast<char>(0xe0 | channel));
		out.append(static_cast<char>(event.data1));
		out.append(static_cast<char>(event.data2));
		return true;
	case MidiExportEventType::Tempo:
		out.append(static_cast<char>(0xff));
		out.append(static_cast<char>(0x51));
		out.append(static_cast<char>(0x03));
		out.append(static_cast<char>(event.data1));
		out.append(static_cast<char>(event.data2));
		out.append(static_cast<char>(event.data3));
		return event.data1 != 0 || event.data2 != 0 || event.data3 != 0;
	case MidiExportEventType::TimeSignature:
		if (event.data1 == 0 || event.data2 > 7) { return false; }
		out.append(static_cast<char>(0xff));
		out.append(static_cast<char>(0x58));
		out.append(static_cast<char>(0x04));
		out.append(static_cast<char>(event.data1));
		out.append(static_cast<char>(event.data2));
		out.append(static_cast<char>(event.data3 == 0 ? 24 : event.data3));
		out.append(static_cast<char>(8));
		return true;
	}
	return false;
}

MidiExportWriteResult fail(const QString& code)
{
	return {false, code, 0};
}
}

MidiExportWriteResult writeStandardMidi(const MidiExportPlan& plan, QIODevice& destination,const std::atomic_bool* cancelled)
{
	if(cancelled&&cancelled->load(std::memory_order_acquire))return fail(QStringLiteral("cancelled"));
	if (!destination.isOpen() || !destination.isWritable()) { return fail(QStringLiteral("destination_not_writable")); }
	if (plan.format != 1 || plan.ppq == 0 || plan.ppq > 0x7fff || plan.tracks.isEmpty() || plan.tracks.size() > 0xffff)
	{
		return fail(QStringLiteral("invalid_midi_header"));
	}

	QByteArray output;
	output.reserve(14);
	output.append("MThd", 4);
	appendU32(output, 6);
	appendU16(output, plan.format);
	appendU16(output, static_cast<quint16>(plan.tracks.size()));
	appendU16(output, plan.ppq);

	for (const auto& track : plan.tracks)
	{
		const auto name = track.name.toUtf8();
		if (name.size() > 0xffff) { return fail(QStringLiteral("track_name_too_long")); }
		QVector<MidiExportEvent> events = track.events;
		for (const auto& event : events)
		{
			if(cancelled&&cancelled->load(std::memory_order_acquire))return fail(QStringLiteral("cancelled"));
			if (event.channel > 15) { return fail(QStringLiteral("channel_out_of_range")); }
		}
		std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
			return std::tuple{a.tick, eventPriority(a.type)} < std::tuple{b.tick, eventPriority(b.type)};
		});

		QByteArray body;
		body.reserve(qMin(MaxTrackBytes, 16 + events.size() * 8));
		body.append(char(0));
		body.append(char(0xff));
		body.append(char(0x03));
		if (!appendVlq(body, static_cast<quint64>(name.size()))) { return fail(QStringLiteral("track_name_too_long")); }
		body.append(name);

		quint64 lastTick = 0;
		for (const auto& event : events)
		{
			if (event.tick < lastTick || event.tick > track.endTick)
			{
				return fail(QStringLiteral("event_after_track_end"));
			}
			if (!appendEvent(body, event, event.tick - lastTick))
			{
				return fail(QStringLiteral("invalid_event_or_delta"));
			}
			lastTick = event.tick;
			if (body.size() > MaxTrackBytes) { return fail(QStringLiteral("track_size_limit")); }
		}
		if (track.endTick < lastTick || !appendVlq(body, track.endTick - lastTick))
		{
			return fail(QStringLiteral("invalid_track_end"));
		}
		body.append(char(0xff));
		body.append(char(0x2f));
		body.append(char(0x00));
		if (body.size() > MaxTrackBytes) { return fail(QStringLiteral("track_size_limit")); }

		output.append("MTrk", 4);
		appendU32(output, static_cast<quint32>(body.size()));
		output.append(body);
		if (output.size() > MaxOutputBytes) { return fail(QStringLiteral("output_size_limit")); }
	}

	qint64 offset = 0;
	while (offset < output.size())
	{
		if(cancelled&&cancelled->load(std::memory_order_acquire))return fail(QStringLiteral("cancelled"));
		const auto written = destination.write(output.constData() + offset, output.size() - offset);
		if (written <= 0) { return fail(QStringLiteral("write_failed")); }
		offset += written;
	}
	return {true, QStringLiteral("ok"), offset};
}

} // namespace lmms
