#include "AiBridgeMidiActions.h"

#include <QApplication>
#include <QThread>

#include <algorithm>
#include <limits>
#include <utility>

#include "AiBridgeProjectContext.h"
#include "Engine.h"
#include "MidiClip.h"
#include "Note.h"
#include "Song.h"
#include "TimePos.h"

namespace lmms
{
namespace
{
bool onApplicationThread()
{
	return qApp && QThread::currentThread() == qApp->thread();
}

AiBridgeMidiClipSnapshot snapshotMidiClip(const MidiClip &clip, const QString &clipId,
	quint64 projectGeneration)
{
	AiBridgeMidiClipSnapshot snapshot;
	snapshot.clipId = clipId;
	snapshot.projectGeneration = projectGeneration;
	for (const auto *note : clip.notes())
		snapshot.notes.append({note->pos().getTicks(), note->length().getTicks(), note->key(),
			note->getVolume(), note->getPanning()});
	snapshot.notes = canonicalizeMidiNotes(snapshot.notes);
	return snapshot;
}

bool validPlannedNote(const AiBridgeMidiNoteSnapshot &note)
{
	return note.positionTicks >= 0 && note.positionTicks <= std::numeric_limits<qint32>::max()
		&& note.lengthTicks >= 1 && note.lengthTicks <= std::numeric_limits<qint32>::max()
		&& note.key >= 0 && note.key <= 127 && note.volume >= 0 && note.volume <= 200
		&& note.panning >= -100 && note.panning <= 100;
}

bool sameNotes(const QVector<AiBridgeMidiNoteSnapshot> &left,
	const QVector<AiBridgeMidiNoteSnapshot> &right)
{
	if (left.size() != right.size()) return false;
	for (qsizetype index = 0; index < left.size(); ++index)
	{
		const auto &a = left[index];
		const auto &b = right[index];
		if (a.positionTicks != b.positionTicks || a.lengthTicks != b.lengthTicks
			|| a.key != b.key || a.volume != b.volume || a.panning != b.panning)
		{
			return false;
		}
	}
	return true;
}
#ifdef LMMS_TESTING
AiBridgeMidiExecutionTestHooks &testHooks()
{
	static AiBridgeMidiExecutionTestHooks hooks;
	return hooks;
}
#endif
}

AiBridgeMidiActionResult AiBridgeMidiActions::apply(const AiBridgeMidiNativePlan &plan)
{
	AiBridgeMidiActionResult result;
	if (!onApplicationThread()) { result.code = QStringLiteral("wrong_thread"); return result; }
	AiBridgeProjectContext context;
	if (plan.projectGeneration != context.projectGeneration()) { result.code = QStringLiteral("plan_stale"); return result; }
	auto *clip = dynamic_cast<MidiClip *>(context.resolveClip(plan.clipId));
	if (!clip) { result.code = QStringLiteral("plan_stale"); return result; }
	const auto before = snapshotMidiClip(*clip, plan.clipId, context.projectGeneration());
	if (computeMidiStateHash(before) != plan.expectedMidiStateHash)
	{
		result.code = QStringLiteral("plan_stale");
		return result;
	}
	for (const auto &note : plan.notes)
		if (!validPlannedNote(note)) { result.code = QStringLiteral("invalid_plan"); return result; }
	const auto planned = canonicalizeMidiNotes(plan.notes);
	if (sameNotes(before.notes, planned))
	{
		result.success = true;
		result.code = QStringLiteral("no_effect");
		result.postState = before;
		result.midiStateHash = computeMidiStateHash(before);
		return result;
	}
	clip->addJournalCheckPoint();
	result.journalCheckpointCreated = true;
	while (!clip->notes().empty()) clip->removeNote(clip->notes().front());
	for (const auto &note : planned)
		clip->addNote(Note(TimePos(static_cast<tick_t>(note.lengthTicks)),
			TimePos(static_cast<tick_t>(note.positionTicks)), note.key, note.volume, note.panning), false);
	clip->rearrangeAllNotes();
	Engine::getSong()->setModified();
	result.postState = snapshotMidiClip(*clip, plan.clipId, context.projectGeneration());
	result.midiStateHash = computeMidiStateHash(result.postState);
	result.success = true;
	result.code = QStringLiteral("ok");
	return result;
}

AiBridgeMidiExecutionResult AiBridgeMidiActions::execute(const QVector<AiBridgeMidiNativePlan> &plans)
{
	AiBridgeMidiExecutionResult result;
	if (!onApplicationThread()) { result.code = QStringLiteral("wrong_thread"); return result; }
	if (plans.isEmpty()) { result.success = true; result.code = QStringLiteral("no_effect"); result.preflightSucceeded = true; return result; }
	AiBridgeProjectContext context;
	QVector<AiBridgeMidiNativePlan> ordered = plans;
	std::sort(ordered.begin(), ordered.end(), [](const auto &left, const auto &right)
		{ return left.clipId < right.clipId; });
	bool hasEffect = false;
	for (const auto &plan : ordered)
	{
		if (plan.projectGeneration != context.projectGeneration())
		{
			result.code = QStringLiteral("midi_state_mismatch");
			result.unappliedPlans = ordered;
			return result;
		}
		auto *clip = dynamic_cast<MidiClip *>(context.resolveClip(plan.clipId));
		if (!clip || computeMidiStateHash(snapshotMidiClip(*clip, plan.clipId, context.projectGeneration())) != plan.expectedMidiStateHash)
		{
			result.code = QStringLiteral("midi_state_mismatch");
			result.unappliedPlans = ordered;
			return result;
		}
		for (const auto &note : plan.notes)
			if (!validPlannedNote(note)) { result.code = QStringLiteral("invalid_plan"); result.unappliedPlans = ordered; return result; }
		if (!sameNotes(snapshotMidiClip(*clip, plan.clipId, context.projectGeneration()).notes,
			canonicalizeMidiNotes(plan.notes))) hasEffect = true;
	}
	result.preflightSucceeded = true;
	if (!hasEffect) { result.success = true; result.code = QStringLiteral("no_effect"); return result; }
	for (int index = 0; index < ordered.size(); ++index)
	{
		const auto &plan = ordered.at(index);
		#ifdef LMMS_TESTING
		if (testHooks().failBeforeApply && testHooks().failBeforeApply(plan))
		{
			AiBridgeMidiActionResult failed;
			failed.code = QStringLiteral("injected_native_failure");
			result.observedResults.append(failed);
			result.code = result.appliedPlans.isEmpty() ? failed.code : QStringLiteral("partial_failure");
			result.failedPlanIndex = index;
			result.failedPlan = plan;
			result.unappliedPlans = ordered.mid(index);
			return result;
		}
		#endif
		const auto applied = apply(plan);
		AiBridgeMidiActionResult observed = applied;
		#ifdef LMMS_TESTING
		if (testHooks().alterObservedResult) testHooks().alterObservedResult(plan, observed);
		#endif
		result.observedResults.append(observed);
		if (!applied.success)
		{
			result.code = result.appliedPlans.isEmpty() ? applied.code : QStringLiteral("partial_failure");
			result.failedPlanIndex = index;
			result.failedPlan = plan;
			result.unappliedPlans = ordered.mid(index);
			return result;
		}
		if (!sameNotes(observed.postState.notes, canonicalizeMidiNotes(plan.notes)))
		{
			result.code = QStringLiteral("post_verification_failed");
			result.failedPlanIndex = index;
			result.failedPlan = plan;
			result.appliedPlans.append(plan);
			result.unappliedPlans = ordered.mid(index + 1);
			return result;
		}
		result.appliedPlans.append(plan);
	}
	result.success = true;
	result.code = QStringLiteral("ok");
	return result;
}

#ifdef LMMS_TESTING
void AiBridgeMidiActions::setTestHooks(AiBridgeMidiExecutionTestHooks hooks)
{
	testHooks() = std::move(hooks);
}
#endif

} // namespace lmms
