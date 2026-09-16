#ifndef LMMS_AIBRIDGE_ARRANGEMENT_CONTEXT_H
#define LMMS_AIBRIDGE_ARRANGEMENT_CONTEXT_H

#include "AiBridgeTimelineTypes.h"

namespace lmms
{

class AiBridgeArrangementContext
{
public:
	static AiBridgeValidationResult resolvePosition(const AiBridgeMusicalPosition &position,
		qint64 ticksPerBar, qint64 ticksPerBeat, qint64 &resolvedTick);

	AiBridgeTimelineClipPage snapshot(int offset, int limit) const;
	AiBridgeTimelineClipPage region(const AiBridgeTimelineRange &range,
		const AiBridgeTrackScope &scope, int offset, int limit) const;
};

} // namespace lmms

#endif
