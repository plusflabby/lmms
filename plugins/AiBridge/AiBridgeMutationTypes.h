#ifndef LMMS_AIBRIDGE_MUTATION_TYPES_H
#define LMMS_AIBRIDGE_MUTATION_TYPES_H

#include <QVector>
#include <QString>
#include <QByteArray>
#include <optional>

namespace lmms
{

struct AiBridgeCopiedNote
{
	qint64 position_ticks = 0;
	qint64 length_ticks = 0;
	int key = 0;
	int volume = 100;
	int panning = 0;
	QString note_type = QStringLiteral("regular");
};

struct AiBridgeMutationState
{
	qint64 clip_length_ticks = 0;
	bool auto_resize = false;
	QVector<AiBridgeCopiedNote> notes;
};

struct AiBridgeExactNote
{
	std::optional<qint64> position;
	std::optional<qint64> length;
	std::optional<int> key;
	std::optional<int> volume;
	std::optional<int> panning;
	std::optional<QString> note_type;
};

struct AiBridgeNoteSelection
{
	std::optional<qint64> position_start;
	std::optional<qint64> position_end;
	std::optional<int> key_min;
	std::optional<int> key_max;
	std::optional<AiBridgeExactNote> exact;
};

struct AiBridgeMutationCounts
{
	int matched = 0;
	int changed = 0;
	int created = 0;
	int deleted = 0;
};

struct AiBridgeMutationResult
{
	QString clip_id;
	QString track_id;
	int notes_added = 0;
	int notes_deleted = 0;
	int notes_changed = 0;
	QString old_content_hash;
	QString new_content_hash;
	qint64 clip_length_ticks = 0;
	bool current_in_piano_roll = false;
};

struct AiBridgeValidationResult
{
	QString code;
	QString message;
	bool success() const { return code.isEmpty(); }
};

namespace AiBridgeMutationTypes
{

AiBridgeValidationResult validateNoteInput(const AiBridgeCopiedNote &note);
AiBridgeValidationResult validateSelection(const AiBridgeNoteSelection &selection, bool requires_bounded_range);
bool matchesSelection(const AiBridgeCopiedNote &note, const AiBridgeNoteSelection &selection, qint64 source_index);
QByteArray canonicalMutationState(const AiBridgeMutationState &state);
QString contentHash(const AiBridgeMutationState &state);

} // namespace AiBridgeMutationTypes
} // namespace lmms

#endif
