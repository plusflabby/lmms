#include <QtTest>

#include "AiBridgeArrangementPlanner.h"

class AiBridgeArrangementPlannerTest : public QObject
{
	Q_OBJECT

private slots:
	void detectsHalfOpenDestinationConflict()
	{
		lmms::AiBridgeArrangementRequest request;
		request.operation = QStringLiteral("duplicate");
		request.source = {0, 480};
		request.destinationTick = 960;
		request.scope.mode = QStringLiteral("all_tracks");
		lmms::AiBridgeTimelineClipSnapshot clip;
		clip.clipId = QStringLiteral("clip:existing");
		clip.trackId = QStringLiteral("track:1");
		clip.startTick = 960;
		clip.endTick = 1440;
		const auto plan = lmms::AiBridgeArrangementPlanner::plan(request, {clip});
		QCOMPARE(plan.conflicts.size(), 1);
		QVERIFY(plan.orderedOperations.isEmpty());
		request.overwrite = true;
		const auto overwrite = lmms::AiBridgeArrangementPlanner::plan(request, {clip});
		QCOMPARE(overwrite.overwriteDeleteIds, QVector<QString>{QStringLiteral("clip:existing")});
	}

	void scopeAndAutomationDefaultsAreExplicit()
	{
		lmms::AiBridgeArrangementRequest request;
		request.operation = QStringLiteral("delete");
		request.source = {0, 480};
		request.scope.mode = QStringLiteral("explicit_tracks");
		request.scope.trackIds = {QStringLiteral("track:one")};
		lmms::AiBridgeTimelineClipSnapshot outside;
		outside.clipId = QStringLiteral("outside");
		outside.trackId = QStringLiteral("track:two");
		outside.startTick = 0;
		outside.endTick = 480;
		const auto plan = lmms::AiBridgeArrangementPlanner::plan(request, {outside});
		QVERIFY(plan.affectedClips.isEmpty());
		QVERIFY(plan.includeAutomation);
	}

	void emitsTypedOrderedSteps()
	{
		lmms::AiBridgeArrangementRequest request;
		request.operation = QStringLiteral("duplicate");
		request.source = {0, 480};
		request.destinationTick = 960;
		request.scope.mode = QStringLiteral("all_tracks");
		lmms::AiBridgeTimelineClipSnapshot clip;
		clip.clipId = QStringLiteral("clip:source");
		clip.trackId = QStringLiteral("track:one");
		clip.startTick = 0;
		clip.endTick = 480;
		const auto plan = lmms::AiBridgeArrangementPlanner::plan(request, {clip});
		QCOMPARE(plan.steps.size(), 1);
		QCOMPARE(plan.steps.front().kind, lmms::AiBridgeArrangementStepKind::DuplicateClip);
		QCOMPARE(plan.steps.front().clipId, QStringLiteral("clip:source"));
		QCOMPARE(plan.steps.front().destinationTick, qint64(960));
	}

	void closeGapDerivesWidthFromRegion()
	{
		lmms::AiBridgeArrangementRequest request;
		request.operation = QStringLiteral("close_gap");
		request.source = {960, 1920};
		request.scope.mode = QStringLiteral("all_tracks");
		lmms::AiBridgeTimelineClipSnapshot clip;
		clip.clipId = QStringLiteral("later"); clip.trackId = QStringLiteral("track:one");
		clip.startTick = 1920; clip.endTick = 2400;
		const auto plan = lmms::AiBridgeArrangementPlanner::plan(request, {clip});
		QCOMPARE(plan.shiftTick, qint64(-960));
		QCOMPARE(plan.steps.size(), 1);
		QCOMPARE(plan.steps.front().destinationTick, qint64(960));
	}

	void rippleDeleteShiftsLaterTimelineAndAutomation()
	{
		lmms::AiBridgeArrangementRequest request;
		request.operation = QStringLiteral("delete");
		request.source = {0, 480};
		request.scope.mode = QStringLiteral("all_tracks");
		request.ripple = true;
		lmms::AiBridgeTimelineClipSnapshot later;
		later.clipId = QStringLiteral("later"); later.trackId = QStringLiteral("track:one");
		later.startTick = 960; later.endTick = 1440;
		lmms::AiBridgeArrangementSnapshot snapshot;
		snapshot.clips.append(later);
		snapshot.automationClips.append({QStringLiteral("automation-track:one"), QStringLiteral("automation-later"), {}, 960, 1440});
		const auto plan = lmms::AiBridgeArrangementPlanner::plan(request, snapshot);
		QCOMPARE(plan.shiftTick, qint64(-480));
		QCOMPARE(plan.steps.size(), 1);
		QCOMPARE(plan.steps.front().kind, lmms::AiBridgeArrangementStepKind::ShiftClip);
		QCOMPARE(plan.steps.front().destinationTick, qint64(480));
		QCOMPARE(plan.automationSteps.size(), 1);
		QCOMPARE(plan.automationSteps.front().kind, lmms::AiBridgeArrangementAutomationStepKind::ShiftClip);
		QCOMPARE(plan.automationSteps.front().destinationStartTick, qint64(480));
	}

