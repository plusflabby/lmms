#ifndef LMMS_AIBRIDGE_AUTOMATION_TYPES_H
#define LMMS_AIBRIDGE_AUTOMATION_TYPES_H

#include "AiBridgeTimelineTypes.h"

#include <QVector>
#include <QString>

#include <optional>

namespace lmms
{

namespace AiBridgeAutomationLimits
{
constexpr int maxAutomationTracksPerPage = 128;
constexpr int maxAutomationClipsPerPage = 512;
constexpr int maxAutomationPointsPerPage = 1024;
constexpr int maxPointChanges = 512;

bool validTrackPage(int offset, int limit);
bool validClipPage(int offset, int limit);
bool validPointPage(int offset, int limit);
bool validPointChangeCount(int count);
}

struct AiBridgeAutomationPointValue
{
	static AiBridgeValidationResult validate(float value);
};

namespace AiBridgeAutomationCurve
{
bool isSupported(const QString &curve);
}

struct AiBridgeAutomationPointSnapshot
{
	QString pointId;
	qint64 tick = 0;
	float value = 0.0f;
	QString curve = QStringLiteral("discrete");
};

struct AiBridgeAutomationTargetSnapshot
{
	QString automationTargetId;
	QString semanticClass;
	QString ownerId;
	QString parameterId;
	QString valueType;
	float minimum = 0.0f;
	float maximum = 1.0f;
	float defaultValue = 0.0f;
	float step = 0.01f;
	QString unit;
	QString capability;
};

struct AiBridgeAutomationTrackSnapshot
{
	QString automationTrackId;
	QString name;
	int index = 0;
	QVector<QString> automationClipIds;
};

struct AiBridgeAutomationClipSnapshot
{
	QString automationClipId;
	QString automationTrackId;
	QString automationTargetId;
	qint64 startTick = 0;
	qint64 lengthTicks = 0;
	QString curve;
	QVector<AiBridgeAutomationPointSnapshot> points;
};

struct AiBridgeAutomationTrackPage
{
	AiBridgePageMetadata page;
	QVector<AiBridgeAutomationTrackSnapshot> items;
};

struct AiBridgeAutomationClipPage
{
	AiBridgePageMetadata page;
	QVector<AiBridgeAutomationClipSnapshot> items;
};

struct AiBridgeAutomationPointPage
{
	AiBridgePageMetadata page;
	QVector<AiBridgeAutomationPointSnapshot> items;
};

struct AiBridgeAutomationPointMutation
{
	enum class Kind { Add, Update, Delete };
	Kind kind = Kind::Add;
	QString pointId;
	std::optional<qint64> tick;
	std::optional<float> value;
	std::optional<QString> curve;
};

} // namespace lmms

#endif
