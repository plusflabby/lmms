#include "AiBridgeProjectActions.h"

#include <QApplication>
#include <QThread>

#include "Engine.h"
#include "GuiApplication.h"
#include "MidiClip.h"
#include "Note.h"
#include "PianoRoll.h"

namespace lmms
{
namespace
{
AiBridgeProjectActionResult fail(const QString &code, const QString &message)
{
	return {false, code, message};
}

bool qt()
{
	return qApp && QThread::currentThread() == qApp->thread();
}

AiBridgeMutationState copyState(const MidiClip *clip)
{
	AiBridgeMutationState state;
	state.clip_length_ticks = clip->length().getTicks();
	state.auto_resize = clip->getAutoResize();
	for (const auto *note : clip->notes())
	{
		AiBridgeCopiedNote copied;
		copied.position_ticks = note->pos().getTicks();
		copied.length_ticks = note->length().getTicks();
		copied.key = note->key();
		copied.volume = note->getVolume();
		copied.panning = note->getPanning();
		copied.note_type = note->type() == Note::Type::Step ?
			QStringLiteral("step") : QStringLiteral("regular");
		state.notes.append(copied);
	}
	return state;
}

AiBridgeProjectActionResult finishMutation(AiBridgeProjectContext &context,
	const QString &id, MidiClip *clip, const AiBridgeMutationState &before,
	const QString &oldHash, int added, int deleted, int changed,
	const QString &message)
{
	const auto after = copyState(clip);
	auto result = fail("ok", message);
	result.success = true;
	result.mutation.clip_id = id;
	result.mutation.notes_added = added;
	result.mutation.notes_deleted = deleted;
	result.mutation.notes_changed = changed;
	result.mutation.old_content_hash = oldHash;
	result.mutation.new_content_hash = AiBridgeMutationTypes::contentHash(after);
	result.mutation.clip_length_ticks = after.clip_length_ticks;
	result.mutation.current_in_piano_roll = false;
	AiBridgePage page{0, 0, AiBridgeProjectContext::MaxPageSize, false};
	result.clip = context.clip(id, page);
	result.page = page;
	result.mutation.track_id = result.clip.trackId;
	return result;
}
}

AiBridgeProjectActionResult AiBridgeProjectActions::getProjectStructure()
{
	if (!qt()) return fail("wrong_thread", "project inspection requires Qt application thread");
	AiBridgeProjectContext context;
	auto result = fail("ok", "project structure captured");
	result.success = true;
	result.project = context.projectStructure();
	return result;
}

AiBridgeProjectActionResult AiBridgeProjectActions::listTracks(int offset, int limit)
{
	if (!qt()) return fail("wrong_thread", "track inspection requires Qt application thread");
	if (offset < 0 || limit < 0) return fail("invalid_parameter", "offset and limit must be non-negative");
	AiBridgeProjectContext context;
	AiBridgePage page{0, offset, limit, false};
	auto result = fail("ok", "tracks captured");
	result.success = true;
	result.tracks = context.tracks(page);
	result.page = page;
	return result;
}

AiBridgeProjectActionResult AiBridgeProjectActions::listClips(
	const QString &id, int offset, int limit)
{
	if (!qt()) return fail("wrong_thread", "clip inspection requires Qt application thread");
	if (offset < 0 || limit < 0) return fail("invalid_parameter", "offset and limit must be non-negative");
	AiBridgeProjectContext context;
	AiBridgePage page{0, offset, limit, false};
	if (!context.resolveTrack(id)) return fail("stale_track", "track is not live");
	auto result = fail("ok", "clips captured");
	result.success = true;
	result.clips = context.clips(id, page);
	result.page = page;
	return result;
}

AiBridgeProjectActionResult AiBridgeProjectActions::getClip(
	const QString &id, int offset, int limit)
{
	if (!qt()) return fail("wrong_thread", "clip inspection requires Qt application thread");
	if (offset < 0 || limit < 0) return fail("invalid_parameter", "offset and limit must be non-negative");
	AiBridgeProjectContext context;
	AiBridgePage page{0, offset, limit, false};
	if (!context.resolveClip(id)) return fail("stale_clip", "clip is not live");
	auto result = fail("ok", "clip captured");
	result.success = true;
	result.clip = context.clip(id, page);
	result.page = page;
	return result;
}

AiBridgeProjectActionResult AiBridgeProjectActions::openClipInPianoRoll(const QString &id)
{
	if (!qt()) return fail("wrong_thread", "navigation requires Qt application thread");
	AiBridgeProjectContext context;
	QString code;
	QString message;
	context.openableClip(id, code, message);
	if (code != "ok") return fail(code, message);
	auto *midi = dynamic_cast<MidiClip *>(context.resolveClip(id));
	auto *piano = gui::getGUI() ? gui::getGUI()->pianoRoll() : nullptr;
	if (!piano) return fail("piano_roll_unavailable", "Piano Roll is unavailable");
	piano->aiBridgeSetCurrentMidiClip(midi);
	piano->parentWidget()->show();
	piano->show();
	piano->setFocus();
	if (piano->aiBridgeCurrentMidiClip() != midi)
		return fail("editor_open_failed", "requested clip did not become current");
	AiBridgePage page{0, 0, 1, false};
	auto result = fail("ok", "MIDI clip opened in Piano Roll");
	result.success = true;
	result.clip = context.clip(id, page);
	result.page = page;
	result.context = context.currentContext();
	return result;
}

AiBridgeProjectActionResult AiBridgeProjectActions::addNotes(
	const QString &id, const QVector<AiBridgeCopiedNote> &notes,
	const QString &expected)
{
	if (!qt()) return fail("wrong_thread", "MIDI mutation requires Qt application thread");
	if (notes.isEmpty() || notes.size() > AiBridgeProjectContext::MaxPageSize)
		return fail("invalid_parameter", "notes must contain between 1 and 256 items");
	AiBridgeProjectContext context;
	auto *clip = dynamic_cast<MidiClip *>(context.resolveClip(id));
	if (!clip) return fail("stale_clip", "clip is not live");
	const auto before = copyState(clip);
	const auto oldHash = AiBridgeMutationTypes::contentHash(before);
	if (!expected.isEmpty() && expected != oldHash)
		return fail("clip_changed", "clip content hash does not match expected_content_hash");
	for (const auto &note : notes)
	{
		const auto validation = AiBridgeMutationTypes::validateNoteInput(note);
		if (!validation.success()) return fail(validation.code, validation.message);
	}
	clip->addJournalCheckPoint();
	for (const auto &copied : notes)
	{
		Note note(TimePos(static_cast<tick_t>(copied.length_ticks)),
			TimePos(static_cast<tick_t>(copied.position_ticks)), copied.key,
			copied.volume, copied.panning);
		note.setType(copied.note_type == QStringLiteral("step") ?
			Note::Type::Step : Note::Type::Regular);
		clip->addNote(note, false);
	}
	Engine::getSong()->setModified();
	const auto after = copyState(clip);
	auto result = fail("ok", "notes added");
	result.success = true;
	result.mutation.clip_id = id;
	result.mutation.notes_added = notes.size();
	result.mutation.old_content_hash = oldHash;
	result.mutation.new_content_hash = AiBridgeMutationTypes::contentHash(after);
	result.mutation.clip_length_ticks = after.clip_length_ticks;
	result.mutation.current_in_piano_roll = gui::getGUI() && gui::getGUI()->pianoRoll() &&
		gui::getGUI()->pianoRoll()->aiBridgeCurrentMidiClip() == clip;
	AiBridgePage page{0, 0, AiBridgeProjectContext::MaxPageSize, false};
	result.clip = context.clip(id, page);
	result.mutation.track_id = result.clip.trackId;
	result.page = page;
	return result;
}

AiBridgeProjectActionResult AiBridgeProjectActions::deleteNotes(
	const QString &id, const AiBridgeNoteSelection &selection,
	const QString &expected)
{
	if (!qt()) return fail("wrong_thread", "MIDI mutation requires Qt application thread");
	const auto selectionValidation = AiBridgeMutationTypes::validateSelection(selection, true);
	if (!selectionValidation.success())
		return fail(selectionValidation.code, selectionValidation.message);
	AiBridgeProjectContext context;
	auto *clip = dynamic_cast<MidiClip *>(context.resolveClip(id));
	if (!clip) return fail("stale_clip", "clip is not live");
	const auto before = copyState(clip);
	const auto oldHash = AiBridgeMutationTypes::contentHash(before);
	if (!expected.isEmpty() && expected != oldHash)
		return fail("clip_changed", "clip content hash does not match expected_content_hash");
	QVector<int> indices;
	for (int i = 0; i < before.notes.size(); ++i)
	{
		if (AiBridgeMutationTypes::matchesSelection(before.notes.at(i), selection, i))
			indices.append(i);
	}
	if (!indices.isEmpty())
	{
		clip->addJournalCheckPoint();
		for (auto i = indices.crbegin(); i != indices.crend(); ++i)
			clip->removeNote(clip->notes().at(*i));
		Engine::getSong()->setModified();
	}
	const auto after = copyState(clip);
	auto result = fail("ok", indices.isEmpty() ? "no notes matched" : "notes deleted");
	result.success = true;
	result.mutation.clip_id = id;
	result.mutation.notes_deleted = indices.size();
	result.mutation.old_content_hash = oldHash;
	result.mutation.new_content_hash = AiBridgeMutationTypes::contentHash(after);
	result.mutation.clip_length_ticks = after.clip_length_ticks;
	result.mutation.current_in_piano_roll = gui::getGUI() && gui::getGUI()->pianoRoll() &&
		gui::getGUI()->pianoRoll()->aiBridgeCurrentMidiClip() == clip;
	AiBridgePage page{0, 0, AiBridgeProjectContext::MaxPageSize, false};
	result.clip = context.clip(id, page);
	result.page = page;
	result.mutation.track_id = result.clip.trackId;
	return result;
}

AiBridgeProjectActionResult AiBridgeProjectActions::transposeNotes(
	const QString &id, const AiBridgeNoteSelection &selection, int semitones,
	const QString &expected)
{
	if (!qt()) return fail("wrong_thread", "MIDI mutation requires Qt application thread");
	const auto validation = AiBridgeMutationTypes::validateSelection(selection, true);
	if (!validation.success()) return fail(validation.code, validation.message);
	AiBridgeProjectContext context;
	auto *clip = dynamic_cast<MidiClip *>(context.resolveClip(id));
	if (!clip) return fail("stale_clip", "clip is not live");
	const auto before = copyState(clip);
	const auto oldHash = AiBridgeMutationTypes::contentHash(before);
	if (!expected.isEmpty() && expected != oldHash)
		return fail("clip_changed", "clip content hash does not match expected_content_hash");
	QVector<int> indices;
	for (int i = 0; i < before.notes.size(); ++i)
		if (AiBridgeMutationTypes::matchesSelection(before.notes.at(i), selection, i))
			indices.append(i);
	for (const auto index : indices)
	{
		const int key = before.notes.at(index).key + semitones;
		if (key < 0 || key > 127)
			return fail("invalid_note", "transpose would leave the MIDI key domain");
	}
	if (semitones == 0 || indices.isEmpty())
	{
		auto result = finishMutation(context, id, clip, before, oldHash, 0, 0, 0,
			indices.isEmpty() ? "no notes matched" : "transpose was a no-op");
		result.mutation.current_in_piano_roll = gui::getGUI() && gui::getGUI()->pianoRoll() &&
			gui::getGUI()->pianoRoll()->aiBridgeCurrentMidiClip() == clip;
		return result;
	}
	clip->addJournalCheckPoint();
	for (const auto index : indices)
		clip->notes().at(index)->setKey(before.notes.at(index).key + semitones);
	clip->rearrangeAllNotes();
	Engine::getSong()->setModified();
	auto result = finishMutation(context, id, clip, before, oldHash, 0, 0,
		indices.size(), "notes transposed");
	result.mutation.current_in_piano_roll = gui::getGUI() && gui::getGUI()->pianoRoll() &&
		gui::getGUI()->pianoRoll()->aiBridgeCurrentMidiClip() == clip;
	return result;
}

AiBridgeProjectActionResult AiBridgeProjectActions::moveNotes(
	const QString &id, const AiBridgeNoteSelection &selection, qint64 delta,
	const QString &expected)
{
	if (!qt()) return fail("wrong_thread", "MIDI mutation requires Qt application thread");
	const auto validation = AiBridgeMutationTypes::validateSelection(selection, true);
	if (!validation.success()) return fail(validation.code, validation.message);
	AiBridgeProjectContext context;
	auto *clip = dynamic_cast<MidiClip *>(context.resolveClip(id));
	if (!clip) return fail("stale_clip", "clip is not live");
	const auto before = copyState(clip);
	const auto oldHash = AiBridgeMutationTypes::contentHash(before);
	if (!expected.isEmpty() && expected != oldHash)
		return fail("clip_changed", "clip content hash does not match expected_content_hash");
	QVector<int> indices;
	for (int i = 0; i < before.notes.size(); ++i)
		if (AiBridgeMutationTypes::matchesSelection(before.notes.at(i), selection, i))
			indices.append(i);
	for (const auto index : indices)
	{
		const qint64 position = before.notes.at(index).position_ticks + delta;
		if (position < 0 || position > INT32_MAX)
			return fail("invalid_note", "move would leave the LMMS tick range");
	}
	if (delta == 0 || indices.isEmpty())
	{
		auto result = finishMutation(context, id, clip, before, oldHash, 0, 0, 0,
			indices.isEmpty() ? "no notes matched" : "move was a no-op");
		result.mutation.current_in_piano_roll = gui::getGUI() && gui::getGUI()->pianoRoll() &&
			gui::getGUI()->pianoRoll()->aiBridgeCurrentMidiClip() == clip;
		return result;
	}
	clip->addJournalCheckPoint();
	for (const auto index : indices)
		clip->notes().at(index)->setPos(TimePos(static_cast<tick_t>(
			before.notes.at(index).position_ticks + delta)));
	clip->rearrangeAllNotes();
	Engine::getSong()->setModified();
	auto result = finishMutation(context, id, clip, before, oldHash, 0, 0,
		indices.size(), "notes moved");
	result.mutation.current_in_piano_roll = gui::getGUI() && gui::getGUI()->pianoRoll() &&
		gui::getGUI()->pianoRoll()->aiBridgeCurrentMidiClip() == clip;
	return result;
}

AiBridgeProjectActionResult AiBridgeProjectActions::duplicateRegion(
	const QString &id, qint64 sourceStart, qint64 sourceEnd,
	qint64 destinationStart, const QString &expected)
{
	if (!qt()) return fail("wrong_thread", "MIDI mutation requires Qt application thread");
	if (sourceStart < 0 || sourceEnd <= sourceStart || sourceEnd > INT32_MAX ||
		destinationStart < 0)
		return fail("invalid_parameter", "duplicate region bounds are invalid");
	AiBridgeProjectContext context;
	auto *clip = dynamic_cast<MidiClip *>(context.resolveClip(id));
	if (!clip) return fail("stale_clip", "clip is not live");
	const auto before = copyState(clip);
	const auto oldHash = AiBridgeMutationTypes::contentHash(before);
	if (!expected.isEmpty() && expected != oldHash)
		return fail("clip_changed", "clip content hash does not match expected_content_hash");
	QVector<Note *> sources;
	for (auto *note : clip->notes())
	{
		const qint64 start = note->pos().getTicks();
		const qint64 end = start + note->length().getTicks();
		if (start >= sourceStart && end <= sourceEnd)
		{
			const qint64 destination = destinationStart + (start - sourceStart);
			if (destination < 0 || destination > INT32_MAX)
				return fail("invalid_note", "duplicate destination is outside the LMMS tick range");
			sources.append(note);
		}
	}
	if (sources.isEmpty())
		return finishMutation(context, id, clip, before, oldHash, 0, 0, 0,
			"no notes matched");
	clip->addJournalCheckPoint();
	for (auto *source : sources)
	{
		Note copy(*source);
		copy.setPos(TimePos(static_cast<tick_t>(
			destinationStart + source->pos().getTicks() - sourceStart)));
		clip->addNote(copy, false);
	}
	clip->rearrangeAllNotes();
	Engine::getSong()->setModified();
	return finishMutation(context, id, clip, before, oldHash, sources.size(), 0,
		0, "region duplicated");
}
}
