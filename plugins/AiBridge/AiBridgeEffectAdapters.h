#ifndef LMMS_AIBRIDGE_EFFECT_ADAPTERS_H
#define LMMS_AIBRIDGE_EFFECT_ADAPTERS_H

#include "AiBridgeEffectTypes.h"

namespace lmms
{
class EffectControls;

class AiBridgeEffectAdapters
{
public:
	static QVector<AiBridgeEffectParameterMetadata> parameters(const QString& effectId,
		EffectControls* controls);
	static bool setParameter(const QString& effectId, EffectControls* controls,
		const QString& parameterId, float value);
	static bool validateBatch(const QVector<AiBridgeEffectParameterMetadata>& parameters,
		const QVector<AiBridgeEffectParameterChange>& changes, QString* error);
};
}

#endif
