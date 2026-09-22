#include <QtTest>
#include "AiBridgeMidiPlanner.h"

class AiBridgeMidiHumanizeGroovePlannerTest : public QObject
{
	Q_OBJECT
private slots:
	void humanizeIsSeededAndCanonical()
	{
		lmms::AiBridgeMidiClipSnapshot clip;
		clip.notes = {{24, 12, 61, 100, 0}, {0, 12, 60, 100, 0}};
		lmms::AiBridgeMidiHumanizeOptions options;
		options.seed = 123u;
		options.timingRangeTicks = 4;
		options.volumeRange = 10;
		options.lengthRangeTicks = 3;
		const auto first = lmms::planHumanize(clip, {0, 1}, options);
		const auto repeat = lmms::planHumanize(clip, {0, 1}, options);
		QVERIFY(first.success);
		QCOMPARE(first.notes.size(), repeat.notes.size());
		for (int index = 0; index < first.notes.size(); ++index)
		{
			QCOMPARE(first.notes[index].positionTicks, repeat.notes[index].positionTicks);
			QCOMPARE(first.notes[index].lengthTicks, repeat.notes[index].lengthTicks);
			QCOMPARE(first.notes[index].volume, repeat.notes[index].volume);
		}
		options.seed = 124u;
		const auto varied = lmms::planHumanize(clip, {0, 1}, options);
		QVERIFY(first.notes[0].positionTicks != varied.notes[0].positionTicks
			|| first.notes[0].lengthTicks != varied.notes[0].lengthTicks
			|| first.notes[0].volume != varied.notes[0].volume);
	}

	void humanizeClampsOrRejectsBoundaries()
	{
		lmms::AiBridgeMidiClipSnapshot clip;
		clip.notes = {{0, 1, 60, 200, 0}};
		lmms::AiBridgeMidiHumanizeOptions options;
		options.seed = 1u;
		options.timingRangeTicks = 100;
		options.volumeRange = 100;
		options.lengthRangeTicks = 100;
		const auto clamped = lmms::planHumanize(clip, {0}, options);
		QVERIFY(clamped.success);
		QVERIFY(clamped.notes[0].positionTicks >= 0);
		QVERIFY(clamped.notes[0].lengthTicks >= 1);
		QVERIFY(clamped.notes[0].volume <= 200);
		options.boundaryPolicy = lmms::AiBridgeMidiBoundaryPolicy::Reject;
		QVERIFY(!lmms::planHumanize(clip, {0}, options).success);
	}

	void freezesSeedMappingDistributionAndDisabledDraws()
	{
		lmms::AiBridgeMidiClipSnapshot clip;
		clip.notes = {{10, 10, 60, 100, 0}};
		lmms::AiBridgeMidiHumanizeOptions uniform;
		uniform.seed = 123u;
		uniform.timingRangeTicks = 4;
		uniform.volumeRange = 10;
		uniform.lengthRangeTicks = 3;
		uniform.distribution = lmms::AiBridgeMidiHumanizeDistribution::Uniform;
		const auto uniformPlan = lmms::planHumanize(clip, {0}, uniform);
		QCOMPARE(uniformPlan.notes[0].positionTicks, qint64(6));
		QCOMPARE(uniformPlan.notes[0].volume, 105);
		QCOMPARE(uniformPlan.notes[0].lengthTicks, qint64(9));
		uniform.distribution = lmms::AiBridgeMidiHumanizeDistribution::Triangular;
		const auto triangularPlan = lmms::planHumanize(clip, {0}, uniform);
		QCOMPARE(triangularPlan.notes[0].positionTicks, qint64(6));
		QCOMPARE(triangularPlan.notes[0].volume, 105);
		QCOMPARE(triangularPlan.notes[0].lengthTicks, qint64(9));

		lmms::AiBridgeMidiHumanizeOptions seedZero;
		seedZero.seed = 0;
		seedZero.timingRangeTicks = 4;
		auto remapped = seedZero;
		remapped.seed = 0x6D2B79F5u;
		QCOMPARE(lmms::planHumanize(clip, {0}, seedZero).notes[0].positionTicks,
			lmms::planHumanize(clip, {0}, remapped).notes[0].positionTicks);

		uniform.timingRangeTicks = 0;
		uniform.volumeRange = 10;
		uniform.lengthRangeTicks = 0;
		uniform.distribution = lmms::AiBridgeMidiHumanizeDistribution::Uniform;
		QCOMPARE(lmms::planHumanize(clip, {0}, uniform).notes[0].volume, 93);
	}

	void reportsNoEffectAndExactClampCount()
	{
		lmms::AiBridgeMidiClipSnapshot clip;
		clip.notes = {{0, 1, 60, 200, 0}};
		lmms::AiBridgeMidiHumanizeOptions none;
		QCOMPARE(lmms::planHumanize(clip, {0}, none).code, QStringLiteral("no_effect"));
		lmms::AiBridgeMidiHumanizeOptions clamped;
		clamped.seed = 123u;
		clamped.timingRangeTicks = 4;
		clamped.volumeRange = 10;
		clamped.lengthRangeTicks = 3;
		clamped.distribution = lmms::AiBridgeMidiHumanizeDistribution::Uniform;
		const auto result = lmms::planHumanize(clip, {0}, clamped);
		QCOMPARE(result.clampCount, 3);
	}

	void appliesSwingAndCyclicCallerSuppliedTemplate()
	{
		lmms::AiBridgeMidiClipSnapshot clip;
		clip.notes = {{0, 12, 60, 100, 0}, {24, 12, 61, 100, 0}, {48, 12, 62, 100, 0}, {72, 12, 63, 100, 0}};
		lmms::AiBridgeMidiGrooveOptions swing;
		swing.subdivisionTicks = 24;
		swing.swingPercent = 75.0;
		const auto swung = lmms::planGroove(clip, {0, 1, 2, 3}, swing);
		QVERIFY(swung.success);
		QCOMPARE(swung.notes[1].positionTicks, qint64(36));
		swing.strength = 0.5;
		QCOMPARE(lmms::planGroove(clip, {0, 1, 2, 3}, swing).notes[1].positionTicks, qint64(30));
		swing.swingPercent = 50.0;
		QCOMPARE(lmms::planGroove(clip, {0, 1, 2, 3}, swing).code, QStringLiteral("no_effect"));

		lmms::AiBridgeMidiGrooveOptions templ;
		templ.mode = lmms::AiBridgeMidiGrooveMode::Template;
		templ.stepTicks = 24;
		templ.timingOffsets = {0, 3, -1, 4};
		templ.volumeOffsets = {8, -4, 3, -5};
		const auto planned = lmms::planGroove(clip, {0, 1, 2, 3}, templ);
		QCOMPARE(planned.notes[0].positionTicks, qint64(0));
		QCOMPARE(planned.notes[1].positionTicks, qint64(27));
		QCOMPARE(planned.notes[2].positionTicks, qint64(47));
		QCOMPARE(planned.notes[3].positionTicks, qint64(76));
		QCOMPARE(planned.notes[1].volume, 96);
	}
};
QTEST_MAIN(AiBridgeMidiHumanizeGroovePlannerTest)
#include "AiBridgeMidiHumanizeGroovePlannerTest.moc"
