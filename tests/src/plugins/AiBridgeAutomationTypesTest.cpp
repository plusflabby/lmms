#include <QtTest>

#include "AiBridgeAutomationTypes.h"

class AiBridgeAutomationTypesTest : public QObject
{
	Q_OBJECT

private slots:
	void rejectsNonFiniteValues()
	{
		QVERIFY(lmms::AiBridgeAutomationPointValue::validate(0.5f).success());
		QCOMPARE(lmms::AiBridgeAutomationPointValue::validate(qQNaN()).code,
			QStringLiteral("invalid_automation_value"));
		QCOMPARE(lmms::AiBridgeAutomationPointValue::validate(qInf()).code,
			QStringLiteral("invalid_automation_value"));
	}

	void enforcesAutomationPageCapsAndPointBatchCap()
	{
		QVERIFY(lmms::AiBridgeAutomationLimits::validTrackPage(0, 128));
		QVERIFY(!lmms::AiBridgeAutomationLimits::validTrackPage(0, 129));
		QVERIFY(lmms::AiBridgeAutomationLimits::validPointChangeCount(512));
		QVERIFY(!lmms::AiBridgeAutomationLimits::validPointChangeCount(513));
	}

	void exposesOnlyNativeCurveNames()
	{
		QVERIFY(lmms::AiBridgeAutomationCurve::isSupported(QStringLiteral("discrete")));
		QVERIFY(lmms::AiBridgeAutomationCurve::isSupported(QStringLiteral("linear")));
		QVERIFY(lmms::AiBridgeAutomationCurve::isSupported(QStringLiteral("cubic_hermite")));
		QVERIFY(!lmms::AiBridgeAutomationCurve::isSupported(QStringLiteral("bezier")));
	}
};

QTEST_MAIN(AiBridgeAutomationTypesTest)
#include "AiBridgeAutomationTypesTest.moc"
