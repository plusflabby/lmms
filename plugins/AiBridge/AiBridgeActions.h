#ifndef LMMS_AIBRIDGE_ACTIONS_H
#define LMMS_AIBRIDGE_ACTIONS_H

#include <QString>
#include <QVariant>
#include <optional>

namespace lmms
{

class InstrumentTrack;
class MidiClip;
class Song;

struct AiBridgeActionResult
{
	bool success = false;
	QString code;
	QString message;
	std::optional<QVariant> value;

	static AiBridgeActionResult ok(const QString &message)
	{
		return {true, QStringLiteral("ok"), message, std::nullopt};
	}

	static AiBridgeActionResult failure(const QString &code, const QString &message)
	{
		return {false, code, message, std::nullopt};
	}
};

class AiBridgeContext
{
public:
	AiBridgeContext();

	Song *song() const { return m_song; }
	InstrumentTrack *instrumentTrack() const { return m_instrumentTrack; }
	MidiClip *midiClip() const { return m_midiClip; }
	bool hasSong() const { return m_song != nullptr; }
	bool hasMidiClip() const { return m_midiClip != nullptr; }

private:
	Song *m_song = nullptr;
	InstrumentTrack *m_instrumentTrack = nullptr;
	MidiClip *m_midiClip = nullptr;
};

class AiBridgeActions
{
public:
	static AiBridgeActionResult setSongTempo(AiBridgeContext &context, int bpm);
	static AiBridgeActionResult adjustCurrentInstrumentVolume(AiBridgeContext &context,
		float delta);
	static AiBridgeActionResult insertNoteIntoCurrentMidiClip(AiBridgeContext &context,
		int key, int position, int length, int velocity);
};

} // namespace lmms

#endif // LMMS_AIBRIDGE_ACTIONS_H
