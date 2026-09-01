#include <QtTest>

#include "AiBridgeActions.h"

class AiBridgeContextTest : public QObject
{
	Q_OBJECT

private slots:
	void snapshotIsCopiedDataOnly()
	{
		const auto snapshot = lmms::AiBridgeActions::getContextSnapshot();

		QVERIFY(!snapshot.songName.contains("0x"));
		QVERIFY(snapshot.tempo >= 0);
		QVERIFY(snapshot.clipLengthTicks >= 0);
	}
};

QTEST_MAIN(AiBridgeContextTest)
#include "AiBridgeContextTest.moc"
