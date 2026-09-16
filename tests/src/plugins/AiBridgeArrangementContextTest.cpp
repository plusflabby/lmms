#include <QtTest>

#include "AiBridgeArrangementContext.h"

class AiBridgeArrangementContextTest : public QObject
{
	Q_OBJECT

private slots:
	void resolvesDirectTicks()
	{
		qint64 resolved = -1;
		lmms::AiBridgeMusicalPosition position;
		position.tick = 960;
		QVERIFY(lmms::AiBridgeArrangementContext::resolvePosition(position, 1920, 480, resolved).success());
		QCOMPARE(resolved, qint64(960));
	}

	void resolvesOneBasedMusicalPosition()
	{
		qint64 resolved = -1;
		lmms::AiBridgeMusicalPosition position;
		position.bar = 2;
		position.beat = 3;
		position.tickInBeat = 12;
		QVERIFY(lmms::AiBridgeArrangementContext::resolvePosition(position, 1920, 480, resolved).success());
		QCOMPARE(resolved, qint64(2892));
	}

	void rejectsBeatAndTickBounds()
	{
		qint64 resolved = -1;
		lmms::AiBridgeMusicalPosition position;
		position.bar = 1;
		position.beat = 5;
		position.tickInBeat = 0;
		QCOMPARE(lmms::AiBridgeArrangementContext::resolvePosition(position, 1920, 480, resolved).code,
			QStringLiteral("invalid_position"));

		position.beat = 1;
		position.tickInBeat = 480;
		QCOMPARE(lmms::AiBridgeArrangementContext::resolvePosition(position, 1920, 480, resolved).code,
			QStringLiteral("invalid_position"));
	}
};

QTEST_MAIN(AiBridgeArrangementContextTest)
#include "AiBridgeArrangementContextTest.moc"
