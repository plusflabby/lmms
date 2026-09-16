#include "AiBridgeArrangementContext.h"

#include "AiBridgeProjectContext.h"

#include "AutomationClip.h"
#include "Clip.h"
#include "Engine.h"
#include "Song.h"
#include "TimePos.h"
#include "Track.h"

#include <QSet>

#include <limits>

namespace lmms
{

namespace
{
AiBridgeValidationResult invalid(const QString &code, const QString &message)
{
	return {code, message};
}

bool inScope(const QString &trackId, const AiBridgeTrackScope &scope)
{
	return scope.mode == QStringLiteral("all_tracks") || scope.trackIds.contains(trackId);
}

AiBridgeTimelineClipSnapshot copyClip(AiBridgeProjectContext &context, Track *track, Clip *clip)
{
	AiBridgeTimelineClipSnapshot snapshot;
	snapshot.clipId = context.idForClip(clip);
	snapshot.trackId = context.idForTrack(track);
	snapshot.type = dynamic_cast<AutomationClip *>(clip)
		? QStringLiteral("automation") : QStringLiteral("clip");
	snapshot.name = clip->displayName();
	snapshot.startTick = clip->startPosition().getTicks();
	snapshot.endTick = snapshot.startTick + clip->length().getTicks();
	return snapshot;
}
}

AiBridgeValidationResult AiBridgeArrangementContext::resolvePosition(
	const AiBridgeMusicalPosition &position, qint64 ticksPerBar, qint64 ticksPerBeat,
	qint64 &resolvedTick)
{
	const auto validation = position.validate();
	if (!validation.success())
	{
		return validation;
	}
	if (ticksPerBar <= 0 || ticksPerBeat <= 0 || ticksPerBar % ticksPerBeat != 0)
	{
		return invalid(QStringLiteral("invalid_time_signature"),
			QStringLiteral("native ticks-per-bar and ticks-per-beat are invalid"));
	}
	if (position.tick.has_value())
	{
		resolvedTick = *position.tick;
		return {};
	}
	const qint64 beatsPerBar = ticksPerBar / ticksPerBeat;
	if (*position.beat > beatsPerBar || *position.tickInBeat >= ticksPerBeat)
	{
		return invalid(QStringLiteral("invalid_position"),
			QStringLiteral("beat or tick_in_beat is outside the current time signature"));
	}
	const qint64 barOffset = static_cast<qint64>(*position.bar - 1) * ticksPerBar;
	const qint64 beatOffset = static_cast<qint64>(*position.beat - 1) * ticksPerBeat;
	if (barOffset > std::numeric_limits<qint64>::max() - beatOffset - *position.tickInBeat)
	{
		return invalid(QStringLiteral("invalid_position"), QStringLiteral("position overflows tick range"));
	}
	resolvedTick = barOffset + beatOffset + *position.tickInBeat;
	return {};
}

AiBridgeTimelineClipPage AiBridgeArrangementContext::snapshot(int offset, int limit) const
{
	AiBridgeTimelineClipPage page;
	if (!AiBridgeTimelineLimits::validPage(offset, limit))
	{
		return page;
	}
	QVector<AiBridgeTimelineClipSnapshot> all;
	AiBridgeProjectContext context;
	if (auto *song = Engine::getSong())
	{
		for (auto *track : song->tracks())
		{
			for (auto *clip : track->getClips())
			{
				all.append(copyClip(context, track, clip));
			}
		}
	}
	page.page.total = all.size();
	page.page.offset = qBound(0, offset, page.page.total);
	page.page.limit = limit;
	page.page.hasMore = page.page.offset + limit < page.page.total;
	page.items = all.mid(page.page.offset, limit);
	return page;
}

AiBridgeTimelineClipPage AiBridgeArrangementContext::region(
	const AiBridgeTimelineRange &range, const AiBridgeTrackScope &scope, int offset, int limit) const
{
	AiBridgeTimelineClipPage page;
	if (!range.validate().success() || !scope.validate().success()
		|| !AiBridgeTimelineLimits::validPage(offset, limit))
	{
		return page;
	}
	QVector<AiBridgeTimelineClipSnapshot> all;
	AiBridgeProjectContext context;
	if (auto *song = Engine::getSong())
	{
		for (auto *track : song->tracks())
		{
			const auto trackId = context.idForTrack(track);
			if (!inScope(trackId, scope))
			{
				continue;
			}
			for (auto *clip : track->getClips())
			{
				const auto copied = copyClip(context, track, clip);
				if (copied.startTick < range.endTick && copied.endTick > range.startTick)
				{
					all.append(copied);
				}
			}
		}
	}
	page.page.total = all.size();
	page.page.offset = qBound(0, offset, page.page.total);
	page.page.limit = limit;
	page.page.hasMore = page.page.offset + limit < page.page.total;
	page.items = all.mid(page.page.offset, limit);
	return page;
}

} // namespace lmms
