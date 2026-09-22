#ifndef LMMS_AIBRIDGE_MIDI_TYPES_H
#define LMMS_AIBRIDGE_MIDI_TYPES_H

#include <QVector>
#include <QString>

#include <optional>

#include "AiBridgeValidationTypes.h"

namespace lmms
{

namespace AiBridgeMidiLimits
{
constexpr int maxClipsPerMutation = 32;
constexpr int maxSelectedNotesPerMutation = 2048;
constexpr int maxNotesPerPage = 512;
constexpr int maxPreviewEntries = 128;
constexpr int maxAnalysisNotes = 4096;
}

struct AiBridgeMidiNoteSnapshot
{
	qint64 positionTicks = 0;
	qint64 lengthTicks = 1;
	int key = 0;
	int volume = 0;
	int panning = 0;
	int tieOrdinal = 0;
};

struct AiBridgeMidiClipSnapshot
{
	QString clipId;
	quint64 projectGeneration = 0;
	QVector<AiBridgeMidiNoteSnapshot> notes;
};

struct AiBridgeMidiClipExpectation
{
	QString clipId;
	QString expectedMidiStateHash;
};

struct AiBridgeMidiClipPlan
{
	QString clipId;
	QString beforeMidiStateHash;
	QString afterMidiStateHash;
	int selectedCount = 0;
	int changedCount = 0;
};

enum class AiBridgeMidiSelectionMode
{
	NoteRefs,
	Predicate,
};

struct AiBridgeMidiSelection
{
	AiBridgeMidiSelectionMode mode = AiBridgeMidiSelectionMode::NoteRefs;
	QVector<QString> noteRefs;
	QVector<int> keys;
	std::optional<qint64> startTick;
	std::optional<qint64> endTick;
	std::optional<int> keyMin;
	std::optional<int> keyMax;
	std::optional<int> volumeMin;
	std::optional<int> volumeMax;
	std::optional<qint64> lengthMin;
	std::optional<qint64> lengthMax;

	AiBridgeValidationResult validate() const;
};

struct AiBridgeMidiNoteRefResolution
{
	bool ok = false;
	int canonicalIndex = -1;
	QString code;
};

QVector<AiBridgeMidiNoteSnapshot> canonicalizeMidiNotes(
	const QVector<AiBridgeMidiNoteSnapshot> &notes);
QString computeMidiStateHash(const AiBridgeMidiClipSnapshot &snapshot);
QVector<QString> makeMidiNoteRefs(const AiBridgeMidiClipSnapshot &snapshot);
AiBridgeMidiNoteRefResolution resolveMidiNoteRef(
	const AiBridgeMidiClipSnapshot &snapshot, const QString &noteRef);

} // namespace lmms

#endif
