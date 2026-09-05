#ifndef LMMS_AIBRIDGE_MIXER_TYPES_H
#define LMMS_AIBRIDGE_MIXER_TYPES_H

#include <QVector>
#include <QString>

#include <optional>

namespace lmms
{

namespace AiBridgeMixerLimits
{
constexpr int maxChannels = 256;
constexpr int maxRoutes = 512;
constexpr int maxEffectsPerChain = 64;
constexpr int maxCatalogEntries = 64;
constexpr int maxParametersPerEffect = 128;
constexpr int maxParameterChanges = 64;
constexpr int maxMeterChannels = 64;
constexpr float minChannelVolume = 0.0f;
constexpr float maxChannelVolume = 2.0f;
constexpr float channelVolumeStep = 0.001f;
constexpr float minRouteAmount = 0.0f;
constexpr float maxRouteAmount = 1.0f;
constexpr float routeAmountStep = 0.001f;
constexpr float minWetDry = -1.0f;
constexpr float maxWetDry = 1.0f;
constexpr float wetDryStep = 0.01f;
}

struct AiBridgeMixerEffectSummary
{
	QString effectInstanceId;
	QString effectId;
	QString name;
	int position = 0;
};

struct AiBridgeMixerChannelSnapshot
{
	QString mixerChannelId;
	QString name;
	bool master = false;
	float volume = 1.0f;
	bool mute = false;
	bool solo = false;
	QVector<QString> trackIds;
	QVector<AiBridgeMixerEffectSummary> effects;
};

struct AiBridgeMixerRouteSnapshot
{
	QString mixerRouteId;
	QString sourceChannelId;
	QString destinationChannelId;
	float amount = 1.0f;
};

struct AiBridgeMixerPage
{
	int total = 0;
	int offset = 0;
	int limit = 0;
	bool hasMore = false;
};

struct AiBridgeMixerStructureSnapshot
{
	AiBridgeMixerPage channels;
	AiBridgeMixerPage routes;
	QVector<AiBridgeMixerChannelSnapshot> channelItems;
	QVector<AiBridgeMixerRouteSnapshot> routeItems;
};

struct AiBridgeMixerMeter
{
	QString mixerChannelId;
	float leftPeak = 0.0f;
	float rightPeak = 0.0f;
};

struct AiBridgeMixerMeterPage
{
	AiBridgeMixerPage page;
	QVector<AiBridgeMixerMeter> items;
};

struct AiBridgeMixerChannelProperties
{
	std::optional<QString> name;
	std::optional<float> volume;
	std::optional<bool> mute;
	std::optional<bool> solo;
};

struct AiBridgeMixerRouteProperties
{
	std::optional<float> amount;
};

inline bool validPage(int offset, int limit, int maximum)
{
	return offset >= 0 && limit > 0 && limit <= maximum;
}

} // namespace lmms

#endif
