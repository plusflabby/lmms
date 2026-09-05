#ifndef LMMS_AIBRIDGE_METER_ACTIONS_H
#define LMMS_AIBRIDGE_METER_ACTIONS_H

#include "AiBridgeMixerTypes.h"
#include <optional>

namespace lmms
{
struct AiBridgeMeterActionResult
{
	bool success = false;
	QString code;
	QString message;
	AiBridgeMixerMeterPage page;
};

class AiBridgeMeterActions
{
public:
	static AiBridgeMeterActionResult get(const std::optional<QVector<QString>>& channelIds,
		int offset, int limit);
};
}

#endif
