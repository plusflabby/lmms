#include <QtTest>

#include "AiBridgeTimelineActions.h"

class AiBridgeTimelineActionsTest : public QObject
{
	Q_OBJECT

private slots:
	void validatesEditsBeforeNativeAccess()
	{
		lmms::AiBridgeTimelineEdit edit;
		edit.clipId = QStringLiteral("clip:one");
		edit.startTick = 480;
		edit.lengthTicks = 960;
		QVERIFY(lmms::AiBridgeTimelineActions::validateEdit(edit).success());
		edit.startTick = -1;
		QCOMPARE(lmms::AiBridgeTimelineActions::validateEdit(edit).code, QStringLiteral("invalid_clip_position"));
		edit.startTick = 0;
		edit.autoResize = true;
		QCOMPARE(lmms::AiBridgeTimelineActions::validateEdit(edit).code, QStringLiteral("invalid_clip_length"));
	}

	void rejectsDuplicateBatchMembers()
	{
		lmms::AiBridgeTimelineEdit first;
		first.clipId = QStringLiteral("clip:one");
		first.startTick = 0;
		lmms::AiBridgeTimelineEdit second = first;
		const auto result = lmms::AiBridgeTimelineActions::batchEdit({first, second});
		QVERIFY(!result.success);
		QCOMPARE(result.code, QStringLiteral("duplicate_clip"));
	}

	void staleIdsAreRejected()
	{
		const auto result = lmms::AiBridgeTimelineActions::setClip({QStringLiteral("stale"), 0, std::nullopt, std::nullopt});
		QVERIFY(!result.success);
		QCOMPARE(result.code, QStringLiteral("stale_clip"));
	}
};

QTEST_MAIN(AiBridgeTimelineActionsTest)
#include "AiBridgeTimelineActionsTest.moc"
