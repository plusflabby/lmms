#include <QtTest>
#include <limits>

#include "AiBridgeEffectActions.h"
#include "AiBridgeMixerRegistry.h"
#include "AiBridgeEffectRegistry.h"
#include "Engine.h"

class AiBridgeEffectActionsTest : public QObject
{
	Q_OBJECT

private slots:
	void initTestCase() { lmms::Engine::init(true); }
	void cleanupTestCase() { lmms::Engine::destroy(); }

	void standaloneValidationAndStaleIds()
	{
		using namespace lmms;
		const auto masterId = aiBridgeMixerRegistry().masterId();

		const auto invalidPosition = AiBridgeEffectActions::add(masterId, QStringLiteral("amplifier"), -1);
		QVERIFY(!invalidPosition.success);
		QCOMPARE(invalidPosition.code, QStringLiteral("invalid_parameter"));
		QCOMPARE(invalidPosition.effects.size(), 0);
		const auto invalidUpperPosition = AiBridgeEffectActions::add(masterId, QStringLiteral("amplifier"), 65);
		QVERIFY(!invalidUpperPosition.success);
		QCOMPARE(invalidUpperPosition.code, QStringLiteral("invalid_parameter"));

		const auto unsupported = AiBridgeEffectActions::add(masterId, QStringLiteral("unknown-effect"), 0);
		QVERIFY(!unsupported.success);
		QCOMPARE(unsupported.code, QStringLiteral("effect_not_allowed"));
		QCOMPARE(unsupported.effects.size(), 0);

		const auto staleChannel = AiBridgeEffectActions::add(
			QStringLiteral("mixer-channel:mixer:1:does-not-exist"), QStringLiteral("amplifier"), 0);
		QVERIFY(!staleChannel.success);
		QCOMPARE(staleChannel.code, QStringLiteral("stale_mixer_channel"));

		const auto staleEffect = QStringLiteral("mixer-effect:%1:1:does-not-exist").arg(QStringLiteral("not-live"));
		QCOMPARE(AiBridgeEffectActions::remove(staleEffect).code, QStringLiteral("stale_effect"));
		QCOMPARE(AiBridgeEffectActions::move(staleEffect, 0).code, QStringLiteral("stale_effect"));
		QCOMPARE(AiBridgeEffectActions::setState(staleEffect, false, 0.0f).code, QStringLiteral("stale_effect"));
	}

	void nativeWetDryBounds()
	{
		QVERIFY(lmms::AiBridgeEffectActions::validWetDry(-1.0f));
		QVERIFY(lmms::AiBridgeEffectActions::validWetDry(0.0f));
		QVERIFY(lmms::AiBridgeEffectActions::validWetDry(1.0f));
		QVERIFY(!lmms::AiBridgeEffectActions::validWetDry(-1.01f));
		QVERIFY(!lmms::AiBridgeEffectActions::validWetDry(1.01f));
		QVERIFY(!lmms::AiBridgeEffectActions::validWetDry(std::numeric_limits<float>::quiet_NaN()));
		QVERIFY(!lmms::AiBridgeEffectActions::validWetDry(std::numeric_limits<float>::infinity()));
	}
};

QTEST_MAIN(AiBridgeEffectActionsTest)
#include "AiBridgeEffectActionsTest.moc"
