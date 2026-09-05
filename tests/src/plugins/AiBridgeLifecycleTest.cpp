#include <QtTest>
#include "AiBridgeMixerRegistry.h"
#include "AiBridgeEffectRegistry.h"

class AiBridgeLifecycleTest : public QObject
{
	Q_OBJECT
private slots:
	void sessionAndRetirementPreventRetargeting()
	{
		QObject oldChannel;
		lmms::AiBridgeMixerRegistry first(QStringLiteral("first"), 1);
		const auto oldId = first.channelId(&oldChannel);
		first.retireProject();
		QObject replacement;
		QVERIFY(first.channelId(&replacement) != oldId);
		lmms::AiBridgeMixerRegistry second(QStringLiteral("second"), 1);
		QVERIFY(second.channelId(&replacement) != oldId);

		QObject oldEffect;
		lmms::AiBridgeEffectRegistry effects(QStringLiteral("first"), 1);
		const auto effectId = effects.effectId(&oldEffect, QStringLiteral("channel"));
		effects.retireProject();
		QVERIFY(effects.resolve(effectId) == nullptr);
	}
};
QTEST_MAIN(AiBridgeLifecycleTest)
#include "AiBridgeLifecycleTest.moc"
