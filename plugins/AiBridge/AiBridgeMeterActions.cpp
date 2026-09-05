#include "AiBridgeMeterActions.h"

#include "AiBridgeMixerContext.h"
#include "AiBridgeMixerRegistry.h"
#include "Engine.h"
#include "Mixer.h"
#include <QApplication>
#include <QThread>

namespace lmms
{
AiBridgeMeterActionResult AiBridgeMeterActions::get(const std::optional<QVector<QString>>& requested, int offset, int limit)
{
	AiBridgeMeterActionResult result;
	if (!qApp || QThread::currentThread() != qApp->thread()) { result.code = "wrong_thread"; result.message = "meter read requires Qt application thread"; return result; }
	if (!validPage(offset, limit, AiBridgeMixerLimits::maxMeterChannels)) { result.code = "invalid_parameter"; result.message = "invalid meter page"; return result; }
	const auto* mixer = Engine::mixer();
	if (!mixer) { result.code = "ipc_unavailable"; result.message = "mixer is unavailable"; return result; }
	auto &registry = aiBridgeMixerRegistry();
	QVector<MixerChannel*> channels;
	if (requested.has_value()) {
		if (requested->size() > AiBridgeMixerLimits::maxMeterChannels) { result.code = "invalid_parameter"; result.message = "too many meter channels"; return result; }
		for (const auto& id : *requested) {
			AiBridgeMixerContext context(registry);
			auto* channel = context.liveChannel(id);
			if (!channel) { result.code = "stale_mixer_channel"; result.message = "meter channel is not live"; return result; }
			channels.append(channel);
		}
	} else {
		for (auto* channel : mixer->mixerChannels()) channels.append(channel);
	}
	const int total = channels.size();
	result.success = true; result.code = "ok"; result.message = "meters read";
	result.page.page = {total, offset, limit, offset + limit < total};
	for (int i = offset; i < std::min(offset + limit, total); ++i) {
		auto* channel = channels.at(i);
		result.page.items.append({registry.channelId(channel->lifetimeToken(), channel->isMaster()), channel->bridgePeakLeft(), channel->bridgePeakRight()});
	}
	return result;
}
}
