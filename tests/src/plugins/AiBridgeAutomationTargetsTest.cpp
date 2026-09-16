#include <QtTest>

#include "AiBridgeAutomationTargets.h"

class AiBridgeAutomationTargetsTest : public QObject
{
	Q_OBJECT

private slots:
	void registersSupportedSemanticClasses()
	{
		lmms::AiBridgeAutomationTargets targets(QStringLiteral("session"), 1);
		const auto add = [&](const QString &owner, const QString &semantic,
			const QString &parameter, const QString &type, float min, float max,
			float step, const QString &unit) {
			lmms::AiBridgeAutomationTargetSnapshot metadata;
			metadata.valueType = type;
			metadata.minimum = min;
			metadata.maximum = max;
			metadata.step = step;
			metadata.unit = unit;
			metadata.capability = QStringLiteral("read_write");
			return targets.registerTarget(owner, semantic, parameter, metadata);
		};
		const auto trackVolume = add(QStringLiteral("track:1"), QStringLiteral("track_volume"), QStringLiteral("volume"), QStringLiteral("integer"), 0, 200, 1, QStringLiteral("percent"));
		const auto trackPan = add(QStringLiteral("track:1"), QStringLiteral("track_pan"), QStringLiteral("panning"), QStringLiteral("integer"), -100, 100, 1, QStringLiteral("percent"));
		const auto mixerVolume = add(QStringLiteral("mixer:1"), QStringLiteral("mixer_volume"), QStringLiteral("volume"), QStringLiteral("float"), 0, 2, 0.001f, QStringLiteral("gain"));
		const auto mute = add(QStringLiteral("mixer:1"), QStringLiteral("mixer_mute"), QStringLiteral("mute"), QStringLiteral("boolean"), 0, 1, 1, QString());
		const auto route = add(QStringLiteral("route:1"), QStringLiteral("route_amount"), QStringLiteral("amount"), QStringLiteral("float"), 0, 1, 0.001f, QString());
		const auto effectEnabled = add(QStringLiteral("effect:1"), QStringLiteral("effect_enabled"), QStringLiteral("enabled"), QStringLiteral("boolean"), 0, 1, 1, QString());
		const auto effectWetDry = add(QStringLiteral("effect:1"), QStringLiteral("effect_wet_dry"), QStringLiteral("wet_dry"), QStringLiteral("float"), -1, 1, 0.01f, QString());
		const auto effectParameter = add(QStringLiteral("effect:1"), QStringLiteral("effect_parameter"), QStringLiteral("gain"), QStringLiteral("float"), -24, 24, 0.01f, QStringLiteral("dB"));
		for (const auto &id : {trackVolume, trackPan, mixerVolume, mute, route, effectEnabled, effectWetDry, effectParameter})
		{
			QVERIFY(!id.isEmpty());
			QVERIFY(targets.isSupported(id));
		}
		lmms::AiBridgeAutomationTargetSnapshot resolved;
		QVERIFY(targets.resolve(effectWetDry, resolved));
		QCOMPARE(resolved.minimum, -1.0f);
		QCOMPARE(resolved.maximum, 1.0f);
		QCOMPARE(targets.targetsForOwner(QStringLiteral("effect:1")).size(), 3);
	}

	void unsupportedClassesAreDeterministic()
	{
		lmms::AiBridgeAutomationTargets targets(QStringLiteral("session"), 1);
		const auto unsupported = targets.registerUnsupported(QStringLiteral("timeline_marker"), QStringLiteral("native markers are unavailable"));
		QCOMPARE(unsupported.capability, QStringLiteral("unsupported"));
		QVERIFY(!targets.isSupported(unsupported.automationTargetId));
		QVERIFY(targets.registerTarget(QStringLiteral("owner"), QStringLiteral("unsupported"), QStringLiteral("x"), unsupported).isEmpty());
	}

	void ownerRetirementInvalidatesTargets()
	{
		lmms::AiBridgeAutomationTargets targets(QStringLiteral("session"), 1);
		lmms::AiBridgeAutomationTargetSnapshot metadata;
		metadata.capability = QStringLiteral("read_write");
		const auto id = targets.registerTarget(QStringLiteral("effect:1"), QStringLiteral("effect_enabled"), QStringLiteral("enabled"), metadata);
		targets.retireOwner(QStringLiteral("effect:1"));
		lmms::AiBridgeAutomationTargetSnapshot ignored;
		QVERIFY(!targets.resolve(id, ignored));
		targets.retireProject();
		QVERIFY(targets.all().isEmpty());
	}
};

QTEST_MAIN(AiBridgeAutomationTargetsTest)
#include "AiBridgeAutomationTargetsTest.moc"
