#ifndef LMMS_AIBRIDGE_AUDIO_RANGE_PLANNER_H
#define LMMS_AIBRIDGE_AUDIO_RANGE_PLANNER_H
#include "AiBridgeAudioContext.h"
namespace lmms {
enum class AiBridgeAudioRangeKind { TrimLeft, TrimRight, Split };
struct AiBridgeAudioRangeRequest
{
	AiBridgeAudioRangeKind kind;
	QString clipId, expectedAudioStateHash;
	qint64 boundaryTick;
	bool dryRun=false;
};
struct AiBridgeAudioRangeWorld
{
	AudioWindowWorld nativeWorld;
	std::vector<AiBridgeAudioClipSnapshot> clips;
	quint64 generation=0;
	QString fingerprint;
};
struct AiBridgeAudioRangeStep
{
	enum class Kind { PublishLeft, CreateRight } kind;
	AudioSourceWindowV2 window;
	AudioWindowGeometry geometry;
};
struct AiBridgeAudioRangePlan
{
	QString code, expectedWorldHash;
	AiBridgeAudioClipSnapshot before;
	std::vector<AiBridgeAudioRangeStep> steps;
};
QString audioRangeWorldHash(const AiBridgeAudioRangeWorld&);
AiBridgeAudioRangePlan planAudioRange(const AiBridgeAudioRangeRequest&, const AiBridgeAudioClipSnapshot&, const AiBridgeAudioRangeWorld&);
}
#endif
