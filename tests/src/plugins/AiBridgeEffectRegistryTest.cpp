#include <QtTest>
#include <QObject>
#include "AiBridgeEffectRegistry.h"
class AiBridgeEffectRegistryTest : public QObject
{
	Q_OBJECT
private slots:
	void identitySurvivesReorder()
	{
		lmms::AiBridgeEffectRegistry registry("session", 3); QObject effect;
		const auto id=registry.effectId(&effect, "mixer-channel:c");
		QVERIFY(!id.isEmpty()); QCOMPARE(registry.resolve(id), static_cast<QObject*>(&effect));
		QCOMPARE(registry.effectId(&effect, "mixer-channel:c"), id);
		registry.retire(id); QVERIFY(registry.resolve(id) == nullptr);
	}
};
QTEST_MAIN(AiBridgeEffectRegistryTest)
#include "AiBridgeEffectRegistryTest.moc"
