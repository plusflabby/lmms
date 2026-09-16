#include <QtTest>

#include "AiBridgeAutomationActions.h"

class AiBridgeAutomationPointsTest : public QObject
{
	Q_OBJECT

private slots:
	void preflightsPointMutations()
	{
		lmms::AiBridgeAutomationPointMutation add;
		add.kind = lmms::AiBridgeAutomationPointMutation::Kind::Add;
		add.tick = 480;
		add.value = 0.5f;
		add.curve = QStringLiteral("linear");
		QVERIFY(lmms::AiBridgeAutomationCurve::isSupported(*add.curve));
		const auto result = lmms::AiBridgeAutomationActions::editPoints(QStringLiteral("stale"), {add});
		QVERIFY(!result.success);
		QCOMPARE(result.code, QStringLiteral("stale_automation_clip"));
	}

	void rejectsInvalidPointValuesAndCurves()
	{
		lmms::AiBridgeAutomationPointMutation add;
		add.kind = lmms::AiBridgeAutomationPointMutation::Kind::Add;
		add.tick = 1;
		add.value = std::numeric_limits<float>::quiet_NaN();
		const auto invalidValue = lmms::AiBridgeAutomationActions::editPoints(QStringLiteral("stale"), {add});
		QCOMPARE(invalidValue.code, QStringLiteral("stale_automation_clip"));
		add.value = 0.5f;
		add.curve = QStringLiteral("unsupported");
		const auto invalidCurve = lmms::AiBridgeAutomationActions::editPoints(QStringLiteral("stale"), {add});
		QCOMPARE(invalidCurve.code, QStringLiteral("stale_automation_clip"));
	}
};

QTEST_MAIN(AiBridgeAutomationPointsTest)
#include "AiBridgeAutomationPointsTest.moc"
