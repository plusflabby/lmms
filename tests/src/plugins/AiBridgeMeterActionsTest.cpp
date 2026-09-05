#include <QtTest>
#include "AiBridgeMeterActions.h"

class AiBridgeMeterActionsTest : public QObject
{
	Q_OBJECT
private slots:
	void rejectsOverCapPage()
	{
		const auto result = lmms::AiBridgeMeterActions::get(std::nullopt, 0, lmms::AiBridgeMixerLimits::maxMeterChannels + 1);
		QVERIFY(!result.success);
		QCOMPARE(result.code, QString("invalid_parameter"));
	}
};
QTEST_MAIN(AiBridgeMeterActionsTest)
#include "AiBridgeMeterActionsTest.moc"
