#ifndef LMMS_AIBRIDGE_ARRANGEMENT_ACTIONS_H
#define LMMS_AIBRIDGE_ARRANGEMENT_ACTIONS_H

#include "AiBridgeTimelineTypes.h"
#include "AiBridgeArrangementPlanner.h"

#include <functional>

namespace lmms
{

struct AiBridgeArrangementExecutionResult
{
	bool success = false;
	QString code;
	QString message;
	QVector<QString> appliedSteps;
	QVector<QString> unappliedSteps;
	int failedStepIndex = -1;
	bool preflightSucceeded = false;
	bool executionComplete = false;
	bool partialMutation = false;
	bool hasFailedTypedStep = false;
	AiBridgeArrangementStep failedTypedStep;
	QVector<AiBridgeArrangementStep> appliedTypedSteps;
	QVector<AiBridgeArrangementStep> unappliedTypedSteps;
	QVector<QString> affectedTrackIds;
	qint64 affectedStartTick = 0;
	qint64 affectedEndTick = 0;
	quint64 projectGeneration = 0;
	QString nativeFailureCode;
	QString nativeFailureMessage;
	QString postFailureVerification = QStringLiteral("not_run");
	AiBridgeArrangementPlan observed;
};

class AiBridgeArrangementActions
{
public:
	static AiBridgeArrangementPlan planDuplicate(const AiBridgeArrangementRequest &request,
		const QVector<AiBridgeTimelineClipSnapshot> &clips);
	static AiBridgeArrangementPlan planMove(const AiBridgeArrangementRequest &request,
		const QVector<AiBridgeTimelineClipSnapshot> &clips);
	static AiBridgeArrangementPlan planDelete(const AiBridgeArrangementRequest &request,
		const QVector<AiBridgeTimelineClipSnapshot> &clips);
	static AiBridgeArrangementPlan planInsertSpace(const AiBridgeArrangementRequest &request,
		const QVector<AiBridgeTimelineClipSnapshot> &clips);
	static AiBridgeArrangementPlan planCloseGap(const AiBridgeArrangementRequest &request,
		const QVector<AiBridgeTimelineClipSnapshot> &clips);
	static AiBridgeArrangementPlan planReplace(const AiBridgeArrangementRequest &request,
		const QVector<AiBridgeTimelineClipSnapshot> &clips);
	using StepExecutor = std::function<bool(const QString &)>;
	using TypedStepExecutor = std::function<bool(const AiBridgeArrangementStep &)>;
	using Verifier = std::function<bool(AiBridgeArrangementPlan &)>;

	static AiBridgeArrangementExecutionResult execute(const AiBridgeArrangementPlan &plan,
		quint64 expectedProjectGeneration, quint64 currentProjectGeneration,
		const StepExecutor &stepExecutor, const Verifier &verifier);
	static AiBridgeArrangementExecutionResult executeTyped(const AiBridgeArrangementPlan &plan,
		quint64 expectedProjectGeneration, quint64 currentProjectGeneration,
		const TypedStepExecutor &stepExecutor, const Verifier &verifier);
};

} // namespace lmms

#endif
