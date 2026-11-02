#ifndef LMMS_AI_BRIDGE_VOCAL_EDIT_TYPES_H
#define LMMS_AI_BRIDGE_VOCAL_EDIT_TYPES_H

#include <QString>
#include <QtGlobal>
#include <optional>
#include <variant>
#include <vector>

namespace lmms {

struct VocalEditFrameRange
{
	qint64 inFrame = 0;
	qint64 outFrame = 0;
};

enum class VocalEditSourceBasis : quint8
{
	SourcePcm = 0,
	PlaybackReference = 1
};

struct VocalEditSelection
{
	QString clipId;
	QString expectedAudioHash;
	VocalEditSourceBasis basis = VocalEditSourceBasis::SourcePcm;
	VocalEditFrameRange sourceFrames;
};

struct VocalGainPoint
{
	qint64 renderInputFrame = 0;
	double gainDb = 0.0;
};

struct VocalGainEnvelope
{
	std::vector<VocalGainPoint> points;
};

enum class VocalFadeCurve : quint8
{
	LinearAmplitude = 0,
	EqualPower = 1
};

struct VocalFade
{
	VocalEditFrameRange renderInputFrames;
	double startGainDb = 0.0;
	double endGainDb = 0.0;
	VocalFadeCurve curve = VocalFadeCurve::LinearAmplitude;
};

// Each anchor names a frame in the assembled render-input space and either an
// output frame or a musical tick. Tick conversion is performed later against
// the captured constant-tempo world and its hash.
struct VocalTimingAnchor
{
	qint64 renderInputFrame = 0;
	std::optional<qint64> outputFrame;
	std::optional<qint64> targetTick;
};

struct VocalTimingMap
{
	std::vector<VocalTimingAnchor> anchors;
	std::vector<VocalEditFrameRange> protectedRenderInputFrames;
};

using VocalEditOperation = std::variant<VocalGainEnvelope, VocalFade, VocalTimingMap>;

enum class VocalCrossfadeLaw : quint8
{
	LinearAmplitude = 0,
	EqualPower = 1
};

struct VocalCompSegment
{
	size_t selectionIndex = 0;
	VocalEditFrameRange sourceFrames;
	VocalEditFrameRange outputFrames;
};

struct VocalCompBoundary
{
	size_t leftSegmentIndex = 0;
	size_t rightSegmentIndex = 0;
	qint64 overlapFrames = 0;
	VocalCrossfadeLaw law = VocalCrossfadeLaw::LinearAmplitude;
};

struct VocalCompRecipe
{
	std::vector<VocalCompSegment> segments;
	std::vector<VocalCompBoundary> boundaries;
};

struct VocalDirectInput
{
	size_t selectionIndex = 0;
};

using VocalEditInput = std::variant<VocalDirectInput, VocalCompRecipe>;

struct VocalEditRequest
{
	quint32 recipeVersion = 1;
	quint64 expectedProjectGeneration = 0;
	QString expectedWorldHash;
	std::vector<VocalEditSelection> selections;
	VocalEditInput input;
	std::vector<VocalEditOperation> operations;
};

// Limits come from native capabilities and are included in the immutable plan.
// Numeric DSP ranges are supplied by the caller so a provisional value cannot
// become an accidental wire-schema default.
struct VocalEditLimits
{
	size_t maxSelections = 0;
	size_t maxTimingAnchors = 0;
	size_t maxGainFadePoints = 0;
	size_t maxCompSegments = 0;
	qint64 maxCrossfadeFrames = 0;
	double minimumGainDb = 0.0;
	double maximumGainDb = 0.0;
	double minimumLocalStretchRatio = 0.0;
	double maximumLocalStretchRatio = 0.0;
	double maximumAnchorErrorFrames = 1.0;
};

std::optional<qint64> parseVocalEditFrame(const QString& text);
QString validateVocalEditRequest(const VocalEditRequest& request, const VocalEditLimits& limits);
QString canonicalVocalEditRecipeHash(const VocalEditRequest& request);

} // namespace lmms

#endif
