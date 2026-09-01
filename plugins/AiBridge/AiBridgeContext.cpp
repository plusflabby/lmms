#include "AiBridgeContext.h"

#include <QApplication>
#include <QThread>

#include "Engine.h"
#include "GuiApplication.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "PianoRoll.h"
#include "Song.h"

namespace lmms
{

AiBridgeContext::Resolved AiBridgeContext::resolve() const
{
	Resolved resolved;
	resolved.song = Engine::getSong();

	if (gui::getGUI() != nullptr && gui::getGUI()->pianoRoll() != nullptr)
	{
		resolved.midiClip = gui::getGUI()->pianoRoll()->aiBridgeCurrentMidiClip();
		if (resolved.midiClip != nullptr)
		{
			resolved.instrumentTrack = resolved.midiClip->instrumentTrack();
		}
	}

	return resolved;
}

AiBridgeContextSnapshot AiBridgeContext::snapshot() const
{
	AiBridgeContextSnapshot snapshot;
	if (qApp == nullptr || QThread::currentThread() != qApp->thread())
	{
		snapshot.code = QStringLiteral("wrong_thread");
		snapshot.message = QStringLiteral("LMMS context must be read on the Qt main thread");
		return snapshot;
	}

	const auto resolved = resolve();
	snapshot.success = true;
	snapshot.code = QStringLiteral("ok");
	snapshot.message = QStringLiteral("LMMS context snapshot captured");
	if (resolved.song != nullptr)
	{
		snapshot.songName = resolved.song->displayName();
		snapshot.tempo = resolved.song->getTempo();
	}
	if (resolved.midiClip != nullptr)
	{
		snapshot.hasCurrentMidiClip = true;
		snapshot.clipLengthTicks = resolved.midiClip->length().getTicks();
	}
	if (resolved.instrumentTrack != nullptr)
	{
		snapshot.hasOwningInstrumentTrack = true;
		snapshot.trackName = resolved.instrumentTrack->displayName();
		snapshot.trackVolume = static_cast<int>(resolved.instrumentTrack->getVolume());
	}
	return snapshot;
}

} // namespace lmms
