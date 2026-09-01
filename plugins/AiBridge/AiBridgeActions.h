#ifndef LMMS_AIBRIDGE_ACTIONS_H
#define LMMS_AIBRIDGE_ACTIONS_H

#include <QString>
#include <QVariant>
#include <optional>

#include "AiBridgeContext.h"

namespace lmms
{

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

class AiBridgeActions
{
public:
	static AiBridgeContextSnapshot getContextSnapshot();
	static AiBridgeActionResult setSongTempo(AiBridgeContext &context, int bpm);
	static AiBridgeActionResult adjustCurrentInstrumentVolume(AiBridgeContext &context,
		float delta);
	static AiBridgeActionResult insertNoteIntoCurrentMidiClip(AiBridgeContext &context,
		int key, int position, int length, int velocity);
};

} // namespace lmms

#endif // LMMS_AIBRIDGE_ACTIONS_H
