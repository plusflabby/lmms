#include <QtTest>

#include "AiBridgeMidiPlanner.h"

class AiBridgeMidiQuantizePlannerTest : public QObject
{
	Q_OBJECT

	static lmms::AiBridgeMidiClipSnapshot fixture(std::initializer_list<qint64> positions)
	{
		lmms::AiBridgeMidiClipSnapshot clip;
		for (const auto position : positions) clip.notes.append({position, 12, 60, 100, 0});
		return clip;
	}

private slots:
	void usesEarlierPointForNearestGridTies()
	{
		const auto result = lmms::planQuantize(fixture({10, 14, 12}), {0, 1, 2}, {24});
		QVERIFY(result.success);
		QCOMPARE(result.notes[0].positionTicks, qint64(0));
		QCOMPARE(result.notes[1].positionTicks, qint64(0));
		QCOMPARE(result.notes[2].positionTicks, qint64(24));
	}

	void supportsAllDirections()
	{
		auto options = lmms::AiBridgeMidiQuantizeOptions{24};
		options.direction = lmms::AiBridgeMidiQuantizeDirection::Earlier;
		QCOMPARE(lmms::planQuantize(fixture({14}), {0}, options).notes[0].positionTicks, qint64(0));
		options.direction = lmms::AiBridgeMidiQuantizeDirection::Later;
		QCOMPARE(lmms::planQuantize(fixture({14}), {0}, options).notes[0].positionTicks, qint64(24));
		options.direction = lmms::AiBridgeMidiQuantizeDirection::Nearest;
		QCOMPARE(lmms::planQuantize(fixture({14}), {0}, options).notes[0].positionTicks, qint64(24));
	}

	void usesHalfAwayFromZeroStrengthInterpolation()
	{
		auto options = lmms::AiBridgeMidiQuantizeOptions{24};
		options.strength = 0.5;
		const auto result = lmms::planQuantize(fixture({10, 14}), {0, 1}, options);
		QCOMPARE(result.notes[0].positionTicks, qint64(5));
		QCOMPARE(result.notes[1].positionTicks, qint64(19));
	}

	void enforcesMaxShiftAndBoundaryPolicy()
	{
		auto limited = lmms::AiBridgeMidiQuantizeOptions{24};
		limited.maxShiftTicks = 5;
		const auto skipped = lmms::planQuantize(fixture({10}), {0}, limited);
		QVERIFY(skipped.success);
		QCOMPARE(skipped.skippedCount, 1);
		QCOMPARE(skipped.notes[0].positionTicks, qint64(10));

		auto clamp = lmms::AiBridgeMidiQuantizeOptions{24};
		clamp.direction = lmms::AiBridgeMidiQuantizeDirection::Earlier;
		clamp.boundaryPolicy = lmms::AiBridgeMidiBoundaryPolicy::Clamp;
		QCOMPARE(lmms::planQuantize(fixture({-10}), {0}, clamp).notes[0].positionTicks, qint64(0));
		clamp.boundaryPolicy = lmms::AiBridgeMidiBoundaryPolicy::Reject;
		const auto rejected = lmms::planQuantize(fixture({-10}), {0}, clamp);
		QVERIFY(!rejected.success);
		QCOMPARE(rejected.code, QStringLiteral("boundary_rejected"));
	}

	void reportsNoEffectForAlreadyQuantizedSelection()
	{
		const auto result = lmms::planQuantize(fixture({0, 24}), {0, 1}, {24});
		QVERIFY(result.success);
		QCOMPARE(result.code, QStringLiteral("no_effect"));
		QCOMPARE(result.changedCount, 0);
		QCOMPARE(result.stepCount, 0);
	}
};

QTEST_MAIN(AiBridgeMidiQuantizePlannerTest)
#include "AiBridgeMidiQuantizePlannerTest.moc"
