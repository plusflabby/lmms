#ifndef LMMS_AIBRIDGE_AUDIO_PLANNER_H
#define LMMS_AIBRIDGE_AUDIO_PLANNER_H
#include "AiBridgeAudioContext.h"
#include <QSet>
#include <QJsonArray>
namespace lmms {
enum class AiBridgeAudioEditKind { Move, Duplicate, Delete };
struct AiBridgeAudioEdit
{
	AiBridgeAudioEditKind kind = AiBridgeAudioEditKind::Move;
	QString clipId, expectedAudioStateHash, destinationTrackId;
	std::optional<qint64> position;
};
struct AiBridgeAudioWorld
{
	quint64 generation = 0;
	int tempo = 0;
	qint64 ticksPerBar = 0;
	double outputFramesPerTick = 0;
	bool tempoAutomated = false;
	std::vector<AiBridgeAudioTrackSnapshot> tracks;
	std::vector<AiBridgeAudioClipState> clips;
	QSet<QString> selected;
};
struct AiBridgeAudioPlanStep
{
	AiBridgeAudioEditKind kind;
	int index = 0;
	AiBridgeAudioClipState before;
	std::optional<AiBridgeAudioClipState> after;
	QString beforeWorldHash, afterWorldHash;
	bool changed = true;
};
struct AiBridgeAudioPlan
{
	QString code;
	AiBridgeAudioWorld initial;
	std::vector<AiBridgeAudioPlanStep> steps;
	QJsonArray preview;
	bool previewHasMore = false;
	int affectedClips = 0;
};
QString audioWorldHash(const AiBridgeAudioWorld& world);
class AiBridgeAudioPlanner
{
public:
	static constexpr int MaxAffectedClips = 32, MaxSteps = 256, MaxPreviewRows = 128, MaxCollisionRows = 4096;
	static AiBridgeAudioPlan plan(const AiBridgeAudioWorld& world,const std::vector<AiBridgeAudioEdit>& edits);
};
}
#endif
