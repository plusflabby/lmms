#include <QtTest>

#include "AiBridgeMixerRegistry.h"

class AiBridgeMixerRegistryTest : public QObject
{
	Q_OBJECT

private slots:
	void masterIsDeterministicAndProtected()
	{
		lmms::AiBridgeMixerRegistry registry(QStringLiteral("session"), 4);
		QCOMPARE(registry.masterId(), QStringLiteral("mixer-channel:master:session:4"));
		QVERIFY(registry.isLive(registry.masterId()));
		QVERIFY(!registry.isRetired(registry.masterId()));
	}

	void tokenIdentitySurvivesUnrelatedChanges()
	{
		QObject first;
		QObject second;
		lmms::AiBridgeMixerRegistry registry(QStringLiteral("session"), 1);
		const auto firstId = registry.channelId(&first);
		QVERIFY(!firstId.isEmpty());
		QCOMPARE(registry.channelId(&second), QStringLiteral("mixer-channel:session:1:2"));
		QCOMPARE(registry.channelId(&first), firstId);
		QVERIFY(registry.isLive(firstId));
	}

	void destructionRetiresWithoutRetargeting()
	{
		lmms::AiBridgeMixerRegistry registry(QStringLiteral("session"), 1);
		QString oldId;
		{
			QObject deletedToken;
			oldId = registry.channelId(&deletedToken);
		}
		QVERIFY(registry.isRetired(oldId));
		QObject replacement;
		const auto replacementId = registry.channelId(&replacement);
		QVERIFY(replacementId != oldId);
	}

	void explicitRetirementInvalidatesProject()
	{
		QObject token;
		lmms::AiBridgeMixerRegistry registry(QStringLiteral("session"), 7);
		const auto id = registry.channelId(&token);
		registry.retireProject();
		QVERIFY(registry.isRetired(id));
	}
};

QTEST_MAIN(AiBridgeMixerRegistryTest)
#include "AiBridgeMixerRegistryTest.moc"
