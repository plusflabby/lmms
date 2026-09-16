#include <QtTest>

#include "AiBridgeTimelineTypes.h"

class AiBridgeTimelineTypesTest : public QObject
{
	Q_OBJECT

private slots:
	void validatesCanonicalTicksAndConveniencePositions()
	{
		lmms::AiBridgeMusicalPosition direct;
		direct.tick = 960;
		QVERIFY(direct.validate().success());
		direct.tick = -1;
		QCOMPARE(direct.validate().code, QStringLiteral("invalid_tick"));

		lmms::AiBridgeMusicalPosition musical;
		musical.bar = 1;
		musical.beat = 1;
		musical.tickInBeat = 0;
		QVERIFY(musical.validate().success());
		musical.tick = 0;
		QCOMPARE(musical.validate().code, QStringLiteral("position_representations_conflict"));
	}

	void validatesHalfOpenRangesAndScopes()
	{
		lmms::AiBridgeTimelineRange range{0, 480};
		QVERIFY(range.validate().success());
		QVERIFY(range.contains(0));
		QVERIFY(!range.contains(480));
		range.endTick = 0;
		QCOMPARE(range.validate().code, QStringLiteral("invalid_tick_range"));

		lmms::AiBridgeTrackScope scope;
		scope.mode = QStringLiteral("track_ids");
		scope.trackIds = {QStringLiteral("track:a"), QStringLiteral("track:a")};
		QCOMPARE(scope.validate().code, QStringLiteral("duplicate_track_id"));
	}

	void enforcesPageCaps()
	{
		QVERIFY(lmms::AiBridgeTimelineLimits::validPage(0, 512));
		QVERIFY(!lmms::AiBridgeTimelineLimits::validPage(-1, 1));
		QVERIFY(!lmms::AiBridgeTimelineLimits::validPage(0, 513));
	}
};

QTEST_MAIN(AiBridgeTimelineTypesTest)
#include "AiBridgeTimelineTypesTest.moc"
