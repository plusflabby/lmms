#include <QtTest>

#include <algorithm>
#include <type_traits>
#include <utility>

#include "AiBridgeAutomationContext.h"
#include "AiBridgeAutomationReconciler.h"
#include "AiBridgeProjectContext.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "Song.h"
#include "Mixer.h"
#include "AiBridgeMixerRegistry.h"
#include "Effect.h"

class AiBridgeAutomationReconcilerTest : public QObject
{
	Q_OBJECT

private slots:
	void initTestCase()
	{
		lmms::Engine::init(true);
	}

	void cleanupTestCase()
	{
		lmms::Engine::destroy();
	}

	void registersStableInstrumentTrackTargets()
	{
		auto *track = new lmms::InstrumentTrack(lmms::Engine::getSong());
		lmms::Engine::getSong()->addTrack(track);
		lmms::AiBridgeAutomationTargets targets(QStringLiteral("session"), 1);
		lmms::AiBridgeAutomationRegistry registry(QStringLiteral("session"), 1);
		lmms::AiBridgeProjectContext project;
		lmms::AiBridgeAutomationReconciler reconciler(project, registry, targets);

		const auto before = targets.all();
		reconciler.reconcile();
		const auto first = targets.all();
		QVERIFY(first.size() > before.size());

		lmms::AiBridgePage page;
		page.limit = 64;
		const auto tracks = project.tracks(page);
		const auto instrument = std::find_if(tracks.cbegin(), tracks.cend(), [](const auto &track) {
			return track.type == QStringLiteral("instrument");
		});
		QVERIFY(instrument != tracks.cend());

		const auto owned = targets.targetsForOwner(instrument->id);
		QCOMPARE(owned.size(), 2);
		QVERIFY(std::all_of(owned.cbegin(), owned.cend(), [](const auto &target) {
			return target.capability == QStringLiteral("read_write")
				&& (target.parameterId == QStringLiteral("volume")
					|| target.parameterId == QStringLiteral("panning"));
		}));

		reconciler.reconcile();
		const auto second = targets.all();
		QCOMPARE(second.size(), first.size());
		QCOMPARE(targets.targetsForOwner(instrument->id).size(), owned.size());
		lmms::Engine::getSong()->removeTrack(track);
		delete track;
		reconciler.reconcile();
		QCOMPARE(targets.targetsForOwner(instrument->id).size(), 0);
	}

	void registersStableMixerChannelTargets()
	{
		const auto channelIndex = lmms::Engine::mixer()->createChannel();
		auto *channel = lmms::Engine::mixer()->mixerChannel(channelIndex);
		lmms::AiBridgeAutomationTargets targets(QStringLiteral("session"), 1);
		lmms::AiBridgeAutomationRegistry registry(QStringLiteral("session"), 1);
		lmms::AiBridgeProjectContext project;
		lmms::AiBridgeAutomationReconciler reconciler(project, registry, targets);

		reconciler.reconcile();
		const auto owner = lmms::aiBridgeMixerRegistry().channelId(channel->lifetimeToken());
		const auto owned = targets.targetsForOwner(owner);
		QCOMPARE(owned.size(), 3);
		QVERIFY(std::all_of(owned.cbegin(), owned.cend(), [](const auto &target) {
			return target.capability == QStringLiteral("read_write")
				&& (target.parameterId == QStringLiteral("volume")
					|| target.parameterId == QStringLiteral("mute")
					|| target.parameterId == QStringLiteral("solo"));
		}));
		const auto first = targets.all();
		reconciler.reconcile();
		QCOMPARE(targets.all().size(), first.size());
		QCOMPARE(targets.targetsForOwner(owner).size(), owned.size());

		lmms::Engine::mixer()->deleteChannel(channelIndex);
		reconciler.reconcile();
		QCOMPARE(targets.targetsForOwner(owner).size(), 0);
	}

	void registersStableMixerRouteTarget()
	{
		const auto sourceIndex = lmms::Engine::mixer()->createChannel();
		const auto destinationIndex = lmms::Engine::mixer()->createChannel();
		auto *source = lmms::Engine::mixer()->mixerChannel(sourceIndex);
		auto *destination = lmms::Engine::mixer()->mixerChannel(destinationIndex);
		auto *route = lmms::Engine::mixer()->createRoute(source, destination, 0.5f);
		const auto owner = lmms::aiBridgeMixerRegistry().routeId(route);
		lmms::AiBridgeAutomationTargets targets(QStringLiteral("session"), 1);
		lmms::AiBridgeAutomationRegistry registry(QStringLiteral("session"), 1);
		lmms::AiBridgeProjectContext project;
		lmms::AiBridgeAutomationReconciler reconciler(project, registry, targets);

		reconciler.reconcile();
		const auto owned = targets.targetsForOwner(owner);
		QCOMPARE(owned.size(), 1);
		QCOMPARE(owned.first().parameterId, QStringLiteral("amount"));
		QCOMPARE(owned.first().valueType, QStringLiteral("float"));
		QCOMPARE(owned.first().minimum, 0.0f);
		QCOMPARE(owned.first().maximum, 1.0f);
		const auto id = owned.first().automationTargetId;
		reconciler.reconcile();
		QCOMPARE(targets.targetsForOwner(owner).first().automationTargetId, id);

		lmms::Engine::mixer()->deleteChannelSend(route);
		reconciler.reconcile();
		QCOMPARE(targets.targetsForOwner(owner).size(), 0);
		lmms::Engine::mixer()->deleteChannel(destinationIndex);
		lmms::Engine::mixer()->deleteChannel(sourceIndex);
	}

	void effectCommonControlsExposeNarrowNativeModels()
	{
		static_assert(std::is_same_v<decltype(std::declval<lmms::Effect &>().enabledModel()), lmms::BoolModel *>);
		static_assert(std::is_same_v<decltype(std::declval<lmms::Effect &>().wetDryModel()), lmms::FloatModel *>);
		QVERIFY(true);
	}

	void retiresStaleEffectParameterOwners()
	{
		lmms::AiBridgeAutomationTargets targets(QStringLiteral("session"), 1);
		lmms::AiBridgeAutomationTargetSnapshot metadata;
		metadata.valueType = QStringLiteral("float");
		metadata.capability = QStringLiteral("read_write");
		targets.registerTarget(QStringLiteral("stale-effect"),
			QStringLiteral("effect_parameter"), QStringLiteral("volume"), metadata);
		lmms::AiBridgeAutomationRegistry registry(QStringLiteral("session"), 1);
		lmms::AiBridgeProjectContext project;
		lmms::AiBridgeAutomationReconciler reconciler(project, registry, targets);
		reconciler.reconcile();
		QCOMPARE(targets.targetsForOwner(QStringLiteral("stale-effect")).size(), 0);
	}
};

QTEST_MAIN(AiBridgeAutomationReconcilerTest)
#include "AiBridgeAutomationReconcilerTest.moc"
