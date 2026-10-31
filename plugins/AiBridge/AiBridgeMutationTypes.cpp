#include "AiBridgeMutationTypes.h"

#include <QCryptographicHash>
#include <QIODevice>
#include <QTextStream>
#include <algorithm>
#include <cstdint>

namespace lmms::AiBridgeMutationTypes
{

AiBridgeValidationResult validateNoteInput(const AiBridgeCopiedNote &note)
{
	if (note.position_ticks < 0 || note.position_ticks > INT32_MAX)
		return {QStringLiteral("invalid_note"), QStringLiteral("note position is outside the LMMS tick range")};
	if (note.length_ticks <= 0 || note.length_ticks > INT32_MAX ||
		note.position_ticks > INT32_MAX - note.length_ticks)
		return {QStringLiteral("invalid_note"), QStringLiteral("note length is outside the LMMS tick range")};
	if (note.key < 0 || note.key > 127)
		return {QStringLiteral("invalid_note"), QStringLiteral("MIDI key must be between 0 and 127")};
	if (note.volume < 0 || note.volume > 200)
		return {QStringLiteral("invalid_note"), QStringLiteral("LMMS note volume must be between 0 and 200")};
	if (note.panning < -100 || note.panning > 100)
		return {QStringLiteral("invalid_note"), QStringLiteral("LMMS note panning must be between -100 and 100")};
	if (note.note_type != QStringLiteral("regular") && note.note_type != QStringLiteral("step"))
		return {QStringLiteral("invalid_note"), QStringLiteral("note_type must be regular or step")};
	return {};
}

AiBridgeValidationResult validateSelection(const AiBridgeNoteSelection &selection, bool requires_bounded_range)
{
	if (selection.position_start.has_value() != selection.position_end.has_value())
		return {QStringLiteral("invalid_selection"), QStringLiteral("position range endpoints must be paired")};
	if (selection.position_start && (*selection.position_start < 0 || *selection.position_end <= *selection.position_start ||
		*selection.position_end > INT32_MAX))
		return {QStringLiteral("invalid_selection"), QStringLiteral("position range must be bounded and half-open")};
	if (selection.key_min.has_value() != selection.key_max.has_value())
		return {QStringLiteral("invalid_selection"), QStringLiteral("key range endpoints must be paired")};
	if (selection.key_min && (*selection.key_min < 0 || *selection.key_max > 127 || *selection.key_min > *selection.key_max))
		return {QStringLiteral("invalid_selection"), QStringLiteral("key range is outside the MIDI domain")};
	if (requires_bounded_range && !selection.position_start && !selection.key_min)
		return {QStringLiteral("selection_requires_range"), QStringLiteral("mutation selection requires a bounded position or key range")};
	if (selection.exact)
	{
		const auto &exact = *selection.exact;
		if (exact.position && (*exact.position < 0 || *exact.position > INT32_MAX))
			return {QStringLiteral("invalid_selection"), QStringLiteral("exact position is outside the LMMS tick range")};
		if (exact.length && (*exact.length <= 0 || *exact.length > INT32_MAX))
			return {QStringLiteral("invalid_selection"), QStringLiteral("exact length is outside the LMMS tick range")};
		if (exact.key && (*exact.key < 0 || *exact.key > 127))
			return {QStringLiteral("invalid_selection"), QStringLiteral("exact key is outside the MIDI domain")};
		if (exact.volume && (*exact.volume < 0 || *exact.volume > 200))
			return {QStringLiteral("invalid_selection"), QStringLiteral("exact volume is outside the LMMS range")};
		if (exact.panning && (*exact.panning < -100 || *exact.panning > 100))
			return {QStringLiteral("invalid_selection"), QStringLiteral("exact panning is outside the LMMS range")};
		if (exact.note_type && *exact.note_type != QStringLiteral("regular") && *exact.note_type != QStringLiteral("step"))
			return {QStringLiteral("invalid_selection"), QStringLiteral("exact note type must be regular or step")};
	}
	return {};
}

bool matchesSelection(const AiBridgeCopiedNote &note, const AiBridgeNoteSelection &selection, qint64 source_index)
{
	Q_UNUSED(source_index);
	if (selection.position_start && (note.position_ticks < *selection.position_start || note.position_ticks >= *selection.position_end)) return false;
	if (selection.key_min && (note.key < *selection.key_min || note.key > *selection.key_max)) return false;
	if (selection.exact)
	{
		const auto &exact = *selection.exact;
		if (exact.position && note.position_ticks != *exact.position) return false;
		if (exact.length && note.length_ticks != *exact.length) return false;
		if (exact.key && note.key != *exact.key) return false;
		if (exact.volume && note.volume != *exact.volume) return false;
		if (exact.panning && note.panning != *exact.panning) return false;
		if (exact.note_type && note.note_type != *exact.note_type) return false;
	}
	return true;
}

QByteArray canonicalMutationState(const AiBridgeMutationState &state)
{
	QVector<AiBridgeCopiedNote> notes = state.notes;
	std::stable_sort(notes.begin(), notes.end(), [](const auto &a, const auto &b) {
		if (a.position_ticks != b.position_ticks) return a.position_ticks < b.position_ticks;
		if (a.key != b.key) return a.key < b.key;
		if (a.length_ticks != b.length_ticks) return a.length_ticks < b.length_ticks;
		if (a.volume != b.volume) return a.volume < b.volume;
		if (a.panning != b.panning) return a.panning < b.panning;
		return a.note_type < b.note_type;
	});

	QByteArray result;
	QTextStream stream(&result, QIODevice::WriteOnly);
	stream << "lmms-phase3c-content-v1\n";
	stream << "length_ticks=" << state.clip_length_ticks << '\n';
	stream << "auto_resize=" << (state.auto_resize ? 1 : 0) << '\n';
	stream << "note_count=" << notes.size() << '\n';
	for (int i = 0; i < notes.size(); ++i)
	{
		const auto &note = notes.at(i);
		int occurrence = 0;
		for (int j = 0; j < i; ++j)
		{
			const auto &previous = notes.at(j);
			if (previous.position_ticks == note.position_ticks && previous.key == note.key &&
				previous.length_ticks == note.length_ticks && previous.volume == note.volume &&
				previous.panning == note.panning && previous.note_type == note.note_type)
				++occurrence;
		}
		stream << "note=" << note.position_ticks << ',' << note.length_ticks << ',' << note.key << ','
			<< note.volume << ',' << note.panning << ',' << note.note_type << ',' << occurrence << '\n';
	}
	stream.flush();
	return result;
}

QString contentHash(const AiBridgeMutationState &state)
{
	return QString::fromLatin1(QCryptographicHash::hash(canonicalMutationState(state), QCryptographicHash::Sha256).toHex());
}

} // namespace lmms::AiBridgeMutationTypes
