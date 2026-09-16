#ifndef LMMS_AIBRIDGE_ARRANGEMENT_PLANNER_H
#define LMMS_AIBRIDGE_ARRANGEMENT_PLANNER_H

#include "AiBridgeTimelineTypes.h"

namespace lmms
{

class AiBridgeArrangementPlanner
{
public:
	static AiBridgeValidationResult validate(const AiBridgeArrangementRequest &request);
	static AiBridgeArrangementPlan plan(const AiBridgeArrangementRequest &request,
		const QVector<AiBridgeTimelineClipSnapshot> &clips);
	static AiBridgeArrangementPlan plan(const AiBridgeArrangementRequest &request,
		const AiBridgeArrangementSnapshot &snapshot);
};

} // namespace lmms

#endif