	void zeroStepMutationIsNoEffect()
	{
		lmms::AiBridgeArrangementRequest request;
		request.operation = QStringLiteral("replace");
		request.source = {0, 480};
		request.scope.mode = QStringLiteral("all_tracks");
		const auto plan = lmms::AiBridgeArrangementPlanner::plan(request, QVector<lmms::AiBridgeTimelineClipSnapshot>{});
		QVERIFY(plan.noEffect);
		QVERIFY(!plan.noEffectReason.isEmpty());
	}

	void combinedSnapshotPlansAutomationWithoutNativeIdentity()
	{
		lmms::AiBridgeArrangementRequest request;
		request.operation = QStringLiteral("insert_space");
		request.source = {480, 960}; request.deltaTick = 480;
		request.scope.mode = QStringLiteral("all_tracks");
		lmms::AiBridgeArrangementSnapshot snapshot;
		snapshot.automationClips.append({QStringLiteral("automation-track:1"), QStringLiteral("automation-clip:1"), QStringLiteral("automation-target:1"), 960, 1440});
		const auto plan = lmms::AiBridgeArrangementPlanner::plan(request, snapshot);
		QCOMPARE(plan.automationSteps.size(), 1);
		QCOMPARE(plan.automationSteps.front().kind, lmms::AiBridgeArrangementAutomationStepKind::ShiftClip);
		QCOMPARE(plan.automationSteps.front().destinationStartTick, qint64(1440));
	}

	void combinedSnapshotCarriesAutomationContentForDuplication()
	{
		lmms::AiBridgeArrangementRequest request;
		request.operation = QStringLiteral("duplicate");
		request.source = {0, 480};
		request.destinationTick = 960;
		request.scope.mode = QStringLiteral("all_tracks");
		lmms::AiBridgeArrangementSnapshot snapshot;
		lmms::AiBridgeArrangementAutomationSnapshot automation;
		automation.automationTrackId = QStringLiteral("automation-track:1");
		automation.automationClipId = QStringLiteral("automation-clip:1");
		automation.automationTargetId = QStringLiteral("automation-target:1");
		automation.startTick = 0;
		automation.endTick = 480;
		automation.points = {
			{QStringLiteral("automation-clip:1:0"), 0, 0.25f, QStringLiteral("linear")},
			{QStringLiteral("automation-clip:1:240"), 240, 0.75f, QStringLiteral("cubic_hermite")}};
		snapshot.automationClips.append(automation);
		const auto plan = lmms::AiBridgeArrangementPlanner::plan(request, snapshot);
		QVERIFY(!plan.noEffect);
		QCOMPARE(plan.automationSteps.size(), 1);
		QCOMPARE(plan.automationSteps.front().points.size(), 2);
		QCOMPARE(plan.automationSteps.front().points.at(1).tick, qint64(240));
		QCOMPARE(plan.automationSteps.front().points.at(1).curve, QStringLiteral("cubic_hermite"));
	}

	void combinedSnapshotDoesNotScheduleAutomationAsTimelineClip()
	{
		lmms::AiBridgeArrangementRequest request;
		request.operation = QStringLiteral("duplicate");
		request.source = {0, 480};
		request.destinationTick = 960;
		request.scope.mode = QStringLiteral("all_tracks");
		lmms::AiBridgeArrangementSnapshot snapshot;
		lmms::AiBridgeTimelineClipSnapshot timeline;
		timeline.clipId = QStringLiteral("clip:automation-native");
		timeline.trackId = QStringLiteral("automation-track:1");
		timeline.type = QStringLiteral("automation");
		timeline.startTick = 0;
		timeline.endTick = 480;
		snapshot.clips.append(timeline);
		snapshot.automationClips.append({QStringLiteral("automation-track:1"), QStringLiteral("automation-clip:1"), {}, 0, 480});
		const auto plan = lmms::AiBridgeArrangementPlanner::plan(request, snapshot);
		QVERIFY(plan.steps.isEmpty());
		QCOMPARE(plan.automationSteps.size(), 1);
	}

	void replacePlansDestinationAutomationDeletion()
	{
		lmms::AiBridgeArrangementRequest request;
		request.operation = QStringLiteral("replace");
		request.source = {0, 480};
		request.destinationTick = 960;
		request.scope.mode = QStringLiteral("all_tracks");
		lmms::AiBridgeArrangementSnapshot snapshot;
		snapshot.clips.append({QStringLiteral("timeline-source"), QStringLiteral("track:1"), QStringLiteral("midi"), {}, 0, 480});
		snapshot.automationClips.append({QStringLiteral("automation-track:1"), QStringLiteral("source"), {}, 0, 480});
		snapshot.automationClips.append({QStringLiteral("automation-track:1"), QStringLiteral("destination"), {}, 960, 1440});
		const auto plan = lmms::AiBridgeArrangementPlanner::plan(request, snapshot);
		QCOMPARE(plan.automationSteps.size(), 2);
		QVERIFY(plan.deleteClipIds.isEmpty());
		QCOMPARE(plan.automationSteps.at(0).kind, lmms::AiBridgeArrangementAutomationStepKind::DuplicateClip);
		QCOMPARE(plan.automationSteps.at(1).kind, lmms::AiBridgeArrangementAutomationStepKind::DeleteClip);
		QCOMPARE(plan.automationSteps.at(1).automationClipId, QStringLiteral("destination"));
	}
};

QTEST_MAIN(AiBridgeArrangementPlannerTest)
#include "AiBridgeArrangementPlannerTest.moc"
