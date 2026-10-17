#ifndef LMMS_AIBRIDGE_AUDIO_CONTEXT_H
#define LMMS_AIBRIDGE_AUDIO_CONTEXT_H
#include "AiBridgeAudioTypes.h"
#include "AiBridgeProjectContext.h"
#include <QJsonObject>
namespace lmms {
struct AiBridgeAudioClipSnapshot
{
	AiBridgeAudioClipState state;
	QString name, sourceLabel, stateHash;
	quint64 generation = 0;
	qint64 endTick = 0, ticksPerBar = 0, ticksPerBeat = 0;
	bool sourceVerified = false;
	std::optional<SampleDecoder::PcmWaveInfo> verifiedSourceInfo;
};
struct AiBridgeAudioTrackSnapshot
{
	QString id, name;
	double volume = 0, panning = 0;
	bool muted = false;
	int fxChannel = 0, clipCount = 0;
};
struct AiBridgeAudioClipResult { QString code; std::optional<AiBridgeAudioClipSnapshot> value; };
struct AiBridgeAudioTrackPage { QString code; AiBridgePage page; std::vector<AiBridgeAudioTrackSnapshot> items; };
struct AiBridgeAudioClipPage { QString code; AiBridgePage page; std::vector<AiBridgeAudioClipSnapshot> items; };
class AiBridgeAudioContext
{
public:
	AiBridgeAudioClipResult clip(const QString& id) const;
	QString validate(const QString& id, const QString& expectedAudioStateHash) const;
	AiBridgeAudioTrackPage tracks(int offset, int limit) const;
	AiBridgeAudioClipPage clips(const QString& trackId, int offset, int limit) const;
	static QJsonObject toJson(const AiBridgeAudioClipSnapshot& snapshot);
};
}
#endif
