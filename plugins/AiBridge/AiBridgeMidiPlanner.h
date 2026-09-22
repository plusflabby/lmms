#ifndef LMMS_AIBRIDGE_MIDI_PLANNER_H
#define LMMS_AIBRIDGE_MIDI_PLANNER_H

#include <optional>

#include "AiBridgeMidiTypes.h"

namespace lmms
{

enum class AiBridgeMidiQuantizeDirection { Nearest, Earlier, Later };
enum class AiBridgeMidiBoundaryPolicy { Clamp, Reject };
enum class AiBridgeMidiHumanizeDistribution { Triangular, Uniform };
enum class AiBridgeMidiGrooveMode { Swing, Template };
enum class AiBridgeMidiValueMode { Set, Offset, Scale, Ramp, GateToNext };
enum class AiBridgeMidiOverlapMode { None, TrimEarlier, Reject };
enum class AiBridgeMidiShortNoteAction { Remove, Extend };
enum class AiBridgeMidiScaleDirection { Nearest, Up, Down };
enum class AiBridgeMidiNearestTieBreak { Down, Up };

struct AiBridgeMidiQuantizeOptions
{
	AiBridgeMidiQuantizeOptions(qint64 grid) : gridTicks(grid) {}
	qint64 gridTicks;
	double strength = 1.0;
	AiBridgeMidiQuantizeDirection direction = AiBridgeMidiQuantizeDirection::Nearest;
	std::optional<qint64> maxShiftTicks;
	AiBridgeMidiBoundaryPolicy boundaryPolicy = AiBridgeMidiBoundaryPolicy::Clamp;
};

struct AiBridgeMidiQuantizePlan
{
	bool success = false;
	QString code;
	QVector<AiBridgeMidiNoteSnapshot> notes;
	int changedCount = 0;
	int skippedCount = 0;
	int stepCount = 0;
};

AiBridgeMidiQuantizePlan planQuantize(const AiBridgeMidiClipSnapshot &snapshot,
	const QVector<int> &canonicalIndices, const AiBridgeMidiQuantizeOptions &options);

struct AiBridgeMidiHumanizeOptions
{
	quint32 seed = 0;
	qint64 timingRangeTicks = 0;
	int volumeRange = 0;
	qint64 lengthRangeTicks = 0;
	AiBridgeMidiHumanizeDistribution distribution = AiBridgeMidiHumanizeDistribution::Triangular;
	AiBridgeMidiBoundaryPolicy boundaryPolicy = AiBridgeMidiBoundaryPolicy::Clamp;
};

struct AiBridgeMidiHumanizePlan
{
	bool success = false;
	QString code;
	QVector<AiBridgeMidiNoteSnapshot> notes;
	int changedCount = 0;
	int clampCount = 0;
};

AiBridgeMidiHumanizePlan planHumanize(const AiBridgeMidiClipSnapshot &snapshot,
	const QVector<int> &canonicalIndices, const AiBridgeMidiHumanizeOptions &options);

struct AiBridgeMidiGrooveOptions
{
	AiBridgeMidiGrooveMode mode = AiBridgeMidiGrooveMode::Swing;
	double strength = 1.0;
	qint64 subdivisionTicks = 0;
	double swingPercent = 50.0;
	qint64 stepTicks = 0;
	QVector<qint64> timingOffsets;
	QVector<int> volumeOffsets;
	AiBridgeMidiBoundaryPolicy boundaryPolicy = AiBridgeMidiBoundaryPolicy::Clamp;
};

AiBridgeMidiHumanizePlan planGroove(const AiBridgeMidiClipSnapshot &snapshot,
	const QVector<int> &canonicalIndices, const AiBridgeMidiGrooveOptions &options);

struct AiBridgeMidiVelocityOptions
{
	AiBridgeMidiValueMode mode = AiBridgeMidiValueMode::Set;
	double value = 0.0;
	int startValue = 0;
	int endValue = 0;
	AiBridgeMidiBoundaryPolicy boundaryPolicy = AiBridgeMidiBoundaryPolicy::Clamp;
};

AiBridgeMidiHumanizePlan planVelocityShape(const AiBridgeMidiClipSnapshot &snapshot,
	const QVector<int> &canonicalIndices, const AiBridgeMidiVelocityOptions &options);

struct AiBridgeMidiLengthOptions
{
	AiBridgeMidiValueMode mode = AiBridgeMidiValueMode::Set;
	double value = 0.0;
	double gatePercent = 100.0;
	std::optional<qint64> fallbackLengthTicks;
	AiBridgeMidiBoundaryPolicy boundaryPolicy = AiBridgeMidiBoundaryPolicy::Clamp;
};

AiBridgeMidiHumanizePlan planLengthShape(const AiBridgeMidiClipSnapshot &snapshot,
	const QVector<int> &canonicalIndices, const AiBridgeMidiLengthOptions &options);

struct AiBridgeMidiCleanupOptions
{
	bool removeExactDuplicates = false;
	AiBridgeMidiOverlapMode samePitchOverlapMode = AiBridgeMidiOverlapMode::None;
	std::optional<qint64> minimumLengthTicks;
	std::optional<AiBridgeMidiShortNoteAction> shortNoteAction;
};

struct AiBridgeMidiCleanupPlan
{
	bool success = false;
	QString code;
	QVector<AiBridgeMidiNoteSnapshot> notes;
	int changedCount = 0;
	int removedCount = 0;
};

AiBridgeMidiCleanupPlan planCleanup(const AiBridgeMidiClipSnapshot &snapshot,
	const QVector<int> &canonicalIndices,
	const AiBridgeMidiCleanupOptions &options);

struct AiBridgeMidiScaleConstraintOptions
{
	QVector<int> allowedPitchClasses;
	AiBridgeMidiScaleDirection direction = AiBridgeMidiScaleDirection::Nearest;
	AiBridgeMidiNearestTieBreak nearestTieBreak = AiBridgeMidiNearestTieBreak::Down;
};

AiBridgeMidiHumanizePlan planScaleConstraint(const AiBridgeMidiClipSnapshot &snapshot,
	const QVector<int> &canonicalIndices, const AiBridgeMidiScaleConstraintOptions &options);

} // namespace lmms

#endif
