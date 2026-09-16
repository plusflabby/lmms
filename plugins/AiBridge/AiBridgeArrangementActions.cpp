#include "AiBridgeArrangementActions.h"

namespace lmms
{

namespace
{
void initializeResult(AiBridgeArrangementExecutionResult &result,
	const AiBridgeArrangementPlan &plan, quint64 projectGeneration)
{
	result.projectGeneration = projectGeneration;
	result.affectedStartTick = plan.resultingBounds.startTick;
	result.affectedEndTick = plan.resultingBounds.endTick;
	for (const auto &step : plan.steps)
		if (!step.trackId.isEmpty() && !result.affectedTrackIds.contains(step.trackId)) result.affectedTrackIds.append(step.trackId);
}

AiBridgeArrangementPlan withOperation(const AiBridgeArrangementRequest &request,
	const QVector<AiBridgeTimelineClipSnapshot> &clips, const QString &operation)
{
	auto copy = request;
	copy.operation = operation;
	return AiBridgeArrangementPlanner::plan(copy, clips);
}
}

AiBridgeArrangementPlan AiBridgeArrangementActions::planDuplicate(const AiBridgeArrangementRequest &request,
	const QVector<AiBridgeTimelineClipSnapshot> &clips) { return withOperation(request, clips, QStringLiteral("duplicate")); }

AiBridgeArrangementPlan AiBridgeArrangementActions::planMove(const AiBridgeArrangementRequest &request,
	const QVector<AiBridgeTimelineClipSnapshot> &clips) { return withOperation(request, clips, QStringLiteral("move")); }

AiBridgeArrangementPlan AiBridgeArrangementActions::planDelete(const AiBridgeArrangementRequest &request,
	const QVector<AiBridgeTimelineClipSnapshot> &clips) { return withOperation(request, clips, QStringLiteral("delete")); }

AiBridgeArrangementPlan AiBridgeArrangementActions::planInsertSpace(const AiBridgeArrangementRequest &request,
	const QVector<AiBridgeTimelineClipSnapshot> &clips) { return withOperation(request, clips, QStringLiteral("insert_space")); }

AiBridgeArrangementPlan AiBridgeArrangementActions::planCloseGap(const AiBridgeArrangementRequest &request,
	const QVector<AiBridgeTimelineClipSnapshot> &clips) { return withOperation(request, clips, QStringLiteral("close_gap")); }

AiBridgeArrangementPlan AiBridgeArrangementActions::planReplace(const AiBridgeArrangementRequest &request,
	const QVector<AiBridgeTimelineClipSnapshot> &clips) { return withOperation(request, clips, QStringLiteral("replace")); }

AiBridgeArrangementExecutionResult AiBridgeArrangementActions::execute(
	const AiBridgeArrangementPlan &plan, quint64 expectedProjectGeneration,
	quint64 currentProjectGeneration, const StepExecutor &stepExecutor,
	const Verifier &verifier)
{
	AiBridgeArrangementExecutionResult result;
	result.observed = plan;
	initializeResult(result, plan, currentProjectGeneration);
	if (expectedProjectGeneration != currentProjectGeneration)
	{
		result.code = QStringLiteral("plan_stale");
		result.message = QStringLiteral("project generation changed before execution");
		result.nativeFailureCode = result.code;
		result.nativeFailureMessage = result.message;
		result.unappliedSteps = plan.orderedOperations;
		result.unappliedTypedSteps = plan.steps;
		return result;
	}
	for (int i = 0; i < plan.orderedOperations.size(); ++i)
	{
		const auto &step = plan.orderedOperations.at(i);
		if (!stepExecutor || !stepExecutor(step))
		{
			result.code = result.appliedSteps.isEmpty() ? QStringLiteral("execution_failed") : QStringLiteral("partial_failure");
			result.message = QStringLiteral("native arrangement step failed");
			result.nativeFailureCode = result.code;
			result.nativeFailureMessage = result.message;
			result.unappliedSteps = plan.orderedOperations.mid(i);
			return result;
		}
		result.appliedSteps.append(step);
	}
	if (verifier && !verifier(result.observed))
	{
		result.code = QStringLiteral("post_verification_failed");
		result.message = QStringLiteral("native result did not match post-execution verification");
		return result;
	}
	result.success = true;
	result.preflightSucceeded = true;
	result.executionComplete = true;
	result.code = QStringLiteral("ok");
	result.message = QStringLiteral("arrangement executed and verified");
	return result;
}

AiBridgeArrangementExecutionResult AiBridgeArrangementActions::executeTyped(
	const AiBridgeArrangementPlan &plan, quint64 expectedProjectGeneration,
	quint64 currentProjectGeneration, const TypedStepExecutor &stepExecutor,
	const Verifier &verifier)
{
	AiBridgeArrangementExecutionResult result;
	result.observed = plan;
	initializeResult(result, plan, currentProjectGeneration);
	if (expectedProjectGeneration != currentProjectGeneration)
	{
		result.code = QStringLiteral("plan_stale");
		result.message = QStringLiteral("project generation changed before execution");
		result.nativeFailureCode = result.code;
		result.nativeFailureMessage = result.message;
		result.unappliedSteps = plan.orderedOperations;
		result.unappliedTypedSteps = plan.steps;
		return result;
	}
	for (int i = 0; i < plan.steps.size(); ++i)
	{
		const auto &step = plan.steps.at(i);
		if (!stepExecutor || !stepExecutor(step))
		{
			result.failedStepIndex = i;
			result.hasFailedTypedStep = true;
			result.failedTypedStep = step;
			result.code = result.appliedSteps.isEmpty() ? QStringLiteral("execution_failed") : QStringLiteral("partial_failure");
			result.message = QStringLiteral("native arrangement step failed");
			result.nativeFailureCode = result.code;
			result.nativeFailureMessage = result.message;
			result.preflightSucceeded = true;
			result.partialMutation = !result.appliedTypedSteps.isEmpty();
			result.unappliedSteps = plan.orderedOperations.mid(qMin(i, plan.orderedOperations.size()));
			result.unappliedTypedSteps = plan.steps.mid(i);
			return result;
		}
		result.appliedTypedSteps.append(step);
		if (i < plan.orderedOperations.size()) result.appliedSteps.append(plan.orderedOperations.at(i));
	}
	if (verifier && !verifier(result.observed))
	{
		result.code = QStringLiteral("post_verification_failed");
		result.message = QStringLiteral("native result did not match post-execution verification");
		return result;
	}
	result.success = true;
	result.preflightSucceeded = true;
	result.executionComplete = true;
	result.code = QStringLiteral("ok");
	result.message = QStringLiteral("arrangement executed and verified");
	return result;
}

} // namespace lmms
