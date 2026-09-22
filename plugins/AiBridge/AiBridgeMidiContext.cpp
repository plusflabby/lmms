#include "AiBridgeMidiContext.h"

namespace lmms
{
AiBridgeMidiCapabilities AiBridgeMidiContext::getCapabilities()
{
	AiBridgeMidiCapabilities result;
	result.humanizeDistributions = QStringList{QStringLiteral("triangular"), QStringLiteral("uniform")};
	return result;
}

AiBridgeMidiNotePage AiBridgeMidiContext::pageNotes(const AiBridgeMidiClipSnapshot &snapshot, int offset, int limit)
{
	AiBridgeMidiNotePage result;
	const auto canonical = canonicalizeMidiNotes(snapshot.notes);
	result.midiStateHash = computeMidiStateHash(snapshot);
	result.total = canonical.size();
	result.offset = qBound(0, offset, result.total);
	result.limit = qBound(1, limit, AiBridgeMidiLimits::maxNotesPerPage);
	result.hasMore = result.offset + result.limit < result.total;
	result.notes = canonical.mid(result.offset, result.limit);
	result.noteRefs = makeMidiNoteRefs(snapshot).mid(result.offset, result.limit);
	return result;
}

AiBridgeMidiSelectionResult AiBridgeMidiContext::resolveSelection(const AiBridgeMidiClipSnapshot &snapshot,
	const QString &expectedMidiStateHash, const AiBridgeMidiSelection &selection)
{
	const auto hash = computeMidiStateHash(snapshot);
	if (expectedMidiStateHash != hash) return {false, QStringLiteral("midi_state_mismatch"), {}};
	if (const auto validation = selection.validate(); !validation.success()) return {false, validation.code, {}};
	const auto canonical = canonicalizeMidiNotes(snapshot.notes);
	const auto refs = makeMidiNoteRefs(snapshot);
	QVector<int> selected;
	for (int index = 0; index < canonical.size(); ++index)
	{
		const auto &note = canonical[index];
		const bool exact = selection.mode == AiBridgeMidiSelectionMode::NoteRefs
			? selection.noteRefs.contains(refs[index])
			: (!selection.startTick || (note.positionTicks >= *selection.startTick && note.positionTicks < *selection.endTick))
				&& (selection.keys.isEmpty() || selection.keys.contains(note.key))
				&& (!selection.keyMin || (note.key >= *selection.keyMin && note.key <= *selection.keyMax))
				&& (!selection.volumeMin || (note.volume >= *selection.volumeMin && note.volume <= *selection.volumeMax))
				&& (!selection.lengthMin || (note.lengthTicks >= *selection.lengthMin && note.lengthTicks <= *selection.lengthMax));
		if (exact) selected.append(index);
	}
	if (selection.mode == AiBridgeMidiSelectionMode::NoteRefs && selected.size() != selection.noteRefs.size())
		return {false, QStringLiteral("stale_note_ref"), {}};
	if (selected.size() > AiBridgeMidiLimits::maxSelectedNotesPerMutation)
		return {false, QStringLiteral("note_limit_exceeded"), {}};
	return {true, QString(), selected};
}
}
