#include "AiBridgeAutomationActions.h"

#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "Engine.h"
#include "Song.h"
#include "Track.h"

#include <limits>
#include <QSet>
#include <cmath>

namespace lmms
{

namespace
{
AiBridgeAutomationActionResult fail(const QString &code, const QString &message)
{
	return {false, code, message};
}

AiBridgeAutomationActionResult ok(const QString &message)
{
	AiBridgeAutomationActionResult result;
	result.success = true;
	result.code = QStringLiteral("ok");
	result.message = message;
	return result;
}

bool validRange(qint64 startTick, qint64 lengthTicks)
{
	return startTick >= 0 && lengthTicks > 0
		&& startTick <= std::numeric_limits<qint32>::max()
		&& lengthTicks <= std::numeric_limits<qint32>::max()
		&& startTick <= std::numeric_limits<qint32>::max() - lengthTicks;
}
}

AiBridgeAutomationActionResult AiBridgeAutomationActions::createTrack(const QString &name)
{
	if (!Engine::getSong()) return fail(QStringLiteral("project_unavailable"), QStringLiteral("no project is loaded"));
	auto *track = dynamic_cast<AutomationTrack *>(Track::create(Track::Type::Automation, Engine::getSong()));
	if (!track) return fail(QStringLiteral("track_unavailable"), QStringLiteral("automation track could not be created"));
	if (!name.isEmpty()) track->setName(name);
	const auto id = aiBridgeAutomationRegistry().automationTrackId(track);
	Engine::getSong()->setModified();
	auto result = ok(QStringLiteral("automation track created"));
	result.automationTrackId = id;
	return result;
}

AiBridgeAutomationActionResult AiBridgeAutomationActions::deleteTrack(const QString &trackId)
{
	auto *token = aiBridgeAutomationRegistry().resolveTrackToken(trackId);
	auto *track = dynamic_cast<AutomationTrack *>(token);
	if (!track) return fail(QStringLiteral("stale_automation_track"), QStringLiteral("automation track is not live"));
	aiBridgeAutomationRegistry().retireTrack(trackId);
	delete track;
	Engine::getSong()->setModified();
	auto result = ok(QStringLiteral("automation track deleted"));
	result.automationTrackId = trackId;
	return result;
}

AiBridgeAutomationActionResult AiBridgeAutomationActions::createClip(const QString &trackId,
	qint64 startTick, qint64 lengthTicks, const QString &targetId)
{
	if (!validRange(startTick, lengthTicks))
		return fail(QStringLiteral("invalid_automation_range"), QStringLiteral("automation range is outside the LMMS tick range"));
	auto *track = dynamic_cast<AutomationTrack *>(aiBridgeAutomationRegistry().resolveTrackToken(trackId));
	if (!track) return fail(QStringLiteral("stale_automation_track"), QStringLiteral("automation track is not live"));
	if (!targetId.isEmpty() && !aiBridgeAutomationTargets().isSupported(targetId))
		return fail(QStringLiteral("stale_automation_target"), QStringLiteral("automation target is unsupported or stale"));
	auto *clip = dynamic_cast<AutomationClip *>(track->createClip(TimePos(static_cast<tick_t>(startTick))));
	if (!clip) return fail(QStringLiteral("clip_unavailable"), QStringLiteral("automation clip could not be created"));
	clip->changeLength(TimePos(static_cast<tick_t>(lengthTicks)));
	const auto clipId = aiBridgeAutomationRegistry().automationClipId(clip, trackId);
	Engine::getSong()->setModified();
	auto result = ok(QStringLiteral("automation clip created"));
	result.automationTrackId = trackId;
	result.automationClipId = clipId;
	result.automationTargetId = targetId;
	return result;
}

AiBridgeAutomationActionResult AiBridgeAutomationActions::duplicateClip(const QString &clipId,
	const QString &destinationTrackId, qint64 destinationTick)
{
	auto *source = dynamic_cast<AutomationClip *>(aiBridgeAutomationRegistry().resolveClipToken(clipId));
	if (!source) return fail(QStringLiteral("stale_automation_clip"), QStringLiteral("automation clip is not live"));
	auto *destination = dynamic_cast<AutomationTrack *>(aiBridgeAutomationRegistry().resolveTrackToken(destinationTrackId));
	if (!destination) return fail(QStringLiteral("stale_automation_track"), QStringLiteral("automation destination track is not live"));
	const auto length = source->length().getTicks();
	if (!validRange(destinationTick, length))
		return fail(QStringLiteral("invalid_automation_range"), QStringLiteral("duplicated automation range is outside the LMMS tick range"));
	auto *copy = dynamic_cast<AutomationClip *>(destination->createClip(TimePos(static_cast<tick_t>(destinationTick))));
	if (!copy) return fail(QStringLiteral("clip_unavailable"), QStringLiteral("automation clip could not be duplicated"));
	copy->changeLength(TimePos(static_cast<tick_t>(length)));
	for (const auto &object : source->objects())
	{
		if (object && !copy->addObject(object, false))
		{
			delete copy;
			return fail(QStringLiteral("automation_target_unavailable"), QStringLiteral("automation target could not be copied"));
		}
	}
	copy->setProgressionType(source->progressionType());
	for (auto it = source->getTimeMap().cbegin(); it != source->getTimeMap().cend(); ++it)
		copy->putValue(TimePos(it.key()), it.value().getInValue(), false, false);
	const auto trackId = aiBridgeAutomationRegistry().automationTrackId(destination);
	auto result = ok(QStringLiteral("automation clip duplicated"));
	result.automationTrackId = trackId;
	result.automationClipId = aiBridgeAutomationRegistry().automationClipId(copy, trackId);
	if (const auto *target = source->firstObject())
	{
		for (const auto &candidate : aiBridgeAutomationTargets().all())
			if (aiBridgeAutomationTargets().bindingFor(candidate.automationTargetId) == target)
			{
				result.automationTargetId = candidate.automationTargetId;
				break;
		}
	}
	Engine::getSong()->setModified();
	return result;
}

AiBridgeAutomationActionResult AiBridgeAutomationActions::deleteClip(const QString &clipId)
{
	auto *clip = dynamic_cast<AutomationClip *>(aiBridgeAutomationRegistry().resolveClipToken(clipId));
	if (!clip) return fail(QStringLiteral("stale_automation_clip"), QStringLiteral("automation clip is not live"));
	aiBridgeAutomationRegistry().retireClip(clipId);
	delete clip;
	Engine::getSong()->setModified();
	auto result = ok(QStringLiteral("automation clip deleted"));
	result.automationClipId = clipId;
	return result;
}

AiBridgeAutomationActionResult AiBridgeAutomationActions::setClipProperties(const QString &clipId,
	const std::optional<qint64> &startTick, const std::optional<qint64> &lengthTicks)
{
	if (!startTick && !lengthTicks) return fail(QStringLiteral("invalid_parameter"), QStringLiteral("a clip property is required"));
	auto *clip = dynamic_cast<AutomationClip *>(aiBridgeAutomationRegistry().resolveClipToken(clipId));
	if (!clip) return fail(QStringLiteral("stale_automation_clip"), QStringLiteral("automation clip is not live"));
	const auto start = startTick.value_or(clip->startPosition().getTicks());
	const auto length = lengthTicks.value_or(clip->length().getTicks());
	if (!validRange(start, length)) return fail(QStringLiteral("invalid_automation_range"), QStringLiteral("automation range is invalid"));
	clip->addJournalCheckPoint();
	if (startTick) clip->movePosition(TimePos(static_cast<tick_t>(start)));
	if (lengthTicks) clip->changeLength(TimePos(static_cast<tick_t>(length)));
	Engine::getSong()->setModified();
	auto result = ok(QStringLiteral("automation clip updated"));
	result.automationClipId = clipId;
	return result;
}

AiBridgeAutomationActionResult AiBridgeAutomationActions::setClipTarget(const QString &clipId, const QString &targetId)
{
	auto *clip = dynamic_cast<AutomationClip *>(aiBridgeAutomationRegistry().resolveClipToken(clipId));
	if (!clip) return fail(QStringLiteral("stale_automation_clip"), QStringLiteral("automation clip is not live"));
	if (!aiBridgeAutomationTargets().isSupported(targetId))
		return fail(QStringLiteral("stale_automation_target"), QStringLiteral("automation target is unsupported or stale"));
	auto *binding = dynamic_cast<AutomatableModel *>(aiBridgeAutomationTargets().bindingFor(targetId));
	if (!binding) return fail(QStringLiteral("automation_target_unbound"), QStringLiteral("automation target has no native binding"));
	if (!clip->addObject(binding)) return fail(QStringLiteral("automation_target_duplicate"), QStringLiteral("target is already bound"));
	Engine::getSong()->setModified();
	auto result = ok(QStringLiteral("automation clip target updated"));
	result.automationClipId = clipId;
	result.automationTargetId = targetId;
	return result;
}

AiBridgeAutomationActionResult AiBridgeAutomationActions::editPoints(const QString &clipId,
	const QVector<AiBridgeAutomationPointMutation> &mutations)
{
	if (mutations.isEmpty()) return fail(QStringLiteral("invalid_parameter"), QStringLiteral("point mutations are required"));
	auto *clip = dynamic_cast<AutomationClip *>(aiBridgeAutomationRegistry().resolveClipToken(clipId));
	if (!clip) return fail(QStringLiteral("stale_automation_clip"), QStringLiteral("automation clip is not live"));
	QSet<QString> ids;
	QSet<int> resultingTicks;
	for (const auto &mutation : mutations)
	{
		if (!mutation.pointId.isEmpty() && ids.contains(mutation.pointId))
			return fail(QStringLiteral("duplicate_point"), QStringLiteral("a point may appear only once"));
		if (!mutation.pointId.isEmpty()) ids.insert(mutation.pointId);
		if (mutation.curve && !AiBridgeAutomationCurve::isSupported(*mutation.curve))
			return fail(QStringLiteral("unsupported_curve"), QStringLiteral("curve is not supported by LMMS"));
		if (mutation.value && !std::isfinite(*mutation.value))
			return fail(QStringLiteral("invalid_value"), QStringLiteral("automation value must be finite"));
		if (mutation.tick && (*mutation.tick < 0 || *mutation.tick > std::numeric_limits<qint32>::max()))
			return fail(QStringLiteral("invalid_point_tick"), QStringLiteral("point tick is outside the LMMS range"));
		if (mutation.kind == AiBridgeAutomationPointMutation::Kind::Add)
		{
			if (!mutation.tick || !mutation.value)
				return fail(QStringLiteral("invalid_point"), QStringLiteral("add requires tick and value"));
			if (clip->getTimeMap().contains(TimePos(static_cast<tick_t>(*mutation.tick))) || resultingTicks.contains(static_cast<int>(*mutation.tick)))
				return fail(QStringLiteral("duplicate_point_tick"), QStringLiteral("point tick already exists"));
			resultingTicks.insert(static_cast<int>(*mutation.tick));
		}
		else
		{
			if (mutation.pointId.isEmpty()) return fail(QStringLiteral("invalid_point"), QStringLiteral("update/delete requires point_id"));
			const auto prefix = clipId + QStringLiteral(":");
			if (!mutation.pointId.startsWith(prefix)) return fail(QStringLiteral("stale_point"), QStringLiteral("point is not owned by this clip"));
			bool parsed = false;
			const auto oldTick = mutation.pointId.mid(prefix.size()).toInt(&parsed);
			if (!parsed || !clip->getTimeMap().contains(TimePos(oldTick)))
				return fail(QStringLiteral("stale_point"), QStringLiteral("point is not live"));
			if (mutation.kind == AiBridgeAutomationPointMutation::Kind::Update && mutation.tick
				&& (*mutation.tick != oldTick) && (clip->getTimeMap().contains(TimePos(static_cast<tick_t>(*mutation.tick))) || resultingTicks.contains(static_cast<int>(*mutation.tick))))
				return fail(QStringLiteral("duplicate_point_tick"), QStringLiteral("updated point tick already exists"));
			if (mutation.kind == AiBridgeAutomationPointMutation::Kind::Update && mutation.tick)
				resultingTicks.insert(static_cast<int>(*mutation.tick));
		}
	}
	for (const auto &mutation : mutations)
	{
		QString pointId = mutation.pointId;
		int oldTick = -1;
		if (!pointId.isEmpty()) oldTick = pointId.mid(clipId.size() + 1).toInt();
		if (mutation.kind == AiBridgeAutomationPointMutation::Kind::Add)
		{
			clip->putValue(TimePos(static_cast<tick_t>(*mutation.tick)), *mutation.value, false, false);
			pointId = QStringLiteral("%1:%2").arg(clipId).arg(*mutation.tick);
		}
		else if (mutation.kind == AiBridgeAutomationPointMutation::Kind::Delete)
		{
			clip->removeNode(TimePos(oldTick));
		}
		else
		{
			const auto tick = mutation.tick.value_or(oldTick);
			const auto node = clip->getTimeMap().constFind(oldTick);
			if (node == clip->getTimeMap().cend())
				return fail(QStringLiteral("stale_point"), QStringLiteral("point is not live"));
			const auto value = mutation.value.value_or(node.value().getInValue());
			clip->removeNode(TimePos(oldTick));
			clip->putValue(TimePos(static_cast<tick_t>(tick)), value, false, false);
			pointId = QStringLiteral("%1:%2").arg(clipId).arg(tick);
		}
		if (mutation.curve)
		{
			if (*mutation.curve == QStringLiteral("linear")) clip->setProgressionType(AutomationClip::ProgressionType::Linear);
			else if (*mutation.curve == QStringLiteral("cubic_hermite")) clip->setProgressionType(AutomationClip::ProgressionType::CubicHermite);
			else clip->setProgressionType(AutomationClip::ProgressionType::Discrete);
		}
		resultingTicks.insert(0);
		const auto applied = pointId;
		if (!applied.isEmpty()) ids.insert(applied);
	}
	Engine::getSong()->setModified();
	auto result = ok(QStringLiteral("automation points edited"));
	result.automationClipId = clipId;
	result.appliedPointIds = ids.values().toVector();
	return result;
}

} // namespace lmms
