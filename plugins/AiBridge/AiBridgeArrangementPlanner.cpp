#include "AiBridgeArrangementPlanner.h"

#include <QSet>

namespace lmms
{

namespace
{
bool inScope(const AiBridgeTimelineClipSnapshot &clip, const AiBridgeTrackScope &scope)
{
	return scope.mode == QStringLiteral("all_tracks") || scope.trackIds.contains(clip.trackId);
}

bool overlaps(const AiBridgeTimelineClipSnapshot &clip, const AiBridgeTimelineRange &range)
{
	return clip.startTick < range.endTick && clip.endTick > range.startTick;
}
}

AiBridgeValidationResult AiBridgeArrangementPlanner::validate(const AiBridgeArrangementRequest &request)
{
	const auto scope = request.scope.validate();
	if (!scope.success()) return scope;
	if (!request.source.validate().success()) return {QStringLiteral("invalid_range"), QStringLiteral("source range must be half-open and positive")};
	if (request.operation != QStringLiteral("duplicate") && request.operation != QStringLiteral("move")
		&& request.operation != QStringLiteral("delete") && request.operation != QStringLiteral("insert_space")
		&& request.operation != QStringLiteral("close_gap") && request.operation != QStringLiteral("replace"))
		return {QStringLiteral("invalid_operation"), QStringLiteral("unsupported arrangement operation")};
	if ((request.operation == QStringLiteral("duplicate") || request.operation == QStringLiteral("move"))
		&& request.destinationTick < 0)
		return {QStringLiteral("invalid_destination"), QStringLiteral("destination tick must be non-negative")};
	if (request.operation == QStringLiteral("insert_space")
		&& request.deltaTick <= 0)
		return {QStringLiteral("invalid_delta"), QStringLiteral("delta tick must be positive")};
	return {};
}

AiBridgeArrangementPlan AiBridgeArrangementPlanner::plan(
	const AiBridgeArrangementRequest &request, const QVector<AiBridgeTimelineClipSnapshot> &clips)
{
	AiBridgeArrangementPlan result;
	result.includeAutomation = request.includeAutomation;
	result.overwrite = request.overwrite;
	result.dryRun = request.dryRun;
	const auto validation = validate(request);
	if (!validation.success()) return result;
	QSet<QString> selected;
	for (const auto &clip : clips)
	{
		if (inScope(clip, request.scope) && overlaps(clip, request.source))
		{
			result.affectedClips.append(clip);
			selected.insert(clip.clipId);
		}
	}
	if (result.affectedClips.size() > AiBridgeTimelineLimits::maxRegionAffectedObjects) return {};
	const auto width = request.source.endTick - request.source.startTick;
	AiBridgeTimelineRange destination{request.destinationTick, request.destinationTick + width};
	if (request.operation == QStringLiteral("duplicate") || request.operation == QStringLiteral("move")
		|| request.operation == QStringLiteral("replace"))
	{
		for (const auto &clip : clips)
		{
			if (!inScope(clip, request.scope) || selected.contains(clip.clipId) || !overlaps(clip, destination)) continue;
			AiBridgeArrangementConflict conflict;
			conflict.trackId = clip.trackId;
			conflict.clipId = clip.clipId;
			conflict.range = destination;
			conflict.overwriteWouldResolve = true;
			result.conflicts.append(conflict);
			if (request.overwrite) result.overwriteDeleteIds.append(clip.clipId);
		}
	}
	if (!result.conflicts.isEmpty() && !request.overwrite) return result;
	if (request.operation == QStringLiteral("delete"))
		for (const auto &clip : result.affectedClips) result.deleteClipIds.append(clip.clipId);
	if (request.operation == QStringLiteral("insert_space") || request.operation == QStringLiteral("close_gap")
		|| (request.operation == QStringLiteral("delete") && request.ripple))
	{
		result.shiftTick = request.operation == QStringLiteral("insert_space") ? request.deltaTick : -(request.source.endTick - request.source.startTick);
		for (const auto &clip : clips)
		{
			if (inScope(clip, request.scope) && clip.startTick >= request.source.endTick)
				result.shiftedClipIds.append(clip.clipId);
		}
	}
	if (request.operation == QStringLiteral("duplicate") || request.operation == QStringLiteral("replace"))
		result.orderedOperations = {QStringLiteral("delete_overwrites"), QStringLiteral("create_clips"), QStringLiteral("create_automation")};
	else if (request.operation == QStringLiteral("move"))
		result.orderedOperations = {QStringLiteral("delete_overwrites"), QStringLiteral("move_clips"), QStringLiteral("move_automation")};
	else if ((request.operation == QStringLiteral("delete") && !request.ripple) || request.operation == QStringLiteral("replace"))
		result.orderedOperations = {QStringLiteral("delete_clips"), QStringLiteral("delete_automation")};
	else if (request.operation == QStringLiteral("insert_space") || request.operation == QStringLiteral("close_gap")
		|| (request.operation == QStringLiteral("delete") && request.ripple))
		result.orderedOperations = request.operation == QStringLiteral("delete")
			? QVector<QString>{QStringLiteral("delete_clips"), QStringLiteral("shift_clips"), QStringLiteral("shift_automation")}
			: QVector<QString>{QStringLiteral("shift_clips"), QStringLiteral("shift_automation")};
	else
		result.orderedOperations = {request.operation};
	for (const auto &id : result.overwriteDeleteIds)
		result.steps.append({AiBridgeArrangementStepKind::DeleteClip, id, {}, 0, 0});
	if (request.operation == QStringLiteral("replace"))
		for (const auto &id : result.deleteClipIds)
			result.steps.append({AiBridgeArrangementStepKind::DeleteClip, id, {}, 0, 0});
	if (request.operation == QStringLiteral("duplicate") || request.operation == QStringLiteral("replace"))
	{
		for (const auto &clip : result.affectedClips)
			result.steps.append({AiBridgeArrangementStepKind::DuplicateClip, clip.clipId, clip.trackId,
				clip.startTick, request.destinationTick + (clip.startTick - request.source.startTick)});
	}
	else if (request.operation == QStringLiteral("move"))
	{
		for (const auto &clip : result.affectedClips)
			result.steps.append({AiBridgeArrangementStepKind::MoveClip, clip.clipId, clip.trackId,
				clip.startTick, request.destinationTick + (clip.startTick - request.source.startTick)});
	}
	else if (request.operation == QStringLiteral("delete") && !request.ripple)
	{
		for (const auto &id : result.deleteClipIds)
			result.steps.append({AiBridgeArrangementStepKind::DeleteClip, id, {}, 0, 0});
	}
	if (request.operation == QStringLiteral("delete") && request.ripple)
	{
		for (const auto &id : result.deleteClipIds)
			result.steps.append({AiBridgeArrangementStepKind::DeleteClip, id, {}, 0, 0});
	}
	if (request.operation == QStringLiteral("insert_space") || request.operation == QStringLiteral("close_gap")
		|| (request.operation == QStringLiteral("delete") && request.ripple))
	{
		for (const auto &clip : clips)
			if (result.shiftedClipIds.contains(clip.clipId))
				result.steps.append({AiBridgeArrangementStepKind::ShiftClip, clip.clipId, clip.trackId,
					clip.startTick, clip.startTick + result.shiftTick});
	}
	if (result.steps.isEmpty())
	{
		result.noEffect = true;
		result.noEffectReason = result.affectedClips.isEmpty()
			? QStringLiteral("source or scope contains no applicable objects")
			: QStringLiteral("operation produces no executable changes");
	}
	result.resultingBounds = request.operation == QStringLiteral("delete") ? request.source : destination;
	return result;
}

AiBridgeArrangementPlan AiBridgeArrangementPlanner::plan(
	const AiBridgeArrangementRequest &request, const AiBridgeArrangementSnapshot &snapshot)
{
	QSet<QString> automationClipIds;
	for (const auto &clip : snapshot.automationClips) automationClipIds.insert(clip.automationClipId);
	QVector<AiBridgeTimelineClipSnapshot> timelineClips;
	for (const auto &clip : snapshot.clips)
		if (clip.type != QStringLiteral("automation") && !automationClipIds.contains(clip.clipId)) timelineClips.append(clip);
	const auto result = plan(request, timelineClips);
	if (!result.includeAutomation) return result;
	auto enriched = result;
	const auto inScope = [&](const AiBridgeArrangementAutomationSnapshot &clip) {
		if (request.scope.mode == QStringLiteral("all_tracks")) return true;
		return request.scope.trackIds.contains(clip.automationTrackId) || request.scope.trackIds.contains(clip.ownerId);
	};
	const auto overlaps = [&](const AiBridgeArrangementAutomationSnapshot &clip, const AiBridgeTimelineRange &range) {
		return clip.startTick < range.endTick && clip.endTick > range.startTick;
	};
	const auto width = request.source.endTick - request.source.startTick;
	const auto makeStep = [&](AiBridgeArrangementAutomationStepKind kind,
		const AiBridgeArrangementAutomationSnapshot &clip, qint64 destinationStart, qint64 destinationEnd) {
		AiBridgeArrangementAutomationStep step;
		step.kind = kind;
		step.automationTrackId = clip.automationTrackId;
		step.automationClipId = clip.automationClipId;
		step.automationTargetId = clip.automationTargetId;
		step.sourceStartTick = clip.startTick;
		step.sourceEndTick = clip.endTick;
		step.destinationStartTick = destinationStart;
		step.destinationEndTick = destinationEnd;
		step.points = clip.points;
		return step;
	};
	for (const auto &clip : snapshot.automationClips)
	{
		if (!inScope(clip)) continue;
		const bool sourceMatch = overlaps(clip, request.source);
		const bool destinationMatch = overlaps(clip, {request.destinationTick, request.destinationTick + width});
		if ((request.operation == QStringLiteral("duplicate") || request.operation == QStringLiteral("replace")) && sourceMatch)
			enriched.automationSteps.append(makeStep(AiBridgeArrangementAutomationStepKind::DuplicateClip, clip,
			request.destinationTick + clip.startTick - request.source.startTick,
			request.destinationTick + clip.endTick - request.source.startTick));
		else if (request.operation == QStringLiteral("move") && sourceMatch)
			enriched.automationSteps.append(makeStep(AiBridgeArrangementAutomationStepKind::MoveClip, clip,
			request.destinationTick + clip.startTick - request.source.startTick,
			request.destinationTick + clip.endTick - request.source.startTick));
		else if (request.operation == QStringLiteral("delete") && sourceMatch)
			enriched.automationSteps.append(makeStep(AiBridgeArrangementAutomationStepKind::DeleteClip, clip, 0, 0));
		if ((request.operation == QStringLiteral("insert_space") || request.operation == QStringLiteral("close_gap")
			|| (request.operation == QStringLiteral("delete") && request.ripple)) && clip.startTick >= request.source.endTick)
		{
			const auto delta = request.operation == QStringLiteral("insert_space") ? request.deltaTick : -width;
			enriched.automationSteps.append(makeStep(AiBridgeArrangementAutomationStepKind::ShiftClip, clip,
				clip.startTick + delta, clip.endTick + delta));
		}
		else if (request.operation == QStringLiteral("replace") && destinationMatch)
			enriched.automationSteps.append(makeStep(AiBridgeArrangementAutomationStepKind::DeleteClip, clip, 0, 0));
	}
	if (!enriched.steps.isEmpty() || !enriched.automationSteps.isEmpty()) enriched.noEffect = false;
	return enriched;
}

} // namespace lmms
