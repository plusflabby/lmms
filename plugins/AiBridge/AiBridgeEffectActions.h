#ifndef LMMS_AIBRIDGE_EFFECT_ACTIONS_H
#define LMMS_AIBRIDGE_EFFECT_ACTIONS_H
#include "AiBridgeEffectRegistry.h"
#include "AiBridgeMixerContext.h"
#include "AiBridgeMixerTypes.h"
#include "AiBridgeEffectTypes.h"
namespace lmms {
struct AiBridgeEffectActionResult {
	bool success = false;
	QString code;
	QString message;
	QString effectInstanceId;
	QString effectId;
	QString mixerChannelId;
	int position = -1;
	bool enabled = false;
	float wetDry = 0;
	QVector<AiBridgeMixerEffectSummary> effects;
};
struct AiBridgeEffectParameterActionResult
{
	bool success = false;
	QString code;
	QString message;
	QString effectInstanceId;
	AiBridgeEffectParameterPage page;
};
class AiBridgeEffectActions {
public:
	static bool validWetDry(float value);
	static AiBridgeEffectActionResult add(const QString&, const QString&, int);
	static AiBridgeEffectActionResult remove(const QString&);
	static AiBridgeEffectActionResult move(const QString&, int);
	static AiBridgeEffectActionResult setState(const QString&, bool, float);
	static AiBridgeEffectParameterActionResult getParameters(const QString&, int, int);
	static AiBridgeEffectParameterActionResult setParameters(const QString&, const QVector<AiBridgeEffectParameterChange>&);
};
}
#endif
