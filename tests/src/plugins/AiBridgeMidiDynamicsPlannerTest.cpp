#include <QtTest>
#include "AiBridgeMidiPlanner.h"

class AiBridgeMidiDynamicsPlannerTest : public QObject
{
	Q_OBJECT
private slots:
	void shapesVelocityModesAndBounds()
	{
		lmms::AiBridgeMidiClipSnapshot clip;
		clip.notes = {{0, 10, 60, 80, 0}, {24, 10, 61, 80, 0}, {48, 10, 62, 80, 0}, {72, 10, 63, 80, 0}};
		lmms::AiBridgeMidiVelocityOptions options;
		options.mode = lmms::AiBridgeMidiValueMode::Set; options.value = 90;
		const auto set = lmms::planVelocityShape(clip, {0}, options);
		QVERIFY(set.success); QCOMPARE(set.notes[0].volume, 90);
		options.mode = lmms::AiBridgeMidiValueMode::Offset; options.value = 10;
		QCOMPARE(lmms::planVelocityShape(clip, {0}, options).notes[0].volume, 90);
		options.value = -10;
		QCOMPARE(lmms::planVelocityShape(clip, {0}, options).notes[0].volume, 70);
		options.mode = lmms::AiBridgeMidiValueMode::Scale; options.value = 1.5;
		QCOMPARE(lmms::planVelocityShape(clip, {0}, options).notes[0].volume, 120);
		clip.notes[0].volume = 81;
		QCOMPARE(lmms::planVelocityShape(clip, {0}, options).notes[0].volume, 122);

		lmms::AiBridgeMidiClipSnapshot unordered;
		unordered.notes = {{72, 10, 63, 80, 0}, {0, 10, 60, 80, 0}, {48, 10, 62, 80, 0}, {24, 10, 61, 80, 0}};
		options.mode = lmms::AiBridgeMidiValueMode::Ramp; options.startValue = 70; options.endValue = 100;
		const auto ramp = lmms::planVelocityShape(unordered, {3, 1, 0, 2}, options);
		QVERIFY(ramp.success);
		QCOMPARE(ramp.notes[0].volume, 70); QCOMPARE(ramp.notes[1].volume, 80);
		QCOMPARE(ramp.notes[2].volume, 90); QCOMPARE(ramp.notes[3].volume, 100);
		lmms::AiBridgeMidiClipSnapshot interpolation;
		interpolation.notes = {{0, 10, 60, 80, 0}, {1, 10, 61, 80, 0}, {2, 10, 62, 80, 0}};
		options.startValue = 0; options.endValue = 1;
		const auto halfAway = lmms::planVelocityShape(interpolation, {0, 1, 2}, options);
		QCOMPARE(halfAway.notes[0].volume, 0); QCOMPARE(halfAway.notes[1].volume, 1);
		QCOMPARE(halfAway.notes[2].volume, 1);
	}

	void appliesVelocityBoundaryPoliciesAndNoEffect()
	{
		lmms::AiBridgeMidiClipSnapshot clip;
		clip.notes = {{0, 10, 60, 80, 0}};
		lmms::AiBridgeMidiVelocityOptions options;
		options.mode = lmms::AiBridgeMidiValueMode::Set; options.value = 300;
		const auto highClamp = lmms::planVelocityShape(clip, {0}, options);
		QVERIFY(highClamp.success); QCOMPARE(highClamp.notes[0].volume, 200);
		options.value = -10;
		const auto lowClamp = lmms::planVelocityShape(clip, {0}, options);
		QVERIFY(lowClamp.success); QCOMPARE(lowClamp.notes[0].volume, 0);
		options.boundaryPolicy = lmms::AiBridgeMidiBoundaryPolicy::Reject;
		const auto lowReject = lmms::planVelocityShape(clip, {0}, options);
		QVERIFY(!lowReject.success); QCOMPARE(lowReject.code, QStringLiteral("boundary_rejected"));
		options.value = 300;
		const auto highReject = lmms::planVelocityShape(clip, {0}, options);
		QVERIFY(!highReject.success); QCOMPARE(highReject.code, QStringLiteral("boundary_rejected"));
		options.boundaryPolicy = lmms::AiBridgeMidiBoundaryPolicy::Clamp;
		options.value = 80;
		const auto unchanged = lmms::planVelocityShape(clip, {0}, options);
		QVERIFY(unchanged.success); QCOMPARE(unchanged.code, QStringLiteral("no_effect"));
	}

