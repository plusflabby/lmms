#include <QtTest>

#include "AiBridgeMixerTypes.h"

class AiBridgeMixerTypesTest : public QObject
{
	Q_OBJECT

private slots:
	void nativeBoundsAreExplicit()
	{
		QCOMPARE(lmms::AiBridgeMixerLimits::minChannelVolume, 0.0f);
		QCOMPARE(lmms::AiBridgeMixerLimits::maxChannelVolume, 2.0f);
		QCOMPARE(lmms::AiBridgeMixerLimits::channelVolumeStep, 0.001f);
		QCOMPARE(lmms::AiBridgeMixerLimits::minRouteAmount, 0.0f);
		QCOMPARE(lmms::AiBridgeMixerLimits::maxRouteAmount, 1.0f);
		QCOMPARE(lmms::AiBridgeMixerLimits::routeAmountStep, 0.001f);
	}

	void pageBoundsRejectInvalidValues()
	{
		QVERIFY(lmms::validPage(0, 1, 256));
		QVERIFY(lmms::validPage(256, 256, 256));
		QVERIFY(!lmms::validPage(-1, 1, 256));
		QVERIFY(!lmms::validPage(0, 0, 256));
		QVERIFY(!lmms::validPage(0, 257, 256));
	}

	void pageMetadataIsCopied()
	{
		lmms::AiBridgeMixerStructureSnapshot snapshot;
		snapshot.channels = {12, 4, 8, true};
		snapshot.routes = {3, 0, 3, false};
		QCOMPARE(snapshot.channels.total, 12);
		QCOMPARE(snapshot.channels.offset, 4);
		QCOMPARE(snapshot.channels.limit, 8);
		QVERIFY(snapshot.channels.hasMore);
		QCOMPARE(snapshot.routes.total, 3);
		QVERIFY(!snapshot.routes.hasMore);
	}
};

QTEST_MAIN(AiBridgeMixerTypesTest)
#include "AiBridgeMixerTypesTest.moc"
