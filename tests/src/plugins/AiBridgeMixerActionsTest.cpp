#include <QtTest>

#include "AiBridgeMixerActions.h"
#include "AiBridgeMixerRegistry.h"
#include "Engine.h"
#include "Mixer.h"

class AiBridgeMixerActionsTest : public QObject
{
	Q_OBJECT

private slots:
	void initTestCase() { lmms::Engine::init(true); }
	void cleanupTestCase() { lmms::Engine::destroy(); }

	void validatesNativeChannelProperties()
	{
		lmms::AiBridgeMixerChannelProperties properties;
		QCOMPARE(lmms::AiBridgeMixerActions::validateChannelProperties(properties), QStringLiteral("invalid_parameter"));
		properties.volume = 0.0f;
		QVERIFY(lmms::AiBridgeMixerActions::validateChannelProperties(properties).isEmpty());
		properties.volume = 2.0f;
		QVERIFY(lmms::AiBridgeMixerActions::validateChannelProperties(properties).isEmpty());
		properties.volume = 2.001f;
		QCOMPARE(lmms::AiBridgeMixerActions::validateChannelProperties(properties), QStringLiteral("invalid_parameter"));
	}

	void rejectsInvalidNames()
	{
		lmms::AiBridgeMixerChannelProperties properties;
		properties.name = QString();
		QCOMPARE(lmms::AiBridgeMixerActions::validateChannelProperties(properties), QStringLiteral("invalid_name"));
	}

	void validatesCurrentMixerChannelIndex()
	{
		QVERIFY(lmms::AiBridgeMixerActions::isValidMixerChannelIndex(2, 3));
		QVERIFY(!lmms::AiBridgeMixerActions::isValidMixerChannelIndex(-1, 3));
		QVERIFY(!lmms::AiBridgeMixerActions::isValidMixerChannelIndex(3, 3));
		QVERIFY(!lmms::AiBridgeMixerActions::isValidMixerChannelIndex(0, 0));
	}

	void reportsSuccessfulRouteDeletion()
	{
		auto* mixer = lmms::Engine::mixer();
		const auto source = mixer->createChannel();
		const auto destination = mixer->createChannel();
		auto* route = mixer->createChannelSend(source, destination, 0.5f);
		const auto routeId = lmms::aiBridgeMixerRegistry().routeId(route);
		const auto result = lmms::AiBridgeMixerActions::deleteRoute(routeId);
		QVERIFY(result.success);
		QCOMPARE(result.code, QStringLiteral("ok"));
	}
};

QTEST_MAIN(AiBridgeMixerActionsTest)
#include "AiBridgeMixerActionsTest.moc"
