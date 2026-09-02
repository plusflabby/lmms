#include <QtTest/QtTest>

#include "AiBridgeProjectActions.h"

class AiBridgeAssetActionsTest : public QObject
{
	Q_OBJECT

private slots:
	void rejectsStaleTrackBeforeAssetLoad()
	{
		const auto result = lmms::AiBridgeProjectActions::loadBundledSample(
			QStringLiteral("track:not-live"), QStringLiteral("sample/audiofileprocessor/a.wav"));
		QVERIFY(!result.success);
		QCOMPARE(result.code, QStringLiteral("stale_track"));
	}
};

QTEST_MAIN(AiBridgeAssetActionsTest)
#include "AiBridgeAssetActionsTest.moc"
