#include "AiBridgeMidiTypes.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>

#include <algorithm>

namespace lmms
{
namespace
{
bool noteLess(const AiBridgeMidiNoteSnapshot &left, const AiBridgeMidiNoteSnapshot &right)
{
	if (left.positionTicks != right.positionTicks) return left.positionTicks < right.positionTicks;
	if (left.key != right.key) return left.key < right.key;
	if (left.lengthTicks != right.lengthTicks) return left.lengthTicks < right.lengthTicks;
	if (left.volume != right.volume) return left.volume < right.volume;
	return left.panning < right.panning;
}

bool sameVisibleNote(const AiBridgeMidiNoteSnapshot &left, const AiBridgeMidiNoteSnapshot &right)
{
	return left.positionTicks == right.positionTicks && left.lengthTicks == right.lengthTicks
		&& left.key == right.key && left.volume == right.volume && left.panning == right.panning;
}

QByteArray canonicalStateBytes(const AiBridgeMidiClipSnapshot &snapshot)
{
	QByteArray bytes;
	QDataStream stream(&bytes, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_15);
	stream << QByteArrayLiteral("lmms-phase3h-midi-state-v1");
	const auto notes = canonicalizeMidiNotes(snapshot.notes);
	stream << static_cast<quint32>(notes.size());
	for (const auto &note : notes)
	{
		stream << note.positionTicks << note.lengthTicks << static_cast<qint32>(note.key)
			<< static_cast<qint32>(note.volume) << static_cast<qint32>(note.panning)
			<< static_cast<qint32>(note.tieOrdinal);
	}
	return bytes;
}

QString opaqueToken(const AiBridgeMidiClipSnapshot &snapshot,
	const QString &stateHash, const AiBridgeMidiNoteSnapshot &note)
{
	QByteArray bytes;
	QDataStream stream(&bytes, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_15);
	stream << QByteArrayLiteral("lmms-phase3h-note-ref-v1") << snapshot.clipId
		<< snapshot.projectGeneration << stateHash << note.positionTicks << note.lengthTicks
		<< static_cast<qint32>(note.key) << static_cast<qint32>(note.volume)
		<< static_cast<qint32>(note.panning) << static_cast<qint32>(note.tieOrdinal);
	return QStringLiteral("nref_") + QString::fromLatin1(
		QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool hasPredicate(const AiBridgeMidiSelection &selection)
{
	return !selection.keys.isEmpty() || selection.startTick || selection.endTick || selection.keyMin || selection.keyMax
		|| selection.volumeMin || selection.volumeMax || selection.lengthMin || selection.lengthMax;
}
}

QVector<AiBridgeMidiNoteSnapshot> canonicalizeMidiNotes(
	const QVector<AiBridgeMidiNoteSnapshot> &notes)
{
	auto result = notes;
	std::sort(result.begin(), result.end(), noteLess);
	for (int index = 0; index < result.size(); ++index)
	{
		result[index].tieOrdinal = index > 0 && sameVisibleNote(result[index - 1], result[index])
			? result[index - 1].tieOrdinal + 1 : 0;
	}
	return result;
}

QString computeMidiStateHash(const AiBridgeMidiClipSnapshot &snapshot)
{
	return QString::fromLatin1(QCryptographicHash::hash(canonicalStateBytes(snapshot),
		QCryptographicHash::Sha256).toHex());
}

QVector<QString> makeMidiNoteRefs(const AiBridgeMidiClipSnapshot &snapshot)
{
	const auto canonical = canonicalizeMidiNotes(snapshot.notes);
	const auto hash = computeMidiStateHash(snapshot);
	QVector<QString> refs;
	refs.reserve(canonical.size());
	for (const auto &note : canonical) refs.append(opaqueToken(snapshot, hash, note));
	return refs;
}

AiBridgeMidiNoteRefResolution resolveMidiNoteRef(
	const AiBridgeMidiClipSnapshot &snapshot, const QString &noteRef)
{
	const auto refs = makeMidiNoteRefs(snapshot);
	for (int index = 0; index < refs.size(); ++index)
	{
		if (refs[index] == noteRef) return {true, index, QString()};
	}
	return {false, -1, QStringLiteral("stale_note_ref")};
}

AiBridgeValidationResult AiBridgeMidiSelection::validate() const
{
	const bool predicate = hasPredicate(*this);
	if ((mode == AiBridgeMidiSelectionMode::NoteRefs && (noteRefs.isEmpty() || predicate))
		|| (mode == AiBridgeMidiSelectionMode::Predicate && (!noteRefs.isEmpty() || !predicate)))
	{
		return {QStringLiteral("invalid_selection"), QStringLiteral("selection mode and fields conflict")};
	}
	if (noteRefs.size() > AiBridgeMidiLimits::maxSelectedNotesPerMutation)
		return {QStringLiteral("note_limit_exceeded"), QStringLiteral("too many note references")};
	for (const int key : keys)
	{
		if (key < 0 || key > 127)
			return {QStringLiteral("invalid_selection"), QStringLiteral("key is outside the MIDI range")};
	}
	if (startTick.has_value() != endTick.has_value() || (startTick && (*startTick < 0 || *endTick <= *startTick)))
		return {QStringLiteral("invalid_selection"), QStringLiteral("tick range must be half-open")};
	if (keyMin.has_value() != keyMax.has_value() || (keyMin && (*keyMin < 0 || *keyMax > 127 || *keyMin > *keyMax)))
		return {QStringLiteral("invalid_selection"), QStringLiteral("key range is invalid")};
	if (volumeMin.has_value() != volumeMax.has_value() || (volumeMin && (*volumeMin < 0 || *volumeMax > 200 || *volumeMin > *volumeMax)))
		return {QStringLiteral("invalid_selection"), QStringLiteral("volume range is invalid")};
	if (lengthMin.has_value() != lengthMax.has_value() || (lengthMin && (*lengthMin < 1 || *lengthMax < *lengthMin)))
		return {QStringLiteral("invalid_selection"), QStringLiteral("length range is invalid")};
	return {};
}

} // namespace lmms
