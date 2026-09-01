#include "AiBridgeActions.h"

#include <QApplication>
#include <QThread>

#include "Engine.h"
#include "GuiApplication.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Note.h"
#include "PianoRoll.h"
#include "Song.h"

namespace lmms
{

static AiBridgeActionResult requireMainThread()
{
	if (qApp == nullptr || QThread::currentThread() != qApp->thread())
	{
		return AiBridgeActionResult::failure("wrong_thread",
			"LMMS model mutations must run on the Qt main thread");
	}
	return AiBridgeActionResult::ok(QString());
}

AiBridgeActionResult AiBridgeActions::setSongTempo(AiBridgeContext &context, int bpm)
{
	if (const auto result = requireMainThread(); !result.success) return result;
	const auto resolved = context.resolve();
	if (resolved.song == nullptr) return AiBridgeActionResult::failure("no_song", "No song is open");
	if (bpm < MinTempo || bpm > MaxTempo)
	{
		return AiBridgeActionResult::failure("invalid_parameter", "Tempo is outside LMMS limits");
	}
	resolved.song->tempoModel().setValue(bpm);
	return AiBridgeActionResult::ok("Song tempo updated");
}

AiBridgeActionResult AiBridgeActions::adjustCurrentInstrumentVolume(AiBridgeContext &context,
	float delta)
{
	if (const auto result = requireMainThread(); !result.success) return result;
	const auto resolved = context.resolve();
	if (resolved.midiClip == nullptr) return AiBridgeActionResult::failure("no_current_midi_clip", "No current MIDI clip");
	if (resolved.instrumentTrack == nullptr)
		return AiBridgeActionResult::failure("no_owning_instrument_track", "Current clip has no instrument track");
	resolved.instrumentTrack->setVolume(qBound(0, static_cast<int>(resolved.instrumentTrack->getVolume() + delta), 100));
	return AiBridgeActionResult::ok("Instrument track volume updated");
}

AiBridgeActionResult AiBridgeActions::insertNoteIntoCurrentMidiClip(AiBridgeContext &context,
	int key, int position, int length, int velocity)
{
	if (const auto result = requireMainThread(); !result.success) return result;
	const auto resolved = context.resolve();
	if (resolved.midiClip == nullptr) return AiBridgeActionResult::failure("no_current_midi_clip", "No current MIDI clip");
	if (key < 0 || key >= NumKeys || position < 0 || length <= 0 || velocity < 0 || velocity > 100)
		return AiBridgeActionResult::failure("invalid_parameter", "Note parameters are invalid");
	resolved.midiClip->addNote(Note(TimePos(length), TimePos(position), key, velocity));
	resolved.midiClip->dataChanged();
	return AiBridgeActionResult::ok("MIDI note inserted");
}

AiBridgeContextSnapshot AiBridgeActions::getContextSnapshot()
{
	return AiBridgeContext().snapshot();
}

} // namespace lmms
