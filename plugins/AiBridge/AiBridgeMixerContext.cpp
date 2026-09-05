#include "AiBridgeMixerContext.h"

#include "Engine.h"
#include "AiBridgeProjectContext.h"
#include "AiBridgeEffectRegistry.h"
#include "Effect.h"
#include "InstrumentTrack.h"
#include "Mixer.h"
#include "SampleTrack.h"
#include "Song.h"

#include <algorithm>

namespace lmms
{

AiBridgeMixerContext::AiBridgeMixerContext(AiBridgeMixerRegistry& registry) :
	m_registry(registry)
{
}

AiBridgeMixerStructureSnapshot AiBridgeMixerContext::snapshot(int offset, int limit)
{
	AiBridgeMixerStructureSnapshot result;
	if (!validPage(offset, limit, AiBridgeMixerLimits::maxChannels))
	{
		return result;
	}
	const auto* mixer = Engine::mixer();
	if (mixer == nullptr)
	{
		return result;
	}
	const auto& channels = mixer->mixerChannels();
	const int total = static_cast<int>(channels.size());
	result.channels = {total, offset, limit, offset + limit < total};
	const int end = std::min(offset + limit, total);
	for (int index = offset; index < end; ++index)
	{
		auto* channel = channels[static_cast<std::size_t>(index)];
		AiBridgeMixerChannelSnapshot copied;
		copied.mixerChannelId = m_registry.channelId(channel->lifetimeToken(), index == 0);
		copied.name = index == 0 ? QStringLiteral("Master") : channel->m_name;
		copied.master = index == 0;
		copied.volume = channel->m_volumeModel.value();
		copied.mute = channel->m_muteModel.value();
		copied.solo = channel->m_soloModel.value();
		for (int position = 0; position < channel->m_fxChain.effectCount(); ++position)
		{
			auto* effect = channel->m_fxChain.effectAt(position);
			if (effect == nullptr) continue;
			AiBridgeMixerEffectSummary effectSummary;
			effectSummary.effectInstanceId = aiBridgeEffectRegistry().effectId(effect, copied.mixerChannelId);
			effectSummary.effectId = QString::fromUtf8(effect->descriptor()->name);
			effectSummary.name = QString::fromUtf8(effect->descriptor()->displayName);
			effectSummary.position = position;
			copied.effects.append(std::move(effectSummary));
		}
		for (auto* track : Engine::getSong()->tracks())
		{
			if (auto* instrument = dynamic_cast<InstrumentTrack*>(track); instrument && instrument->mixerChannelModel()->value() == index)
				copied.trackIds.append(AiBridgeProjectContext().idForTrack(track));
			else if (auto* sample = dynamic_cast<SampleTrack*>(track); sample && sample->mixerChannelModel()->value() == index)
				copied.trackIds.append(AiBridgeProjectContext().idForTrack(track));
		}
		std::sort(copied.trackIds.begin(), copied.trackIds.end());
		result.channelItems.append(std::move(copied));
	}
	const int routeTotal = static_cast<int>(mixer->m_mixerRoutes.size());
	result.routes = {routeTotal, offset, limit, offset + limit < routeTotal};
	const int routeEnd = std::min(offset + limit, routeTotal);
	for (int index = offset; index < routeEnd; ++index)
	{
		auto* route = mixer->m_mixerRoutes[static_cast<std::size_t>(index)];
		AiBridgeMixerRouteSnapshot copied;
		copied.mixerRouteId = m_registry.routeId(route);
		copied.sourceChannelId = m_registry.channelId(route->sender()->lifetimeToken(), route->sender()->isMaster());
		copied.destinationChannelId = m_registry.channelId(route->receiver()->lifetimeToken(), route->receiver()->isMaster());
		copied.amount = route->amount()->value();
		result.routeItems.append(std::move(copied));
	}
	return result;
}

MixerChannel* AiBridgeMixerContext::liveChannel(const QString& id)
{
	const auto* mixer = Engine::mixer();
	if (mixer == nullptr)
	{
		return nullptr;
	}
	const auto& channels = mixer->mixerChannels();
	for (std::size_t index = 0; index < channels.size(); ++index)
	{
		auto* channel = channels[index];
		if (m_registry.channelId(channel->lifetimeToken(), index == 0) == id)
		{
			return channel;
		}
	}
	return nullptr;
}

} // namespace lmms
