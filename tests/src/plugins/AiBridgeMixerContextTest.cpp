#include <QtTest>

#include "AiBridgeMixerContext.h"

class AiBridgeMixerContextTest : public QObject
{
	Q_OBJECT

private slots:
	void invalidPageReturnsEmptyResult()
	{
		lmms::AiBridgeMixerRegistry registry(QStringLiteral("session"), 1);
		lmms::AiBridgeMixerContext context(registry);
		const auto result = context.snapshot(-1, 1);
		QVERIFY(result.channelItems.isEmpty());
		QCOMPARE(result.channels.total, 0);
	}
};

QTEST_MAIN(AiBridgeMixerContextTest)
#include "AiBridgeMixerContextTest.moc"
