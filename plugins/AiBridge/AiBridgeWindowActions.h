#ifndef LMMS_AIBRIDGE_WINDOW_ACTIONS_H
#define LMMS_AIBRIDGE_WINDOW_ACTIONS_H

#include "AiBridgeActions.h"
#include "AiBridgeWindowContext.h"

namespace lmms
{

class AiBridgeWindowActions
{
public:
	static AiBridgeActionResult focusWindow(AiBridgeWindowContext& context, const QString& id);
	static AiBridgeActionResult setWindowGeometry(AiBridgeWindowContext& context, const QString& id,
		const AiBridgeWindowRect& geometry);
	static AiBridgeActionResult arrangeWindows(AiBridgeWindowContext& context, const QString& strategy);
};

} // namespace lmms

#endif
