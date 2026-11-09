#ifndef LMMS_AI_BRIDGE_VOCAL_EDIT_PLANNER_H
#define LMMS_AI_BRIDGE_VOCAL_EDIT_PLANNER_H

#include "AiBridgeVocalEditTypes.h"
#include <QString>
#include <optional>
#include <vector>

namespace lmms {

struct VocalEditWorld
{
	int tempoBpm = 0;
	int outputRate = 0;
	int ticksPerQuarterNote = 48;
	bool tempoAutomationPresent = false;
	int timeSignatureNumerator = 4;
	int timeSignatureDenominator = 4;
};

struct VocalEditSourceSnapshot
{
	QString clipId;
	QString audioStateHash;
	quint64 sourceFrames = 0;
	int sampleRate = 0;
	int channels = 0;
	bool sourceVerified = false;
};

struct VocalEditPlanningSnapshot
{
	quint64 generation = 0;
	QString worldHash;
	VocalEditWorld world;
	std::vector<VocalEditSourceSnapshot> sources;
};

struct VocalEditAnchorPreview
{
	qint64 renderInputFrame = 0;
	qint64 outputFrame = 0;
	qint64 requestedOutputFrame = 0;
	std::optional<qint64> targetTick;
	double roundingErrorFrames = 0.0;
	qint64 roundingErrorNumerator = 0;
	qint64 roundingErrorDenominator = 1;
	double strengthRoundingErrorFrames = 0.0;
	qint64 preAnchorCrossfadeFrames = 0;
	VocalCrossfadeLaw crossfadeLaw = VocalCrossfadeLaw::LinearAmplitude;
	bool copyProtectedInterval = false;
	bool syntheticProtectedBoundary = false;
};

enum class VocalTickFrameRoundingPolicy : quint8
{
	NearestTiesUp = 0
};

struct VocalEditPlan
{
	QString recipeHash;
	QString planHash;
	QString rendererId;
	QString rendererVersion;
	quint64 projectGeneration = 0;
	QString worldHash;
	VocalTickFrameRoundingPolicy roundingPolicy = VocalTickFrameRoundingPolicy::NearestTiesUp;
	std::vector<VocalEditAnchorPreview> resolvedAnchors;
	std::vector<VocalEditAnchorPreview> anchorPreviews;
	bool anchorPreviewTruncated = false;
	size_t omittedAnchorCount = 0;
};

struct VocalEditPlanResult
{
	QString code;
	std::optional<VocalEditPlan> value;
};

QString vocalEditWorldHash(const VocalEditWorld& world);
VocalEditWorld captureVocalEditWorld();

class AiBridgeVocalEditPlanner
{
public:
	static VocalEditPlanResult plan(
		const VocalEditRequest& request,
		const VocalEditPlanningSnapshot& snapshot,
		const VocalEditLimits& limits);
};

} // namespace lmms

#endif
