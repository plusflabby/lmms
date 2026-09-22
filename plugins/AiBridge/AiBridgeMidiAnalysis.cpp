#include "AiBridgeMidiAnalysis.h"

#include <QSet>

namespace lmms
{
namespace
{
bool sameVisibleNote(const AiBridgeMidiNoteSnapshot &left, const AiBridgeMidiNoteSnapshot &right)
{
	return left.positionTicks == right.positionTicks && left.lengthTicks == right.lengthTicks
		&& left.key == right.key && left.volume == right.volume && left.panning == right.panning;
}

qint64 nearestGridDeviation(qint64 positionTicks, qint64 gridTicks)
{
	const qint64 remainder = positionTicks % gridTicks;
	return qMin(remainder, gridTicks - remainder);
}
}

AiBridgeMidiAnalysisResult analyzeMidi(const QVector<AiBridgeMidiClipSnapshot> &clips,
	const AiBridgeMidiAnalysisOptions &options)
{
	AiBridgeMidiAnalysisResult result;
	result.pitchClassHistogram.fill(0, 12);
	if ((options.shortNoteThresholdTicks && *options.shortNoteThresholdTicks < 1)
		|| (options.gridTicks && *options.gridTicks < 1))
	{
		result.code = QStringLiteral("invalid_request");
		return result;
	}

	qint64 totalInput = 0;
	for (const auto &clip : clips) totalInput += clip.notes.size();
	if (totalInput > AiBridgeMidiLimits::maxAnalysisNotes)
	{
		result.code = QStringLiteral("note_limit_exceeded");
		return result;
	}

	QSet<qint64> onsets;
	qint64 volumeTotal = 0;
	qint64 lengthTotal = 0;
	qint64 deviationTotal = 0;
	if (options.gridTicks) result.gridDeviation.emplace();

	for (const auto &clip : clips)
	{
		const auto notes = canonicalizeMidiNotes(clip.notes);
		for (int index = 0; index < notes.size(); ++index)
		{
			const auto &note = notes[index];
			if (result.noteCount == 0)
			{
				result.pitchMinimum = result.pitchMaximum = note.key;
				result.volumeMinimum = result.volumeMaximum = note.volume;
				result.lengthMinimumTicks = result.lengthMaximumTicks = note.lengthTicks;
			}
			else
			{
				result.pitchMinimum = qMin(result.pitchMinimum, note.key);
				result.pitchMaximum = qMax(result.pitchMaximum, note.key);
				result.volumeMinimum = qMin(result.volumeMinimum, note.volume);
				result.volumeMaximum = qMax(result.volumeMaximum, note.volume);
				result.lengthMinimumTicks = qMin(result.lengthMinimumTicks, note.lengthTicks);
				result.lengthMaximumTicks = qMax(result.lengthMaximumTicks, note.lengthTicks);
			}
			++result.noteCount;
			onsets.insert(note.positionTicks);
			++result.pitchClassHistogram[note.key % 12];
			volumeTotal += note.volume;
			lengthTotal += note.lengthTicks;
			if (options.shortNoteThresholdTicks && note.lengthTicks < *options.shortNoteThresholdTicks)
				++result.shortNoteCount;
			if (result.gridDeviation)
			{
				const auto deviation = nearestGridDeviation(note.positionTicks, *options.gridTicks);
				if (deviation == 0) ++result.gridDeviation->exactOnGridCount;
				deviationTotal += deviation;
				result.gridDeviation->maximumAbsoluteDeviationTicks = qMax(
					result.gridDeviation->maximumAbsoluteDeviationTicks, deviation);
			}
			if (index > 0 && sameVisibleNote(notes[index - 1], note)) ++result.exactDuplicateCount;
			for (int previous = 0; previous < index; ++previous)
			{
				const auto &earlier = notes[previous];
				if (earlier.key == note.key && !sameVisibleNote(earlier, note)
					&& earlier.positionTicks + earlier.lengthTicks > note.positionTicks)
					++result.samePitchOverlapCount;
			}
		}
	}

	result.distinctOnsetCount = onsets.size();
	if (result.noteCount > 0)
	{
		result.volumeMean = static_cast<double>(volumeTotal) / result.noteCount;
		result.lengthMeanTicks = static_cast<double>(lengthTotal) / result.noteCount;
		if (result.gridDeviation)
			result.gridDeviation->meanAbsoluteDeviationTicks = static_cast<double>(deviationTotal) / result.noteCount;
	}
	result.success = true;
	return result;
}

} // namespace lmms
