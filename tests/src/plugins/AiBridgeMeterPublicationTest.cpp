#include <QtTest>
#include "AiBridgeMeterSnapshot.h"

class AiBridgeMeterPublicationTest : public QObject
{
	Q_OBJECT
private slots:
	void publishesCopiedStereoSnapshot()
	{
		lmms::AiBridgeMeterPublisher publisher;
		QCOMPARE(publisher.read().leftPeak, 0.0f);
		QCOMPARE(publisher.read().rightPeak, 0.0f);
		publisher.publish(0.25f, 0.75f);
		const auto snapshot = publisher.read();
		QCOMPARE(snapshot.leftPeak, 0.25f);
		QCOMPARE(snapshot.rightPeak, 0.75f);
	}
};
QTEST_MAIN(AiBridgeMeterPublicationTest)
#include "AiBridgeMeterPublicationTest.moc"
