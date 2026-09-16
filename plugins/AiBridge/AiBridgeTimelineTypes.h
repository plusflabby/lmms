#ifndef LMMS_AIBRIDGE_TIMELINE_TYPES_H
#define LMMS_AIBRIDGE_TIMELINE_TYPES_H

#include <QVector>
#include <QString>

#include <optional>

#include "AiBridgeValidationTypes.h"

namespace lmms
{

namespace AiBridgeTimelineLimits
{
constexpr int maxTimelineClipsPerPage = 512;
constexpr int maxTimelineClipChanges = 256;
constexpr int maxRegionAffectedObjects = 2048;

bool validPage(int offset, int limit);
}

struct AiBridgeMusicalPosition
{
	std::optional<qint64> tick;
	std::optional<int> bar;
	std::optional<int> beat;
	std::optional<int> tickInBeat;

	AiBridgeValidationResult validate() const;
};

struct AiBridgeTimelineRange
{
	qint64 startTick = 0;
	qint64 endTick = 0;

	AiBridgeValidationResult validate() const;
	bool contains(qint64 value) const;
};

struct AiBridgeTrackScope
{
	QString mode;
	QVector<QString> trackIds;

	AiBridgeValidationResult validate() const;
};

struct AiBridgePageMetadata
{
	int total = 0;
	int offset = 0;
	int limit = 0;
	bool hasMore = false;
};

struct AiBridgeTimelineClipSnapshot
{
	QString clipId;
	QString trackId;
	QString type;
	QString name;
	qint64 startTick = 0;
	qint64 endTick = 0;
};

struct AiBridgeArrangementConflict
{
	QString trackId;
	QString clipId;
	QString automationClipId;
	AiBridgeTimelineRange range;
	bool overwriteWouldResolve = false;
};

enum class AiBridgeArrangementStepKind
{
	DeleteClip,
	DuplicateClip,
	MoveClip,
	ShiftClip,
};

struct AiBridgeArrangementStep
{
	AiBridgeArrangementStepKind kind = AiBridgeArrangementStepKind::DeleteClip;
	QString clipId;
	QString trackId;
	qint64 sourceTick = 0;
	qint64 destinationTick = 0;
};

struct AiBridgeArrangementAutomationSnapshot
{
	QString automationTrackId;
	QString automationClipId;
	QString automationTargetId;
	qint64 startTick = 0;
	qint64 endTick = 0;
	struct Point { QString pointId; qint64 tick = 0; float value = 0.0f; QString curve = QStringLiteral("discrete"); };
	QVector<Point> points;
	QString ownerId;
};

enum class AiBridgeArrangementAutomationStepKind
{
	DeleteClip,
	DuplicateClip,
	MoveClip,
	ShiftClip,
};

struct AiBridgeArrangementAutomationStep
{
	AiBridgeArrangementAutomationStepKind kind = AiBridgeArrangementAutomationStepKind::DeleteClip;
	QString automationTrackId;
	QString automationClipId;
	QString automationTargetId;
	qint64 sourceStartTick = 0;
	qint64 sourceEndTick = 0;
	qint64 destinationStartTick = 0;
	qint64 destinationEndTick = 0;
	QVector<AiBridgeArrangementAutomationSnapshot::Point> points;
};

struct AiBridgeArrangementPlan
{
	QVector<AiBridgeArrangementStep> steps;
	QVector<AiBridgeArrangementAutomationStep> automationSteps;
	QVector<QString> orderedOperations;
	QVector<AiBridgeTimelineClipSnapshot> affectedClips;
	QVector<AiBridgeTimelineClipSnapshot> destinationClips;
	QVector<QString> deleteClipIds;
	QVector<QString> overwriteDeleteIds;
	QVector<QString> shiftedClipIds;
	qint64 shiftTick = 0;
	QVector<AiBridgeArrangementConflict> conflicts;
	AiBridgeTimelineRange resultingBounds;
	bool includeAutomation = true;
	bool overwrite = false;
	bool dryRun = false;
	bool noEffect = false;
	QString noEffectReason;
};

struct AiBridgeArrangementSnapshot
{
	QVector<AiBridgeTimelineClipSnapshot> clips;
	QVector<AiBridgeArrangementAutomationSnapshot> automationClips;
	quint64 projectGeneration = 0;
};

struct AiBridgeArrangementRequest
{
	QString operation;
	AiBridgeTimelineRange source;
	qint64 destinationTick = 0;
	qint64 deltaTick = 0;
	AiBridgeTrackScope scope;
	bool includeAutomation = true;
	bool overwrite = false;
	bool ripple = false;
	bool dryRun = false;
};

struct AiBridgeTimelineClipPage
{
	AiBridgePageMetadata page;
	QVector<AiBridgeTimelineClipSnapshot> items;
};

} // namespace lmms

#endif
