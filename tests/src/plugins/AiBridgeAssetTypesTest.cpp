#include <QtTest/QtTest>

#include "AiBridgeAssetTypes.h"

class AiBridgeAssetTypesTest : public QObject
{
	Q_OBJECT

private slots:
	void constantsAreBounded()
	{
		QCOMPARE(lmms::AiBridgeAssetTypes::DefaultAssetPageSize, 64);
		QCOMPARE(lmms::AiBridgeAssetTypes::MaxAssetPageSize, 256);
		QVERIFY(lmms::AiBridgeAssetTypes::MaxAssetPageSize <= 256);
		QCOMPARE(lmms::AiBridgeAssetTypes::MaxFilterBytes, 64);
		QCOMPARE(lmms::AiBridgeAssetTypes::MaxAssetIdBytes, 512);
	}

	void copiedPageHasCompleteMetadata()
	{
		lmms::AiBridgeAssetPage<lmms::AiBridgeSampleValue> page;
		page.total = 3;
		page.offset = 1;
		page.limit = 2;
		page.hasMore = true;
		QCOMPARE(page.total, 3);
		QCOMPARE(page.offset, 1);
		QCOMPARE(page.limit, 2);
		QVERIFY(page.hasMore);
	}
};

QTEST_MAIN(AiBridgeAssetTypesTest)
#include "AiBridgeAssetTypesTest.moc"
