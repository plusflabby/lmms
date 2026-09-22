#ifndef LMMS_AIBRIDGE_MIDI_ACTIONS_H
#define LMMS_AIBRIDGE_MIDI_ACTIONS_H

#include "AiBridgeMidiTypes.h"

#ifdef LMMS_TESTING
#include <functional>
#endif

namespace lmms
{

struct AiBridgeMidiNativePlan
{
	QString clipId;
	quint64 projectGeneration = 0;
	QString expectedMidiStateHash;
	QVector<AiBridgeMidiNoteSnapshot> notes;
};

struct AiBridgeMidiActionResult
{
	bool success = false;
	QString code;
	bool journalCheckpointCreated = false;
	AiBridgeMidiClipSnapshot postState;
	QString midiStateHash;
};

struct AiBridgeMidiExecutionResult
{
	bool success = false;
	QString code;
	bool preflightSucceeded = false;
	int failedPlanIndex = -1;
	AiBridgeMidiNativePlan failedPlan;
	QVector<AiBridgeMidiNativePlan> appliedPlans;
	QVector<AiBridgeMidiNativePlan> unappliedPlans;
	QVector<AiBridgeMidiActionResult> observedResults;
};

#ifdef LMMS_TESTING
struct AiBridgeMidiExecutionTestHooks
{
	std::function<bool(const AiBridgeMidiNativePlan &)> failBeforeApply;
	std::function<void(const AiBridgeMidiNativePlan &, AiBridgeMidiActionResult &)> alterObservedResult;
};
#endif

class AiBridgeMidiActions
{
public:
	static AiBridgeMidiActionResult apply(const AiBridgeMidiNativePlan &plan);
	static AiBridgeMidiExecutionResult execute(const QVector<AiBridgeMidiNativePlan> &plans);
#ifdef LMMS_TESTING
	static void setTestHooks(AiBridgeMidiExecutionTestHooks hooks);
#endif
};

} // namespace lmms

#endif
