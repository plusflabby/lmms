#ifndef LMMS_AIBRIDGE_TIMELINE_ACTIONS_H
#define LMMS_AIBRIDGE_TIMELINE_ACTIONS_H

#include <optional>

#include "AiBridgeProjectContext.h"
#include "AiBridgeTimelineTypes.h"

namespace lmms
{

struct AiBridgeTimelineEdit
{
	QString clipId;
	std::optional<qint64> startTick;
	std::optional<qint64> lengthTicks;
	std::optional<bool> autoResize;
};

struct AiBridgeTimelineActionResult
{
	bool success = false;
	QString code;
	QString message;
	QString clipId;
	QString trackId;
	QVector<QString> appliedClipIds;
	QVector<QString> unappliedClipIds;
};

class AiBridgeTimelineActions
{
public:
	static AiBridgeValidationResult validateEdit(const AiBridgeTimelineEdit &edit);
	static AiBridgeTimelineActionResult setClip(const AiBridgeTimelineEdit &edit);
	static AiBridgeTimelineActionResult duplicateClip(const QString &clipId, qint64 destinationTick);
	static AiBridgeTimelineActionResult deleteClip(const QString &clipId);
	static AiBridgeTimelineActionResult batchEdit(const QVector<AiBridgeTimelineEdit> &edits);
};

} // namespace lmms

#endif
