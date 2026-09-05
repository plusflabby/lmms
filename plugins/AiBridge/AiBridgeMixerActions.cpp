#include "AiBridgeMixerActions.h"
#include "AiBridgeProjectContext.h"

#include "Engine.h"
#include "InstrumentTrack.h"
#include "Mixer.h"
#include "SampleTrack.h"
#include "Song.h"
#include "Track.h"

#include <QApplication>
#include <QThread>

#include <cmath>

namespace lmms
{
namespace
{
AiBridgeMixerActionResult failure(const QString& code, const QString& message)
{
	return {false, code, message, {}, {}};
}

bool qtThread()
{
	return qApp != nullptr && QThread::currentThread() == qApp->thread();
}

AiBridgeMixerActionResult copiedRoute(AiBridgeMixerRegistry& mixerRegistry, MixerRoute* route,
	const QString& message)
{
	if (!route) return failure(QStringLiteral("stale_mixer_route"), QStringLiteral("mixer route is not live"));
	auto result = failure(QStringLiteral("ok"), message);
	result.success = true;
	result.route.mixerRouteId = mixerRegistry.routeId(route);
	result.route.sourceChannelId = mixerRegistry.channelId(route->sender()->lifetimeToken(), route->sender()->isMaster());
	result.route.destinationChannelId = mixerRegistry.channelId(route->receiver()->lifetimeToken(), route->receiver()->isMaster());
	result.route.amount = route->amount()->value();
	return result;
}

AiBridgeMixerRegistry& registry()
{
	return aiBridgeMixerRegistry();
}

AiBridgeMixerActionResult copiedResult(AiBridgeMixerContext& context, MixerChannel* channel)
{
	const auto channels = context.snapshot(0, AiBridgeMixerLimits::maxChannels).channelItems;
	for (const auto& copied : channels)
	{
		if (copied.mixerChannelId == registry().channelId(channel->lifetimeToken(), channel->isMaster()))
		{
			auto result = failure(QStringLiteral("ok"), QStringLiteral("mixer channel updated"));
			result.success = true;
			result.mixerChannelId = copied.mixerChannelId;
			result.channel = copied;
			return result;
		}
	}
	return failure(QStringLiteral("stale_mixer_channel"), QStringLiteral("mixer channel is not live"));
}
}

QString AiBridgeMixerActions::validateChannelProperties(
	const AiBridgeMixerChannelProperties& properties)
{
	if (!properties.name && !properties.volume && !properties.mute && !properties.solo)
	{
		return QStringLiteral("invalid_parameter");
	}
	if (properties.name && (properties.name->isEmpty() || properties.name->toUtf8().size() > 256))
	{
		return QStringLiteral("invalid_name");
	}
	if (properties.volume && (*properties.volume < AiBridgeMixerLimits::minChannelVolume ||
		*properties.volume > AiBridgeMixerLimits::maxChannelVolume || !std::isfinite(*properties.volume)))
	{
		return QStringLiteral("invalid_parameter");
	}
	return {};
}

AiBridgeMixerActionResult AiBridgeMixerActions::createChannel(const std::optional<QString>& name)
{
	if (!qtThread()) return failure(QStringLiteral("wrong_thread"), QStringLiteral("mixer mutation requires Qt application thread"));
	if (name && (name->isEmpty() || name->toUtf8().size() > 256)) return failure(QStringLiteral("invalid_name"), QStringLiteral("name is invalid"));
	auto* mixer = Engine::mixer();
	if (!mixer) return failure(QStringLiteral("no_mixer"), QStringLiteral("mixer is unavailable"));
	const int index = mixer->createChannel();
	auto* channel = mixer->mixerChannel(index);
	if (name) channel->m_name = *name;
	Engine::getSong()->setModified();
	AiBridgeMixerContext context(registry());
	return copiedResult(context, channel);
}

AiBridgeMixerActionResult AiBridgeMixerActions::deleteChannel(const QString& id)
{
	if (!qtThread()) return failure(QStringLiteral("wrong_thread"), QStringLiteral("mixer mutation requires Qt application thread"));
	auto* mixer = Engine::mixer();
	if (!mixer) return failure(QStringLiteral("no_mixer"), QStringLiteral("mixer is unavailable"));
	AiBridgeMixerContext context(registry());
	auto* channel = context.liveChannel(id);
	if (!channel) return failure(QStringLiteral("stale_mixer_channel"), QStringLiteral("mixer channel is not live"));
	if (channel->isMaster()) return failure(QStringLiteral("master_operation_not_allowed"), QStringLiteral("Master cannot be deleted"));
	for (auto* track : Engine::getSong()->tracks())
	{
		if (auto* instrument = dynamic_cast<InstrumentTrack*>(track); instrument && instrument->mixerChannelModel()->value() == channel->index())
			return failure(QStringLiteral("channel_in_use"), QStringLiteral("a track targets this channel"));
		if (auto* sample = dynamic_cast<SampleTrack*>(track); sample && sample->mixerChannelModel()->value() == channel->index())
			return failure(QStringLiteral("channel_in_use"), QStringLiteral("a track targets this channel"));
	}
	for (const auto* route : mixer->m_mixerRoutes)
	{
		if (route->sender() == channel || route->receiver() == channel)
			return failure(QStringLiteral("channel_in_use"), QStringLiteral("a route depends on this channel"));
	}
	const auto oldId = id;
	mixer->deleteChannel(channel->index());
	registry().retireChannel(oldId);
	Engine::getSong()->setModified();
	return {true, QStringLiteral("ok"), QStringLiteral("mixer channel deleted"), oldId, {}};
}

AiBridgeMixerActionResult AiBridgeMixerActions::setChannelProperties(
	const QString& id, const AiBridgeMixerChannelProperties& properties)
{
	if (!qtThread()) return failure(QStringLiteral("wrong_thread"), QStringLiteral("mixer mutation requires Qt application thread"));
	const auto validation = validateChannelProperties(properties);
	if (!validation.isEmpty()) return failure(validation, QStringLiteral("channel properties are invalid"));
	AiBridgeMixerContext context(registry());
	auto* channel = context.liveChannel(id);
	if (!channel) return failure(QStringLiteral("stale_mixer_channel"), QStringLiteral("mixer channel is not live"));
	if (channel->isMaster() && properties.name) return failure(QStringLiteral("master_operation_not_allowed"), QStringLiteral("Master cannot be renamed"));
	if (properties.name) channel->m_name = *properties.name;
	if (properties.volume) channel->m_volumeModel.setValue(*properties.volume);
	if (properties.mute) channel->m_muteModel.setValue(*properties.mute);
	if (properties.solo) channel->m_soloModel.setValue(*properties.solo);
	Engine::getSong()->setModified();
	return copiedResult(context, channel);
}

AiBridgeMixerActionResult AiBridgeMixerActions::routeTrackToChannel(
	const QString& trackId, const QString& mixerChannelId)
{
	if (!qtThread()) return failure(QStringLiteral("wrong_thread"), QStringLiteral("mixer routing requires Qt application thread"));
	AiBridgeProjectContext projectContext;
	auto* track = projectContext.resolveTrack(trackId);
	if (!track) return failure(QStringLiteral("stale_track"), QStringLiteral("track is not live"));
	IntModel* model = nullptr;
	if (auto* instrument = dynamic_cast<InstrumentTrack*>(track)) model = instrument->mixerChannelModel();
	else if (auto* sample = dynamic_cast<SampleTrack*>(track)) model = sample->mixerChannelModel();
	else return failure(QStringLiteral("unsupported_track"), QStringLiteral("track type cannot be routed"));
	AiBridgeMixerContext context(registry());
	auto* channel = context.liveChannel(mixerChannelId);
	if (!channel) return failure(QStringLiteral("stale_mixer_channel"), QStringLiteral("mixer channel is not live"));
	if (!isValidMixerChannelIndex(channel->index(), Engine::mixer()->numChannels()))
		return failure(QStringLiteral("invalid_parameter"), QStringLiteral("mixer channel index is invalid"));
	model->setRange(0, Engine::mixer()->numChannels() - 1);
	model->setValue(channel->index());
	Engine::getSong()->setModified();
	auto result = copiedResult(context, channel);
	if (result.success) result.trackId = trackId;
	return result;
}

bool AiBridgeMixerActions::isValidMixerChannelIndex(int channelIndex, int channelCount)
{
	return channelCount > 0 && channelIndex >= 0 && channelIndex < channelCount;
}

AiBridgeMixerActionResult AiBridgeMixerActions::createRoute(const QString& sourceId,
	const QString& destinationId, float amount)
{
	if (!qtThread()) return failure(QStringLiteral("wrong_thread"), QStringLiteral("mixer routing requires Qt application thread"));
	if (!std::isfinite(amount) || amount < AiBridgeMixerLimits::minRouteAmount || amount > AiBridgeMixerLimits::maxRouteAmount)
		return failure(QStringLiteral("invalid_parameter"), QStringLiteral("route amount is outside [0,1]"));
	auto* mixer = Engine::mixer();
	AiBridgeMixerContext context(registry());
	auto* source = context.liveChannel(sourceId);
	auto* destination = context.liveChannel(destinationId);
	if (!source || !destination) return failure(QStringLiteral("stale_mixer_channel"), QStringLiteral("route endpoint is not live"));
	if (source == destination) return failure(QStringLiteral("invalid_route"), QStringLiteral("self-routes are not allowed"));
	for (auto* route : mixer->m_mixerRoutes)
		if (route->sender() == source && route->receiver() == destination)
			return failure(QStringLiteral("duplicate_route"), QStringLiteral("route already exists"));
	if (mixer->isInfiniteLoop(source->index(), destination->index()))
		return failure(QStringLiteral("route_cycle"), QStringLiteral("route would create a cycle"));
	auto* route = mixer->createChannelSend(source->index(), destination->index(), amount);
	return copiedRoute(registry(), route, QStringLiteral("mixer route created"));
}

AiBridgeMixerActionResult AiBridgeMixerActions::deleteRoute(const QString& routeId)
{
	if (!qtThread()) return failure(QStringLiteral("wrong_thread"), QStringLiteral("mixer routing requires Qt application thread"));
	auto* route = registry().liveRoute(routeId);
	if (!route) return failure(QStringLiteral("stale_mixer_route"), QStringLiteral("mixer route is not live"));
	Engine::mixer()->deleteChannelSend(route);
	registry().retireRoute(routeId);
	return {true, QStringLiteral("ok"), QStringLiteral("mixer route deleted"), {}, {}};
}

AiBridgeMixerActionResult AiBridgeMixerActions::setRouteProperties(const QString& routeId,
	const AiBridgeMixerRouteProperties& properties)
{
	if (!qtThread()) return failure(QStringLiteral("wrong_thread"), QStringLiteral("mixer routing requires Qt application thread"));
	if (!properties.amount || !std::isfinite(*properties.amount) || *properties.amount < AiBridgeMixerLimits::minRouteAmount || *properties.amount > AiBridgeMixerLimits::maxRouteAmount)
		return failure(QStringLiteral("invalid_parameter"), QStringLiteral("route amount must be in [0,1]"));
	auto* route = registry().liveRoute(routeId);
	if (!route) return failure(QStringLiteral("stale_mixer_route"), QStringLiteral("mixer route is not live"));
	route->amount()->setValue(*properties.amount);
	Engine::getSong()->setModified();
	return copiedRoute(registry(), route, QStringLiteral("mixer route updated"));
}

} // namespace lmms
