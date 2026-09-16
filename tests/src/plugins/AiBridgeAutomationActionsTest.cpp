#include <QtTest>

#include "AiBridgeAutomationActions.h"

class AiBridgeAutomationActionsTest : public QObject
{
	Q_OBJECT

private slots:
	void rejectsInvalidRangesBeforeNativeMutation()
	{
		const auto negative = lmms::AiBridgeAutomationActions::createClip(QStringLiteral("stale"), -1, 480);
		QVERIFY(!negative.success);
		QCOMPARE(negative.code, QStringLiteral("invalid_automation_range"));
		const auto overflow = lmms::AiBridgeAutomationActions::createClip(QStringLiteral("stale"), 0, 0);
		QVERIFY(!overflow.success);
		QCOMPARE(overflow.code, QStringLiteral("invalid_automation_range"));
	}

	void staleObjectsAndTargetsAreRejected()
	{
		const auto track = lmms::AiBridgeAutomationActions::deleteTrack(QStringLiteral("stale"));
		QVERIFY(!track.success);
		QCOMPARE(track.code, QStringLiteral("stale_automation_track"));
		const auto clip = lmms::AiBridgeAutomationActions::setClipTarget(QStringLiteral("stale"), QStringLiteral("stale"));
		QVERIFY(!clip.success);
		QCOMPARE(clip.code, QStringLiteral("stale_automation_clip"));
	}

	void duplicateClipRejectsStaleSourceBeforeNativeMutation()
	{
		const auto result = lmms::AiBridgeAutomationActions::duplicateClip(
			QStringLiteral("stale"), QStringLiteral("automation-track:destination"), 960);
		QVERIFY(!result.success);
		QCOMPARE(result.code, QStringLiteral("stale_automation_clip"));
	}
};

QTEST_MAIN(AiBridgeAutomationActionsTest)
#include "AiBridgeAutomationActionsTest.moc"
