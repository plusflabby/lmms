#include <QtTest>

#include "AiBridgeArrangementActions.h"

class AiBridgeArrangementExecutionTest : public QObject
{
	Q_OBJECT

private slots:
	void namedRegionOperationsUsePlanner()
	{
		lmms::AiBridgeArrangementRequest request;
		request.source = {0, 480};
		request.destinationTick = 960;
		request.scope.mode = QStringLiteral("all_tracks");
		lmms::AiBridgeTimelineClipSnapshot clip;
		clip.clipId = QStringLiteral("clip:1");
		clip.trackId = QStringLiteral("track:1");
		clip.startTick = 0;
		clip.endTick = 480;
		QCOMPARE(lmms::AiBridgeArrangementActions::planDuplicate(request, {clip}).orderedOperations,
			QVector<QString>({QStringLiteral("delete_overwrites"), QStringLiteral("create_clips"), QStringLiteral("create_automation")}));
		QCOMPARE(lmms::AiBridgeArrangementActions::planMove(request, {clip}).orderedOperations,
			QVector<QString>({QStringLiteral("delete_overwrites"), QStringLiteral("move_clips"), QStringLiteral("move_automation")}));
	}
	void stopsAndReportsPartialFailureWithoutReplay()
	{
		lmms::AiBridgeArrangementPlan plan;
		plan.orderedOperations = {QStringLiteral("delete_overwrites"), QStringLiteral("create_clips"), QStringLiteral("move_automation")};
		QVector<QString> called;
		const auto result = lmms::AiBridgeArrangementActions::execute(plan, 1, 1,
			[&](const QString &step) { called.append(step); return step != QStringLiteral("create_clips"); },
			[](lmms::AiBridgeArrangementPlan &) { return true; });
		QVERIFY(!result.success);
		QCOMPARE(result.code, QStringLiteral("partial_failure"));
		QCOMPARE(called, QVector<QString>({QStringLiteral("delete_overwrites"), QStringLiteral("create_clips")}));
		QCOMPARE(result.appliedSteps, QVector<QString>({QStringLiteral("delete_overwrites")}));
		QCOMPARE(result.unappliedSteps, QVector<QString>({QStringLiteral("create_clips"), QStringLiteral("move_automation")}));
	}

	void staleGenerationDoesZeroWrites()
	{
		lmms::AiBridgeArrangementPlan plan;
		plan.orderedOperations = {QStringLiteral("move_clips")};
		bool called = false;
		const auto result = lmms::AiBridgeArrangementActions::execute(plan, 1, 2,
			[&](const QString &) { called = true; return true; }, {});
		QVERIFY(!result.success);
		QCOMPARE(result.code, QStringLiteral("plan_stale"));
		QVERIFY(!called);
	}

	void verificationMismatchIsExplicit()
	{
		lmms::AiBridgeArrangementPlan plan;
		plan.orderedOperations = {QStringLiteral("delete_clips")};
		const auto result = lmms::AiBridgeArrangementActions::execute(plan, 1, 1,
			[](const QString &) { return true; },
			[](lmms::AiBridgeArrangementPlan &) { return false; });
		QCOMPARE(result.code, QStringLiteral("post_verification_failed"));
	}

	void insertAndCloseProduceExplicitShifts()
	{
		lmms::AiBridgeArrangementRequest request;
		request.source = {0, 480};
		request.deltaTick = 480;
		request.scope.mode = QStringLiteral("all_tracks");
		lmms::AiBridgeTimelineClipSnapshot later;
		later.clipId = QStringLiteral("later");
		later.trackId = QStringLiteral("track:1");
		later.startTick = 960;
		later.endTick = 1440;
		request.operation = QStringLiteral("insert_space");
		const auto insert = lmms::AiBridgeArrangementActions::planInsertSpace(request, {later});
		QCOMPARE(insert.shiftedClipIds, QVector<QString>({QStringLiteral("later")}));
		QCOMPARE(insert.shiftTick, qint64(480));
		request.operation = QStringLiteral("close_gap");
		const auto close = lmms::AiBridgeArrangementActions::planCloseGap(request, {later});
		QCOMPARE(close.shiftTick, qint64(-480));
	}

	void typedExecutorStopsAtFailedStep()
	{
		lmms::AiBridgeArrangementPlan plan;
		plan.steps = {
			{lmms::AiBridgeArrangementStepKind::DeleteClip, QStringLiteral("a"), {}, 0, 0},
			{lmms::AiBridgeArrangementStepKind::MoveClip, QStringLiteral("b"), QStringLiteral("t"), 0, 480},
			{lmms::AiBridgeArrangementStepKind::MoveClip, QStringLiteral("c"), QStringLiteral("t"), 0, 960}};
		plan.orderedOperations = {QStringLiteral("delete_clips"), QStringLiteral("move_clips"), QStringLiteral("move_automation")};
		QVector<QString> called;
		const auto result = lmms::AiBridgeArrangementActions::executeTyped(plan, 1, 1,
			[&](const lmms::AiBridgeArrangementStep &step) {
				called.append(step.clipId);
				return step.clipId != QStringLiteral("b");
			}, {});
		QCOMPARE(called, QVector<QString>({QStringLiteral("a"), QStringLiteral("b")}));
		QCOMPARE(result.code, QStringLiteral("partial_failure"));
	}

	void typedExecutorReportsExactAppliedAndUnappliedSteps()
	{
		lmms::AiBridgeArrangementPlan plan;
		plan.resultingBounds = {0, 960};
		plan.steps = {
			{lmms::AiBridgeArrangementStepKind::MoveClip, QStringLiteral("clip:a"), QStringLiteral("track:a"), 0, 480},
			{lmms::AiBridgeArrangementStepKind::MoveClip, QStringLiteral("clip:b"), QStringLiteral("track:b"), 0, 960},
			{lmms::AiBridgeArrangementStepKind::MoveClip, QStringLiteral("clip:c"), QStringLiteral("track:c"), 0, 1440}};
		const auto result = lmms::AiBridgeArrangementActions::executeTyped(plan, 1, 1,
			[](const lmms::AiBridgeArrangementStep &step) { return step.clipId != QStringLiteral("clip:b"); }, {});
		QCOMPARE(result.failedStepIndex, 1);
		QCOMPARE(result.appliedTypedSteps.size(), 1);
		QCOMPARE(result.unappliedTypedSteps.size(), 2);
		QCOMPARE(result.appliedTypedSteps.front().clipId, QStringLiteral("clip:a"));
		QCOMPARE(result.unappliedTypedSteps.front().clipId, QStringLiteral("clip:b"));
		QVERIFY(result.preflightSucceeded);
		QVERIFY(!result.executionComplete);
		QVERIFY(result.partialMutation);
		QCOMPARE(result.failedTypedStep.clipId, QStringLiteral("clip:b"));
		QCOMPARE(result.affectedTrackIds, QVector<QString>({QStringLiteral("track:a"), QStringLiteral("track:b"), QStringLiteral("track:c")}));
		QCOMPARE(result.affectedStartTick, qint64(0));
		QCOMPARE(result.affectedEndTick, qint64(960));
		QCOMPARE(result.projectGeneration, quint64(1));
	}
};

QTEST_MAIN(AiBridgeArrangementExecutionTest)
#include "AiBridgeArrangementExecutionTest.moc"
