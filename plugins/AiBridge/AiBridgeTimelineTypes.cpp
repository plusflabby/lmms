#include "AiBridgeTimelineTypes.h"

#include <QSet>

namespace lmms
{

namespace
{
AiBridgeValidationResult invalid(const QString &code, const QString &message)
{
	return {code, message};
}
}

namespace AiBridgeTimelineLimits
{

bool validPage(int offset, int limit)
{
	return offset >= 0 && limit > 0 && limit <= maxTimelineClipsPerPage;
}

} // namespace AiBridgeTimelineLimits

AiBridgeValidationResult AiBridgeMusicalPosition::validate() const
{
	const bool hasTick = tick.has_value();
	const bool hasConvenience = bar.has_value() || beat.has_value() || tickInBeat.has_value();
	if (hasTick && hasConvenience)
	{
		return invalid(QStringLiteral("position_representations_conflict"),
			QStringLiteral("tick cannot be combined with bar, beat, or tick_in_beat"));
	}
	if (hasTick)
	{
		return *tick >= 0
			? AiBridgeValidationResult{}
			: invalid(QStringLiteral("invalid_tick"), QStringLiteral("tick must be non-negative"));
	}
	if (!hasConvenience || !bar.has_value() || !beat.has_value() || !tickInBeat.has_value())
	{
		return invalid(QStringLiteral("invalid_position"),
			QStringLiteral("provide either tick or bar, beat, and tick_in_beat"));
	}
	if (*bar < 1 || *beat < 1 || *tickInBeat < 0)
	{
		return invalid(QStringLiteral("invalid_position"),
			QStringLiteral("bar and beat must be positive and tick_in_beat non-negative"));
	}
	return {};
}

AiBridgeValidationResult AiBridgeTimelineRange::validate() const
{
	if (startTick < 0 || endTick < 0 || endTick <= startTick)
	{
		return invalid(QStringLiteral("invalid_tick_range"),
			QStringLiteral("range must satisfy 0 <= start_tick < end_tick"));
	}
	return {};
}

bool AiBridgeTimelineRange::contains(qint64 value) const
{
	return value >= startTick && value < endTick;
}

AiBridgeValidationResult AiBridgeTrackScope::validate() const
{
	if (mode == QStringLiteral("all_tracks"))
	{
		return trackIds.isEmpty() ? AiBridgeValidationResult{} : invalid(
			QStringLiteral("invalid_scope"), QStringLiteral("all_tracks cannot include track_ids"));
	}
	if (mode != QStringLiteral("track_ids") || trackIds.isEmpty())
	{
		return invalid(QStringLiteral("invalid_scope"),
			QStringLiteral("scope must be all_tracks or a non-empty track_ids scope"));
	}
	QSet<QString> unique;
	for (const auto &trackId : trackIds)
	{
		if (trackId.isEmpty())
		{
			return invalid(QStringLiteral("invalid_scope"), QStringLiteral("track IDs cannot be empty"));
		}
		if (unique.contains(trackId))
		{
			return invalid(QStringLiteral("duplicate_track_id"),
				QStringLiteral("track_ids must be unique"));
		}
		unique.insert(trackId);
	}
	return {};
}

} // namespace lmms
