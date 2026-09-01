/*
 * AiBridgeActionsTest.cpp
 *
 * This test defines the Phase 1 action-result contract before the
 * implementation is added.
 */

#include <QtTest>

#include "AiBridgeActions.h"

class AiBridgeActionsTest : public QObject
{
	Q_OBJECT

private slots:
	void actionResultCarriesStableStatusAndMessage()
	{
		const auto result = lmms::AiBridgeActionResult::ok("tempo updated");

		QVERIFY(result.success);
		QCOMPARE(result.code, QString("ok"));
		QCOMPARE(result.message, QString("tempo updated"));
	}

	void invalidParameterProducesStableFailure()
	{
		const auto result = lmms::AiBridgeActionResult::failure(
			"invalid_parameter", "tempo must be positive");

		QVERIFY(!result.success);
		QCOMPARE(result.code, QString("invalid_parameter"));
		QCOMPARE(result.message, QString("tempo must be positive"));
	}
};

QTEST_MAIN(AiBridgeActionsTest)
#include "AiBridgeActionsTest.moc"
