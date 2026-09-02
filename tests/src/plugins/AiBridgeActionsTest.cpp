/*
 * AiBridgeActionsTest.cpp
 *
 * This test defines the Phase 1 action-result contract before the
 * implementation is added.
 */

#include <QtTest>
#include <limits>
#include <optional>

#include "AiBridgeActions.h"
#include "AiBridgeProjectActions.h"

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

	void addNotesActionRejectsEmptyInput()
	{
		QVector<lmms::AiBridgeCopiedNote> notes;
		const auto result = lmms::AiBridgeProjectActions::addNotes(QStringLiteral("missing"), notes);
		QCOMPARE(result.code, QStringLiteral("invalid_parameter"));
	}

	void deleteNotesActionContractExists()
	{
		lmms::AiBridgeNoteSelection selection;
		selection.position_start = 0;
		selection.position_end = 120;
		const auto result = lmms::AiBridgeProjectActions::deleteNotes(
			QStringLiteral("missing"), selection);
		QCOMPARE(result.code, QStringLiteral("stale_clip"));
	}

	void transposeNotesActionContractExists()
	{
		lmms::AiBridgeNoteSelection selection;
		selection.key_min = 60;
		selection.key_max = 72;
		const auto result = lmms::AiBridgeProjectActions::transposeNotes(
			QStringLiteral("missing"), selection, 1);
		QCOMPARE(result.code, QStringLiteral("stale_clip"));
	}

	void moveNotesActionContractExists()
	{
		lmms::AiBridgeNoteSelection selection;
		selection.position_start = 0;
		selection.position_end = 120;
		const auto result = lmms::AiBridgeProjectActions::moveNotes(
			QStringLiteral("missing"), selection, 120);
		QCOMPARE(result.code, QStringLiteral("stale_clip"));
	}

	void duplicateRegionActionContractExists()
	{
		const auto result = lmms::AiBridgeProjectActions::duplicateRegion(
			QStringLiteral("missing"), 0, 480, 480);
		QCOMPARE(result.code, QStringLiteral("stale_clip"));
	}

	void instrumentDiscoveryReturnsCopiedPage()
	{
		const auto result = lmms::AiBridgeProjectActions::listInstruments(0, 16);
		QVERIFY(result.success);
		QCOMPARE(result.page.offset, 0);
		QVERIFY(result.page.limit <= 16);
		for (const auto &instrument : result.instruments)
		{
			QVERIFY(!instrument.instrumentId.isEmpty());
			QVERIFY(!instrument.displayName.isEmpty());
		}
	}

	void trackCreationRejectsInvalidName()
	{
		const auto result = lmms::AiBridgeProjectActions::createInstrumentTrack(QString());
		QCOMPARE(result.code, QStringLiteral("invalid_name"));
	}

	void trackRenameRejectsInvalidName()
	{
		const auto result = lmms::AiBridgeProjectActions::renameTrack(
			QStringLiteral("missing"), QString());
		QCOMPARE(result.code, QStringLiteral("invalid_name"));
	}

	void trackPropertiesRequireAProperty()
	{
		const auto result = lmms::AiBridgeProjectActions::setTrackProperties(
			QStringLiteral("missing"), std::nullopt, std::nullopt);
		QCOMPARE(result.code, QStringLiteral("invalid_parameter"));
	}

	void clipCreationRejectsOutOfRangePosition()
	{
		const auto result = lmms::AiBridgeProjectActions::createMidiClip(
			QStringLiteral("missing"), std::numeric_limits<qint64>::max(),
			std::nullopt, false);
		QCOMPARE(result.code, QStringLiteral("invalid_clip_position"));
	}

};

QTEST_MAIN(AiBridgeActionsTest)
#include "AiBridgeActionsTest.moc"
