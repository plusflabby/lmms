#include "AiBridgeProjectActions.h"

#include <QApplication>
#include <QThread>

#include "Engine.h"
#include "GuiApplication.h"
#include "MidiClip.h"
#include "Note.h"
#include "PianoRoll.h"
#include "Track.h"
#include "TrackContainer.h"
#include "InstrumentTrack.h"
#include "PluginFactory.h"
#include "Plugin.h"
#include <limits>

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
AiBridgeProjectActionResult AiBridgeProjectActions::listInstruments(int offset, int limit)
{
	if (!qt()) return fail("wrong_thread", "instrument discovery requires Qt application thread");
	if (offset < 0 || limit < 0) return fail("invalid_parameter", "offset and limit must be non-negative");
	AiBridgeProjectContext context;
	AiBridgePage page{0, offset, limit, false};
	auto result = fail("ok", "instruments captured");
	result.success = true;
	result.instruments = context.instruments(page);
	result.page = page;
	return result;
}
AiBridgeProjectActionResult AiBridgeProjectActions::createInstrumentTrack(const std::optional<QString> &name)
{
	if (!qt()) return fail("wrong_thread", "track creation requires Qt application thread");
	if (name && (name->isEmpty() || name->toUtf8().size() > 256)) return fail("invalid_name", "name must be non-empty and at most 256 UTF-8 bytes");
	auto *track = Track::create(Track::Type::Instrument, Engine::getSong());
	if (!track) return fail("track_unavailable", "instrument track could not be created");
	if (name) track->setName(*name);
	AiBridgeProjectContext context;
	auto result = fail("ok", "instrument-free track created"); result.success=true;
	const auto value=context.trackSnapshot(track); result.track={value.id,value.name,value.type,value.index,value.volume,static_cast<int>(dynamic_cast<InstrumentTrack *>(track)->panningModel()->value())};
	return result;
}

AiBridgeProjectActionResult AiBridgeProjectActions::renameTrack(const QString &trackId, const QString &name)
{
	if (!qt()) return fail("wrong_thread", "track rename requires Qt application thread");
	if (name.isEmpty() || name.toUtf8().size() > 256) return fail("invalid_name", "name must be non-empty and at most 256 UTF-8 bytes");
	AiBridgeProjectContext context; auto *track=context.resolveTrack(trackId); if(!track) return fail("stale_track", "track is not live");
	const bool changed=track->displayName()!=name; if(changed) track->setName(name);
	auto result=fail("ok", changed ? "track renamed" : "track name unchanged"); result.success=true; const auto value=context.trackSnapshot(track); result.track={value.id,value.name,value.type,value.index,value.volume,track->type()==Track::Type::Instrument ? static_cast<int>(dynamic_cast<InstrumentTrack *>(track)->panningModel()->value()) : 0}; return result;
}

AiBridgeProjectActionResult AiBridgeProjectActions::setTrackInstrument(const QString &trackId, const QString &instrumentId)
{
	if (!qt()) return fail("wrong_thread", "instrument assignment requires Qt application thread");
	AiBridgeProjectContext context; auto *base=context.resolveTrack(trackId); auto *track=dynamic_cast<InstrumentTrack*>(base); if(!base) return fail("stale_track", "track is not live"); if(!track) return fail("invalid_track_type", "track must be an instrument track");
	const auto info=getPluginFactory()->pluginInfo(instrumentId.toUtf8().constData()); if(info.isNull() || !info.descriptor || info.descriptor->type!=Plugin::Type::Instrument || info.descriptor->subPluginFeatures) return fail("instrument_not_found", "instrument is not currently assignable");
	const auto logicalName=track->displayName(); auto *instrument=track->loadInstrument(instrumentId); if(!instrument) return fail("instrument_unavailable", "instrument could not be instantiated"); track->setName(logicalName);
	auto result=fail("ok", "instrument assigned"); result.success=true; const auto value=context.trackSnapshot(track); result.track={value.id,value.name,value.type,value.index,value.volume,static_cast<int>(track->panningModel()->value())}; result.trackProperties={value.id,value.volume,static_cast<int>(track->panningModel()->value())}; return result;
}

AiBridgeProjectActionResult AiBridgeProjectActions::setTrackProperties(const QString &trackId, const std::optional<int> &volume, const std::optional<int> &panning)
{
	if (!qt()) return fail("wrong_thread", "track properties require Qt application thread");
	if (!volume && !panning) return fail("invalid_parameter", "at least one property is required");
	if ((volume && (*volume<0 || *volume>200)) || (panning && (*panning<-100 || *panning>100))) return fail("invalid_parameter", "volume or panning is outside the LMMS range");
	AiBridgeProjectContext context; auto *base=context.resolveTrack(trackId); auto *track=dynamic_cast<InstrumentTrack*>(base); if(!base) return fail("stale_track", "track is not live"); if(!track) return fail("invalid_track_type", "track must be an instrument track");
	const bool changed=(volume && track->getVolume()!=*volume)||(panning && track->panningModel()->value()!=*panning); if(changed) { track->addJournalCheckPoint(); if(volume) track->setVolume(*volume); if(panning) track->panningModel()->setValue(*panning); Engine::getSong()->setModified(); }
	auto result=fail("ok", changed ? "track properties updated" : "track properties unchanged"); result.success=true; result.trackProperties={trackId,track->getVolume(),static_cast<int>(track->panningModel()->value())}; return result;
}

