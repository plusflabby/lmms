#ifndef LMMS_AIBRIDGE_EFFECT_TYPES_H
#define LMMS_AIBRIDGE_EFFECT_TYPES_H
#include "AiBridgeMixerTypes.h"
#include <QStringList>
namespace lmms {
struct AiBridgeEffectValue { QString id; QString name; QString description; };
struct AiBridgeEffectPage { AiBridgeMixerPage page; QVector<AiBridgeEffectValue> items; };

enum class AiBridgeEffectParameterType { Float, Integer, Boolean, Enum };
struct AiBridgeEffectParameterMetadata
{
	QString id;
	QString name;
	AiBridgeEffectParameterType type = AiBridgeEffectParameterType::Float;
	float value = 0.0f;
	float minimum = 0.0f;
	float maximum = 0.0f;
	float defaultValue = 0.0f;
	float step = 0.0f;
	QString unit;
	QStringList enumValues;
	bool writable = true;
};

struct AiBridgeEffectParameterChange
{
	QString id;
	AiBridgeEffectParameterType type = AiBridgeEffectParameterType::Float;
	float value = 0.0f;
};

struct AiBridgeEffectParameterPage
{
	AiBridgeMixerPage page;
	QVector<AiBridgeEffectParameterMetadata> items;
};
}
#endif
