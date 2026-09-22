#ifndef LMMS_AIBRIDGE_MIDI_ANALYSIS_H
#define LMMS_AIBRIDGE_MIDI_ANALYSIS_H

#include <optional>

#include "AiBridgeMidiTypes.h"

namespace lmms
{

struct AiBridgeMidiGridDeviation
{
	int exactOnGridCount = 0;
	double meanAbsoluteDeviationTicks = 0.0;
	qint64 maximumAbsoluteDeviationTicks = 0;
};

struct AiBridgeMidiAnalysisOptions
{
	std::optional<qint64> shortNoteThresholdTicks;
	std::optional<qint64> gridTicks;
};

struct AiBridgeMidiAnalysisResult
{
	bool success = false;
	QString code;
	int noteCount = 0;
	int distinctOnsetCount = 0;
	int pitchMinimum = 0;
	int pitchMaximum = 0;
	QVector<int> pitchClassHistogram;
	int volumeMinimum = 0;
	int volumeMaximum = 0;
	double volumeMean = 0.0;
	qint64 lengthMinimumTicks = 0;
	qint64 lengthMaximumTicks = 0;
	double lengthMeanTicks = 0.0;
	int exactDuplicateCount = 0;
	int samePitchOverlapCount = 0;
	int shortNoteCount = 0;
	std::optional<AiBridgeMidiGridDeviation> gridDeviation;
};

AiBridgeMidiAnalysisResult analyzeMidi(const QVector<AiBridgeMidiClipSnapshot> &clips,
	const AiBridgeMidiAnalysisOptions &options);

} // namespace lmms

#endif
