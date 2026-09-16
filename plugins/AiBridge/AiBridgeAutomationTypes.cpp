#include "AiBridgeAutomationTypes.h"

#include <cmath>

namespace lmms
{

namespace AiBridgeAutomationLimits
{

bool validTrackPage(int offset, int limit)
{
	return offset >= 0 && limit > 0 && limit <= maxAutomationTracksPerPage;
}

bool validClipPage(int offset, int limit)
{
	return offset >= 0 && limit > 0 && limit <= maxAutomationClipsPerPage;
}

bool validPointPage(int offset, int limit)
{
	return offset >= 0 && limit > 0 && limit <= maxAutomationPointsPerPage;
}

bool validPointChangeCount(int count)
{
	return count >= 0 && count <= maxPointChanges;
}

} // namespace AiBridgeAutomationLimits

AiBridgeValidationResult AiBridgeAutomationPointValue::validate(float value)
{
	if (!std::isfinite(value))
	{
		return {QStringLiteral("invalid_automation_value"),
			QStringLiteral("automation values must be finite")};
	}
	return {};
}

namespace AiBridgeAutomationCurve
{

bool isSupported(const QString &curve)
{
	return curve == QStringLiteral("discrete")
		|| curve == QStringLiteral("linear")
		|| curve == QStringLiteral("cubic_hermite");
}

} // namespace AiBridgeAutomationCurve

} // namespace lmms
