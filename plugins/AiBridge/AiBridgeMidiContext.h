#ifndef LMMS_AIBRIDGE_MIDI_CONTEXT_H
#define LMMS_AIBRIDGE_MIDI_CONTEXT_H

#include <QStringList>

#include "AiBridgeMidiTypes.h"

namespace lmms
{

struct AiBridgeMidiCapabilities
{
	int keyMinimum = 0;
	int keyMaximum = 127;
	int volumeMinimum = 0;
	int volumeMaximum = 200;
	int panningMinimum = -100;
	int panningMaximum = 100;
	qint64 minimumLengthTicks = 1;
	bool notePanningSupported = true;
	int maxClipsPerMutation = AiBridgeMidiLimits::maxClipsPerMutation;
	int maxSelectedNotesPerMutation = AiBridgeMidiLimits::maxSelectedNotesPerMutation;
	int pageLimit = AiBridgeMidiLimits::maxNotesPerPage;
	int maxPreviewEntries = AiBridgeMidiLimits::maxPreviewEntries;
	int maxAnalysisNotes = AiBridgeMidiLimits::maxAnalysisNotes;
	QStringList humanizeDistributions;
};

struct AiBridgeMidiNotePage
{
	QString midiStateHash;
	QVector<AiBridgeMidiNoteSnapshot> notes;
	QVector<QString> noteRefs;
	int total = 0;
	int offset = 0;
	int limit = 0;
	bool hasMore = false;
};

struct AiBridgeMidiSelectionResult
{
	bool success = false;
	QString code;
	QVector<int> canonicalIndices;
};

class AiBridgeMidiContext
{
public:
	static AiBridgeMidiCapabilities getCapabilities();
	static AiBridgeMidiNotePage pageNotes(const AiBridgeMidiClipSnapshot &snapshot, int offset, int limit);
	static AiBridgeMidiSelectionResult resolveSelection(const AiBridgeMidiClipSnapshot &snapshot,
		const QString &expectedMidiStateHash, const AiBridgeMidiSelection &selection);
};
}

#endif
