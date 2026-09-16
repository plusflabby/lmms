#include "AiBridgeAutomationContext.h"

#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "Engine.h"
#include "Song.h"
#include "TrackContainer.h"

#include <algorithm>

namespace lmms
{

namespace
{
template<typename T>
QVector<T> pageItems(const QVector<T> &all, int offset, int limit, AiBridgePageMetadata &page)
{
	page.total = all.size();
	page.offset = qBound(0, offset, page.total);
	page.limit = limit;
	page.hasMore = page.offset + limit < page.total;
	return all.mid(page.offset, limit);
}

QString curveName(AutomationClip::ProgressionType type)
{
	switch (type)
	{
	case AutomationClip::ProgressionType::Linear: return QStringLiteral("linear");
	case AutomationClip::ProgressionType::CubicHermite: return QStringLiteral("cubic_hermite");
	default: return QStringLiteral("discrete");
	}
}

QVector<AutomationTrack *> nativeTracks()
{
	QVector<AutomationTrack *> result;
	if (auto *song = Engine::getSong())
	{
		for (auto *track : song->tracks())
		{
			if (auto *automation = dynamic_cast<AutomationTrack *>(track)) result.append(automation);
		}
	}
	return result;
}

AiBridgeAutomationClipSnapshot copyClip(AiBridgeAutomationRegistry &registry, AiBridgeAutomationTargets &targets,
		AutomationTrack *track, AutomationClip *clip, const QString &trackId)
{
	AiBridgeAutomationClipSnapshot result;
	result.automationTrackId = trackId;
	result.automationClipId = registry.automationClipId(clip, trackId);
	result.startTick = clip->startPosition().getTicks();
	result.lengthTicks = clip->length().getTicks();
	result.curve = curveName(clip->progressionType());
	if (const auto *object = clip->firstObject())
	{
		for (const auto &target : targets.all())
			if (targets.bindingFor(target.automationTargetId) == static_cast<QObject *>(const_cast<AutomatableModel *>(object)))
			{
				result.automationTargetId = target.automationTargetId;
				break;
			}
	}
	for (auto it = clip->getTimeMap().cbegin(); it != clip->getTimeMap().cend(); ++it)
	{
		AiBridgeAutomationPointSnapshot point;
		point.pointId = QStringLiteral("%1:%2").arg(result.automationClipId).arg(it.key());
		point.tick = it.key();
		point.value = it.value().getInValue();
		point.curve = result.curve;
		result.points.append(point);
	}
	return result;
}
}

AiBridgeAutomationContext::AiBridgeAutomationContext(AiBridgeAutomationRegistry &registry,
	AiBridgeAutomationTargets &targets) :
	m_registry(registry),
	m_targets(targets)
{
}

QVector<AiBridgeAutomationTargetSnapshot> AiBridgeAutomationContext::listTargets(int offset, int limit) const
{
	auto all = m_targets.all();
	std::sort(all.begin(), all.end(), [](const auto &left, const auto &right) {
		return left.automationTargetId < right.automationTargetId;
	});
	AiBridgePageMetadata page;
	return pageItems(all, offset, limit, page);
}

QVector<AiBridgeAutomationTargetSnapshot> AiBridgeAutomationContext::capabilities(int offset, int limit) const
{
	return listTargets(offset, limit);
}

AiBridgeAutomationTrackPage AiBridgeAutomationContext::listTracks(int offset, int limit) const
{
	AiBridgeAutomationTrackPage page;
	QVector<AiBridgeAutomationTrackSnapshot> all;
	for (auto *track : nativeTracks())
	{
		AiBridgeAutomationTrackSnapshot item;
		item.automationTrackId = m_registry.automationTrackId(track);
		item.name = track->name();
		item.index = 0;
		if (auto *song = Engine::getSong())
		{
			const auto &tracks = song->tracks();
			const auto it = std::find(tracks.cbegin(), tracks.cend(), track);
			if (it != tracks.cend()) item.index = static_cast<int>(std::distance(tracks.cbegin(), it));
		}
		for (auto *clip : track->getClips())
		{
			if (auto *automationClip = dynamic_cast<AutomationClip *>(clip))
			{
				item.automationClipIds.append(m_registry.automationClipId(automationClip, item.automationTrackId));
			}
		}
		all.append(item);
	}
	page.items = pageItems(all, offset, limit, page.page);
	return page;
}

AiBridgeAutomationClipPage AiBridgeAutomationContext::listClips(int offset, int limit) const
{
	AiBridgeAutomationClipPage page;
	QVector<AiBridgeAutomationClipSnapshot> all;
	for (auto *track : nativeTracks())
	{
		const auto trackId = m_registry.automationTrackId(track);
		for (auto *clip : track->getClips())
		{
			if (auto *automationClip = dynamic_cast<AutomationClip *>(clip))
			{
				all.append(copyClip(m_registry, m_targets, track, automationClip, trackId));
			}
		}
	}
	page.items = pageItems(all, offset, limit, page.page);
	return page;
}

bool AiBridgeAutomationContext::getClip(const QString &clipId, int offset, int limit,
	AiBridgeAutomationClipSnapshot &snapshot, AiBridgeAutomationPointPage &points) const
{
	if (!AiBridgeAutomationLimits::validPointPage(offset, limit)) return false;
	auto *object = m_registry.resolveClipToken(clipId);
	auto *clip = dynamic_cast<AutomationClip *>(object);
	if (!clip) return false;
	auto *track = dynamic_cast<AutomationTrack *>(clip->getTrack());
	if (!track) return false;
	const auto trackId = m_registry.automationTrackId(track);
	snapshot = copyClip(m_registry, m_targets, track, clip, trackId);
	points.items = pageItems(snapshot.points, offset, limit, points.page);
	return true;
}

} // namespace lmms
