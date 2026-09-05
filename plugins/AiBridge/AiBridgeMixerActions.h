#ifndef LMMS_AIBRIDGE_MIXER_ACTIONS_H
#define LMMS_AIBRIDGE_MIXER_ACTIONS_H

#include "AiBridgeMixerContext.h"

#include <optional>

namespace lmms
{

class IntModel;

struct AiBridgeMixerActionResult
{
	bool success = false;
	QString code;
	QString message;
	QString mixerChannelId;
	QString trackId;
	AiBridgeMixerChannelSnapshot channel;
	AiBridgeMixerRouteSnapshot route;
};

class AiBridgeMixerActions
{
public:
	static AiBridgeMixerActionResult createChannel(const std::optional<QString>& name);
	static AiBridgeMixerActionResult deleteChannel(const QString& id);
	static AiBridgeMixerActionResult setChannelProperties(
		const QString& id, const AiBridgeMixerChannelProperties& properties);
	static AiBridgeMixerActionResult routeTrackToChannel(
		const QString& trackId, const QString& mixerChannelId);
	static AiBridgeMixerActionResult createRoute(const QString& sourceId,
		const QString& destinationId, float amount);
	static AiBridgeMixerActionResult deleteRoute(const QString& routeId);
	static AiBridgeMixerActionResult setRouteProperties(const QString& routeId,
		const AiBridgeMixerRouteProperties& properties);

	static QString validateChannelProperties(const AiBridgeMixerChannelProperties& properties);
	static bool isValidMixerChannelIndex(int channelIndex, int channelCount);
};

} // namespace lmms

#endif