AiBridgeProjectActionResult AiBridgeProjectActions::createMidiClip(const QString &trackId, qint64 startTicks, const std::optional<qint64> &lengthTicks, bool autoResize)
{
	if (!qt()) return fail("wrong_thread", "clip creation requires Qt application thread");
	if(startTicks<0 || startTicks>std::numeric_limits<qint32>::max()) return fail("invalid_clip_position", "start_ticks is outside the LMMS tick range");
	if(lengthTicks && (*lengthTicks<1 || *lengthTicks>std::numeric_limits<qint32>::max() || startTicks+*lengthTicks>std::numeric_limits<qint32>::max())) return fail("invalid_clip_length", "length_ticks is outside the LMMS tick range");
	if(lengthTicks && autoResize) return fail("invalid_clip_length", "explicit length cannot be combined with auto_resize");
	AiBridgeProjectContext context; auto *base=context.resolveTrack(trackId); auto *track=dynamic_cast<InstrumentTrack*>(base); if(!base) return fail("stale_track", "track is not live"); if(!track) return fail("invalid_track_type", "track must be an instrument track");
	auto *clip=track->createClip(TimePos(static_cast<tick_t>(startTicks))); if(!clip) return fail("clip_unavailable", "MIDI clip could not be created"); clip->setAutoResize(autoResize); if(lengthTicks) clip->changeLength(TimePos(static_cast<tick_t>(*lengthTicks)));
	auto result=fail("ok", "MIDI clip created"); result.success=true; result.constructionClip={context.idForClip(clip),trackId,clip->displayName(),"midi",clip->startPosition().getTicks(),clip->length().getTicks(),clip->getAutoResize(),QString()}; return result;
}

AiBridgeProjectActionResult AiBridgeProjectActions::setClipProperties(
	const QString &id, const std::optional<qint64> &startTicks,
	const std::optional<qint64> &lengthTicks, const std::optional<bool> &autoResize,
	const std::optional<QString> &expectedContentHash,
	const std::optional<qint64> &expectedStartTicks)
{
	if (!qt()) return fail("wrong_thread", "clip properties require Qt application thread");
	if (!startTicks && !lengthTicks && !autoResize)
		return fail("invalid_parameter", "at least one clip property is required");
	if (startTicks && (*startTicks < 0 || *startTicks > std::numeric_limits<qint32>::max()))
		return fail("invalid_clip_position", "start_ticks is outside the LMMS tick range");
	if (lengthTicks && (*lengthTicks < 1 || *lengthTicks > std::numeric_limits<qint32>::max()))
		return fail("invalid_clip_length", "length_ticks is outside the LMMS tick range");
	if (startTicks && lengthTicks && *startTicks + *lengthTicks > std::numeric_limits<qint32>::max())
		return fail("invalid_clip_length", "clip end is outside the LMMS tick range");
	if (autoResize && *autoResize && lengthTicks)
		return fail("invalid_clip_length", "explicit length cannot be combined with auto_resize");

	AiBridgeProjectContext context;
	auto *clip = dynamic_cast<MidiClip *>(context.resolveClip(id));
	if (!clip) return fail("stale_clip", "clip is not live");
	const auto before = copyState(clip);
	const auto oldHash = AiBridgeMutationTypes::contentHash(before);
	const auto currentStart = clip->startPosition().getTicks();
	if (expectedStartTicks && *expectedStartTicks != currentStart)
		return fail("clip_changed", "clip start does not match expected_start_ticks");
	if (expectedContentHash && *expectedContentHash != oldHash)
		return fail("clip_changed", "clip content hash does not match expected_content_hash");
	if (lengthTicks && currentStart + *lengthTicks > std::numeric_limits<qint32>::max())
		return fail("invalid_clip_length", "clip end is outside the LMMS tick range");
	if (lengthTicks && !autoResize && clip->getAutoResize())
		return fail("invalid_clip_length", "explicit length cannot be combined with auto_resize");

	const bool changed = (startTicks && *startTicks != currentStart) ||
		(lengthTicks && *lengthTicks != clip->length().getTicks()) ||
		(autoResize && *autoResize != clip->getAutoResize());
	if (changed)
	{
		clip->addJournalCheckPoint();
		if (startTicks) clip->movePosition(TimePos(static_cast<tick_t>(*startTicks)));
		if (autoResize) clip->setAutoResize(*autoResize);
		if (lengthTicks) clip->changeLength(TimePos(static_cast<tick_t>(*lengthTicks)));
		Engine::getSong()->setModified();
	}

	const auto after = copyState(clip);
	auto result = fail("ok", changed ? "clip properties updated" : "clip properties unchanged");
	result.success = true;
	result.clipProperties = {id, clip->getTrack() ? context.idForTrack(clip->getTrack()) : QString(),
		clip->startPosition().getTicks(), clip->length().getTicks(), clip->getAutoResize(),
		AiBridgeMutationTypes::contentHash(after)};
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
	auto *window = piano->parentWidget();
	if (!window) return fail("piano_roll_unavailable", "Piano Roll window is unavailable");
	piano->aiBridgeSetCurrentMidiClip(midi);
	window->show();
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
