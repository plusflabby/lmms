#include "AiBridgeAutomationReconciler.h"

#include "AiBridgeEffectAdapters.h"
#include "AiBridgeProjectContext.h"
#include "Engine.h"
#include "Effect.h"
#include "InstrumentTrack.h"
#include "SampleTrack.h"
#include "Mixer.h"
#include "Song.h"

#include <QSet>

namespace lmms
{

AiBridgeAutomationReconciler::AiBridgeAutomationReconciler(AiBridgeProjectContext &project,
	AiBridgeAutomationRegistry &registry, AiBridgeAutomationTargets &targets,
	AiBridgeMixerRegistry &mixerRegistry, AiBridgeEffectRegistry &effectRegistry) :
	m_project(project), m_registry(registry), m_targets(targets), m_mixerRegistry(mixerRegistry), m_effectRegistry(effectRegistry)
{
}

void AiBridgeAutomationReconciler::reconcile()
{
	if (!Engine::getSong()) return;
	QSet<QString> liveTrackOwners;
	for (auto *base : Engine::getSong()->tracks())
	{
		auto *track = dynamic_cast<InstrumentTrack *>(base);
		auto *sample = dynamic_cast<SampleTrack *>(base);
		if (!track && !sample) continue;
		auto *volumeModel = track ? track->volumeModel() : sample->volumeModel();
		auto *panningModel = track ? track->panningModel() : sample->panningModel();
		const auto ownerId = m_project.idForTrack(base);
		liveTrackOwners.insert(ownerId);

		AiBridgeAutomationTargetSnapshot volume;
		volume.valueType = sample ? QStringLiteral("float") : QStringLiteral("integer");
		volume.minimum = MinVolume;
		volume.maximum = MaxVolume;
		volume.defaultValue = DefaultVolume;
		volume.step = sample ? 0.1f : 1.0f;
		volume.unit = QStringLiteral("percent");
		volume.capability = QStringLiteral("read_write");
		m_targets.registerTarget(ownerId, QStringLiteral("track_volume"),
			QStringLiteral("volume"), volume, volumeModel);

		AiBridgeAutomationTargetSnapshot panning;
		panning.valueType = sample ? QStringLiteral("float") : QStringLiteral("integer");
		panning.minimum = PanningLeft;
		panning.maximum = PanningRight;
		panning.defaultValue = DefaultPanning;
		panning.step = sample ? 0.1f : 1.0f;
		panning.unit = QStringLiteral("percent");
		panning.capability = QStringLiteral("read_write");
		m_targets.registerTarget(ownerId, QStringLiteral("track_pan"),
			QStringLiteral("panning"), panning, panningModel);
	}

	for (auto *channel : Engine::mixer()->mixerChannels())
	{
		const auto ownerId = channel->isMaster() ? m_mixerRegistry.masterId()
			: m_mixerRegistry.channelId(channel->lifetimeToken());
		liveTrackOwners.insert(ownerId);

		AiBridgeAutomationTargetSnapshot volume;
		volume.valueType = QStringLiteral("float");
		volume.minimum = 0.0f;
		volume.maximum = 2.0f;
		volume.defaultValue = 1.0f;
		volume.step = 0.001f;
		volume.unit = QStringLiteral("gain");
		volume.capability = QStringLiteral("read_write");
		m_targets.registerTarget(ownerId, QStringLiteral("mixer_volume"),
			QStringLiteral("volume"), volume, &channel->m_volumeModel);

		for (const auto &parameter : {QStringLiteral("mute"), QStringLiteral("solo")})
		{
			AiBridgeAutomationTargetSnapshot state;
			state.valueType = QStringLiteral("boolean");
			state.minimum = 0.0f;
			state.maximum = 1.0f;
			state.defaultValue = 0.0f;
			state.step = 1.0f;
			state.capability = QStringLiteral("read_write");
			state.unit = QString();
			m_targets.registerTarget(ownerId,
				parameter == QStringLiteral("mute") ? QStringLiteral("mixer_mute") : QStringLiteral("mixer_solo"),
				parameter, state, parameter == QStringLiteral("mute") ?
					static_cast<QObject *>(&channel->m_muteModel) : static_cast<QObject *>(&channel->m_soloModel));
		}
	}

	for (auto *route : Engine::mixer()->m_mixerRoutes)
	{
		const auto ownerId = m_mixerRegistry.routeId(route);
		liveTrackOwners.insert(ownerId);
		AiBridgeAutomationTargetSnapshot amount;
		amount.valueType = QStringLiteral("float");
		amount.minimum = 0.0f;
		amount.maximum = 1.0f;
		amount.defaultValue = 1.0f;
		amount.step = 0.001f;
		amount.unit = QStringLiteral("gain");
		amount.capability = QStringLiteral("read_write");
		m_targets.registerTarget(ownerId, QStringLiteral("mixer_route_amount"),
			QStringLiteral("amount"), amount, route->amount());
	}

	for (auto *channel : Engine::mixer()->mixerChannels())
	{
		const auto channelId = channel->isMaster() ? m_mixerRegistry.masterId()
			: m_mixerRegistry.channelId(channel->lifetimeToken());
		for (int position = 0; position < channel->m_fxChain.effectCount(); ++position)
		{
			auto *effect = channel->m_fxChain.effectAt(position);
			const auto ownerId = m_effectRegistry.effectId(dynamic_cast<QObject *>(effect), channelId);
			liveTrackOwners.insert(ownerId);

			AiBridgeAutomationTargetSnapshot enabled;
			enabled.valueType = QStringLiteral("boolean");
			enabled.minimum = 0.0f;
			enabled.maximum = 1.0f;
			enabled.defaultValue = 1.0f;
			enabled.step = 1.0f;
			enabled.capability = QStringLiteral("read_write");
			m_targets.registerTarget(ownerId, QStringLiteral("effect_enabled"),
				QStringLiteral("enabled"), enabled, effect->enabledModel());

			AiBridgeAutomationTargetSnapshot wetDry;
			wetDry.valueType = QStringLiteral("float");
			wetDry.minimum = -1.0f;
			wetDry.maximum = 1.0f;
			wetDry.defaultValue = 1.0f;
			wetDry.step = 0.01f;
			wetDry.unit = QStringLiteral("mix");
			wetDry.capability = QStringLiteral("read_write");
			m_targets.registerTarget(ownerId, QStringLiteral("effect_wet_dry"),
				QStringLiteral("wet_dry"), wetDry, effect->wetDryModel());

			const auto effectId = QString::fromUtf8(effect->descriptor()->name);
			for (const auto &parameter : AiBridgeEffectAdapters::parameters(effectId,
				effect->controls()))
			{
				auto *model = AiBridgeEffectAdapters::modelForParameter(effectId,
					effect->controls(), parameter.id);
				if (!model) continue;
				AiBridgeAutomationTargetSnapshot target;
				switch (parameter.type)
				{
					case AiBridgeEffectParameterType::Float: target.valueType = QStringLiteral("float"); break;
					case AiBridgeEffectParameterType::Integer: target.valueType = QStringLiteral("integer"); break;
					case AiBridgeEffectParameterType::Boolean: target.valueType = QStringLiteral("boolean"); break;
					case AiBridgeEffectParameterType::Enum: target.valueType = QStringLiteral("enum"); break;
				}
				target.minimum = parameter.minimum;
				target.maximum = parameter.maximum;
				target.defaultValue = parameter.defaultValue;
				target.step = parameter.step;
				target.unit = parameter.unit;
				target.capability = parameter.writable ? QStringLiteral("read_write")
					: QStringLiteral("read_only");
				m_targets.registerTarget(ownerId, QStringLiteral("effect_parameter"),
					parameter.id, target, model);
			}
		}
	}

	QSet<QString> retiredOwners;
	for (const auto &target : m_targets.all())
	{
		if ((target.semanticClass.startsWith(QStringLiteral("track_"))
			|| target.semanticClass.startsWith(QStringLiteral("mixer_"))
			|| target.semanticClass.startsWith(QStringLiteral("route_"))
			|| target.semanticClass.startsWith(QStringLiteral("effect_")))
			&& !liveTrackOwners.contains(target.ownerId))
			retiredOwners.insert(target.ownerId);
	}
	for (const auto &ownerId : retiredOwners) m_targets.retireOwner(ownerId);
}

} // namespace lmms
