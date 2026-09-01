#ifndef LMMS_AIBRIDGE_CONTEXT_H
#define LMMS_AIBRIDGE_CONTEXT_H

#include <QString>

namespace lmms
{

class InstrumentTrack;
class MidiClip;
class Song;

struct AiBridgeContextSnapshot
{
	bool success = false;
	QString code;
	QString message;
	QString songName;
	int tempo = 0;
	bool hasCurrentMidiClip = false;
	int clipLengthTicks = 0;
	bool hasOwningInstrumentTrack = false;
	QString trackName;
	int trackVolume = 0;
};

class AiBridgeContext
{
public:
	AiBridgeContext() = default;
	AiBridgeContextSnapshot snapshot() const;

private:
	struct Resolved
	{
		Song *song = nullptr;
		InstrumentTrack *instrumentTrack = nullptr;
		MidiClip *midiClip = nullptr;
	};

	Resolved resolve() const;

	friend class AiBridgeActions;
};

} // namespace lmms

#endif // LMMS_AIBRIDGE_CONTEXT_H
