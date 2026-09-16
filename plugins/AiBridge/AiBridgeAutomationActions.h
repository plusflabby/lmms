#ifndef LMMS_AIBRIDGE_AUTOMATION_ACTIONS_H
#define LMMS_AIBRIDGE_AUTOMATION_ACTIONS_H

#include "AiBridgeAutomationContext.h"
#include "AiBridgeAutomationTypes.h"

namespace lmms
{

struct AiBridgeAutomationActionResult
{
	bool success = false;
	QString code;
	QString message;
	QString automationTrackId;
	QString automationClipId;
	QString automationTargetId;
	QVector<QString> appliedPointIds;
	QVector<QString> unappliedPointIds;
};

class AiBridgeAutomationActions
{
public:
	static AiBridgeAutomationActionResult createTrack(const QString &name = {});
	static AiBridgeAutomationActionResult deleteTrack(const QString &trackId);
	static AiBridgeAutomationActionResult createClip(const QString &trackId, qint64 startTick,
		qint64 lengthTicks, const QString &targetId = {});
	static AiBridgeAutomationActionResult duplicateClip(const QString &clipId,
		const QString &destinationTrackId, qint64 destinationTick);
	static AiBridgeAutomationActionResult deleteClip(const QString &clipId);
	static AiBridgeAutomationActionResult setClipProperties(const QString &clipId,
		const std::optional<qint64> &startTick, const std::optional<qint64> &lengthTicks);
	static AiBridgeAutomationActionResult setClipTarget(const QString &clipId, const QString &targetId);
	static AiBridgeAutomationActionResult editPoints(const QString &clipId,
		const QVector<AiBridgeAutomationPointMutation> &mutations);
};

} // namespace lmms

#endif