	void shapesLengthsAndGateToNext()
	{
		lmms::AiBridgeMidiClipSnapshot clip;
		clip.notes = {{0, 10, 60, 100, 0}, {0, 10, 64, 100, 0}, {96, 10, 67, 100, 0}, {192, 10, 69, 100, 0}};
		lmms::AiBridgeMidiLengthOptions options;
		options.mode = lmms::AiBridgeMidiValueMode::Set; options.value = 20;
		const auto set = lmms::planLengthShape(clip, {0}, options);
		QVERIFY(set.success); QCOMPARE(set.notes[0].lengthTicks, qint64(20));
		options.mode = lmms::AiBridgeMidiValueMode::Offset; options.value = -20;
		const auto clamped = lmms::planLengthShape(clip, {0}, options);
		QVERIFY(clamped.success); QCOMPARE(clamped.notes[0].lengthTicks, qint64(1));
		options.boundaryPolicy = lmms::AiBridgeMidiBoundaryPolicy::Reject;
		const auto rejected = lmms::planLengthShape(clip, {0}, options);
		QVERIFY(!rejected.success); QCOMPARE(rejected.code, QStringLiteral("boundary_rejected"));
		options.boundaryPolicy = lmms::AiBridgeMidiBoundaryPolicy::Clamp;
		options.mode = lmms::AiBridgeMidiValueMode::Scale; options.value = 2.0;
		QCOMPARE(lmms::planLengthShape(clip, {0}, options).notes[0].lengthTicks, qint64(20));
		options.mode = lmms::AiBridgeMidiValueMode::GateToNext; options.gatePercent = 75.0;
		const auto gate = lmms::planLengthShape(clip, {0, 1, 2, 3}, options);
		QVERIFY(gate.success);
		QCOMPARE(gate.notes[0].lengthTicks, qint64(72)); QCOMPARE(gate.notes[1].lengthTicks, qint64(72));
		QCOMPARE(gate.notes[2].lengthTicks, qint64(72));
		QCOMPARE(gate.notes[3].lengthTicks, qint64(10));
		lmms::AiBridgeMidiClipSnapshot rounding;
		rounding.notes = {{0, 10, 60, 100, 0}, {3, 10, 62, 100, 0}};
		options.gatePercent = 50.0;
		QCOMPARE(lmms::planLengthShape(rounding, {0}, options).notes[0].lengthTicks, qint64(2));
		options.gatePercent = 75.0;
		const auto finalUnchanged = lmms::planLengthShape(clip, {3}, options);
		QVERIFY(finalUnchanged.success); QCOMPARE(finalUnchanged.code, QStringLiteral("no_effect"));
		options.fallbackLengthTicks = 24;
		QCOMPARE(lmms::planLengthShape(clip, {3}, options).notes[3].lengthTicks, qint64(24));
	}

	void reportsNoEffectForUnchangedLengths()
	{
		lmms::AiBridgeMidiClipSnapshot clip;
		clip.notes = {{0, 10, 60, 100, 0}};
		lmms::AiBridgeMidiLengthOptions options;
		options.mode = lmms::AiBridgeMidiValueMode::Set; options.value = 10;
		const auto unchanged = lmms::planLengthShape(clip, {0}, options);
		QVERIFY(unchanged.success); QCOMPARE(unchanged.code, QStringLiteral("no_effect"));
	}
};
QTEST_MAIN(AiBridgeMidiDynamicsPlannerTest)
#include "AiBridgeMidiDynamicsPlannerTest.moc"
