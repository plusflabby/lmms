#include <QtTest>

#include "AiBridgeMidiActions.h"
#include "AiBridgeProjectContext.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Note.h"
#include "ProjectJournal.h"
#include "Song.h"

class AiBridgeMidiActionsTest : public QObject
{
	Q_OBJECT

	static lmms::AiBridgeMidiClipSnapshot snapshot(lmms::MidiClip &clip, const QString &clipId, quint64 generation)
	{
		lmms::AiBridgeMidiClipSnapshot result;
		result.clipId = clipId;
		result.projectGeneration = generation;
		for (const auto *note : clip.notes())
			result.notes.append({note->pos().getTicks(), note->length().getTicks(), note->key(), note->getVolume(), note->getPanning()});
		result.notes = lmms::canonicalizeMidiNotes(result.notes);
		return result;
	}

private slots:
	void initTestCase()
	{
		lmms::Engine::init(true);
	}

	void cleanupTestCase()
	{
		lmms::Engine::destroy();
	}

	void appliesExactCopiedStateAndReturnsNativePostState()
	{
		using namespace lmms;
		Song *song = Engine::getSong();
		InstrumentTrack track(song);
		MidiClip clip(&track);
		clip.addNote(Note(TimePos(12), TimePos(0), 60, 80, 0), false);
		clip.addNote(Note(TimePos(12), TimePos(24), 62, 90, 0), false);
		AiBridgeProjectContext context;
		const auto clipId = context.idForClip(&clip);
		const auto before = snapshot(clip, clipId, context.projectGeneration());
		AiBridgeMidiNativePlan plan;
		plan.clipId = clipId;
		plan.projectGeneration = before.projectGeneration;
		plan.expectedMidiStateHash = computeMidiStateHash(before);
		plan.notes = {{12, 24, 64, 110, 0}};
		Engine::projectJournal()->clearJournal();
		const auto result = AiBridgeMidiActions::apply(plan);
		QVERIFY(result.success);
		QVERIFY(result.journalCheckpointCreated);
		QVERIFY(Engine::projectJournal()->canUndo());
		QCOMPARE(result.postState.notes.size(), 1);
		QCOMPARE(result.postState.notes[0].positionTicks, qint64(12));
		QCOMPARE(result.postState.notes[0].lengthTicks, qint64(24));
		QCOMPARE(result.postState.notes[0].key, 64);
		QCOMPARE(result.postState.notes[0].volume, 110);
		QCOMPARE(computeMidiStateHash(result.postState), result.midiStateHash);
		const auto nativePostState = snapshot(clip, clipId, before.projectGeneration);
		QCOMPARE(nativePostState.notes.size(), 1);
		QCOMPARE(nativePostState.notes[0].positionTicks, qint64(12));
		QCOMPARE(nativePostState.notes[0].lengthTicks, qint64(24));
		QCOMPARE(nativePostState.notes[0].key, 64);
		QCOMPARE(nativePostState.notes[0].volume, 110);
	}

	void reconstructsCanonicalDuplicateMultisetWithoutDuplicateApplication()
	{
		using namespace lmms;
		Song *song = Engine::getSong();
		InstrumentTrack track(song);
		MidiClip clip(&track);
		clip.addNote(Note(TimePos(24), TimePos(12), 55, 70, 0), false);
		AiBridgeProjectContext context;
		const auto clipId = context.idForClip(&clip);
		const auto before = snapshot(clip, clipId, context.projectGeneration());
		AiBridgeMidiNativePlan plan;
		plan.clipId = clipId;
		plan.projectGeneration = before.projectGeneration;
		plan.expectedMidiStateHash = computeMidiStateHash(before);
		plan.notes = {{48, 8, 69, 100, -5}, {0, 16, 60, 80, 0}, {0, 16, 60, 80, 0}};

		const auto result = AiBridgeMidiActions::apply(plan);
		QVERIFY(result.success);
		QCOMPARE(result.code, QStringLiteral("ok"));
		QCOMPARE(clip.notes().size(), 3);
		QCOMPARE(clip.notes().at(0)->pos().getTicks(), tick_t(0));
		QCOMPARE(clip.notes().at(1)->pos().getTicks(), tick_t(0));
		QCOMPARE(clip.notes().at(2)->pos().getTicks(), tick_t(48));

		const auto nativePostState = snapshot(clip, clipId, before.projectGeneration);
		const auto expected = canonicalizeMidiNotes(plan.notes);
		QCOMPARE(nativePostState.notes.size(), expected.size());
		QCOMPARE(result.postState.notes.size(), expected.size());
		for (qsizetype index = 0; index < expected.size(); ++index)
		{
			QCOMPARE(nativePostState.notes[index].positionTicks, expected[index].positionTicks);
			QCOMPARE(nativePostState.notes[index].lengthTicks, expected[index].lengthTicks);
			QCOMPARE(nativePostState.notes[index].key, expected[index].key);
			QCOMPARE(nativePostState.notes[index].volume, expected[index].volume);
			QCOMPARE(nativePostState.notes[index].panning, expected[index].panning);
			QCOMPARE(result.postState.notes[index].positionTicks, expected[index].positionTicks);
			QCOMPARE(result.postState.notes[index].lengthTicks, expected[index].lengthTicks);
			QCOMPARE(result.postState.notes[index].key, expected[index].key);
			QCOMPARE(result.postState.notes[index].volume, expected[index].volume);
			QCOMPARE(result.postState.notes[index].panning, expected[index].panning);
		}
	}

	void rejectsStalePlanBeforeAnyNativeWrite()
	{
		using namespace lmms;
		Song *song = Engine::getSong();
		InstrumentTrack track(song);
		MidiClip clip(&track);
		clip.addNote(Note(TimePos(12), TimePos(0), 60, 80, 0), false);
		AiBridgeProjectContext context;
		const auto clipId = context.idForClip(&clip);
		const auto before = snapshot(clip, clipId, context.projectGeneration());
		AiBridgeMidiNativePlan plan;
		plan.clipId = clipId;
		plan.projectGeneration = before.projectGeneration;
		plan.expectedMidiStateHash = QStringLiteral("stale");
		plan.notes = {{24, 24, 72, 120, 0}};
		Engine::projectJournal()->clearJournal();
		const auto result = AiBridgeMidiActions::apply(plan);
		QVERIFY(!result.success);
		QCOMPARE(result.code, QStringLiteral("plan_stale"));
		QVERIFY(!result.journalCheckpointCreated);
		QVERIFY(!Engine::projectJournal()->canUndo());
		const auto after = snapshot(clip, clipId, before.projectGeneration);
		QCOMPARE(computeMidiStateHash(after), computeMidiStateHash(before));
	}

	void preflightsEveryClipBeforeAnyNativeWrite()
	{
		using namespace lmms;
		Song *song = Engine::getSong();
		InstrumentTrack firstTrack(song);
		InstrumentTrack secondTrack(song);
		MidiClip firstClip(&firstTrack);
		MidiClip secondClip(&secondTrack);
		firstClip.addNote(Note(TimePos(12), TimePos(0), 60, 80, 0), false);
		secondClip.addNote(Note(TimePos(12), TimePos(0), 64, 80, 0), false);
		AiBridgeProjectContext context;
		const auto generation = context.projectGeneration();
		const auto firstId = context.idForClip(&firstClip);
		const auto secondId = context.idForClip(&secondClip);
		const auto firstBefore = snapshot(firstClip, firstId, generation);
		const auto secondBefore = snapshot(secondClip, secondId, generation);
		AiBridgeMidiNativePlan first{firstId, generation, computeMidiStateHash(firstBefore), {{24, 12, 62, 100, 0}}};
		AiBridgeMidiNativePlan second{secondId, generation, QStringLiteral("stale"), {{24, 12, 65, 100, 0}}};
		Engine::projectJournal()->clearJournal();
		const auto result = AiBridgeMidiActions::execute({first, second});
		QVERIFY(!result.success);
		QCOMPARE(result.code, QStringLiteral("midi_state_mismatch"));
		QVERIFY(!Engine::projectJournal()->canUndo());
		QCOMPARE(computeMidiStateHash(snapshot(firstClip, firstId, generation)), computeMidiStateHash(firstBefore));
		QCOMPARE(computeMidiStateHash(snapshot(secondClip, secondId, generation)), computeMidiStateHash(secondBefore));
	}

	void executesClipPlansInStableClipIdOrderAndReportsNoEffect()
	{
		using namespace lmms;
		Song *song = Engine::getSong();
		InstrumentTrack firstTrack(song);
		InstrumentTrack secondTrack(song);
		MidiClip firstClip(&firstTrack);
		MidiClip secondClip(&secondTrack);
		firstClip.addNote(Note(TimePos(12), TimePos(0), 60, 80, 0), false);
		secondClip.addNote(Note(TimePos(12), TimePos(0), 64, 80, 0), false);
		AiBridgeProjectContext context;
		const auto generation = context.projectGeneration();
		const auto firstId = context.idForClip(&firstClip);
		const auto secondId = context.idForClip(&secondClip);
		const auto firstBefore = snapshot(firstClip, firstId, generation);
		const auto secondBefore = snapshot(secondClip, secondId, generation);
		AiBridgeMidiNativePlan first{firstId, generation, computeMidiStateHash(firstBefore), firstBefore.notes};
		AiBridgeMidiNativePlan second{secondId, generation, computeMidiStateHash(secondBefore), secondBefore.notes};
		Engine::projectJournal()->clearJournal();
		const auto noEffect = AiBridgeMidiActions::execute({second, first});
		QVERIFY(noEffect.success);
		QCOMPARE(noEffect.code, QStringLiteral("no_effect"));
		QVERIFY(noEffect.appliedPlans.isEmpty());
		QVERIFY(!Engine::projectJournal()->canUndo());
	}

	void reportsPostVerificationFailureWithoutRetryOrRollback()
	{
		using namespace lmms;
		Song *song = Engine::getSong(); InstrumentTrack track(song); MidiClip clip(&track);
		clip.addNote(Note(TimePos(12), TimePos(0), 60, 80, 0), false);
		AiBridgeProjectContext context; const auto id = context.idForClip(&clip); const auto generation = context.projectGeneration();
		const auto before = snapshot(clip, id, generation);
		AiBridgeMidiNativePlan plan{id, generation, computeMidiStateHash(before), {{24, 12, 62, 100, 0}}};
		int observedCount = 0;
		AiBridgeMidiActions::setTestHooks({{}, [&](const AiBridgeMidiNativePlan &, AiBridgeMidiActionResult &observed)
			{ ++observedCount; observed.postState.notes.clear(); }});
		const auto result = AiBridgeMidiActions::execute({plan});
		AiBridgeMidiActions::setTestHooks({});
		QVERIFY(!result.success); QCOMPARE(result.code, QStringLiteral("post_verification_failed"));
		QCOMPARE(observedCount, 1); QCOMPARE(result.failedPlan.clipId, id);
		QCOMPARE(result.observedResults.size(), 1); QCOMPARE(result.appliedPlans.size(), 1);
		QCOMPARE(snapshot(clip, id, generation).notes.size(), 1);
		QCOMPARE(snapshot(clip, id, generation).notes[0].key, 62);
	}

	void stopsAtInjectedLaterFailureWithExactPlanSlices()
	{
		using namespace lmms;
		Song *song = Engine::getSong(); InstrumentTrack track(song); MidiClip a(&track), b(&track), c(&track);
		a.addNote(Note(TimePos(12), TimePos(0), 60, 80, 0), false); b.addNote(Note(TimePos(12), TimePos(0), 62, 80, 0), false); c.addNote(Note(TimePos(12), TimePos(0), 64, 80, 0), false);
		AiBridgeProjectContext context; const auto generation = context.projectGeneration(); const auto aid = context.idForClip(&a); const auto bid = context.idForClip(&b); const auto cid = context.idForClip(&c);
		auto planFor = [&](MidiClip &clip, const QString &id, int key) { const auto before = snapshot(clip, id, generation); return AiBridgeMidiNativePlan{id, generation, computeMidiStateHash(before), {{24, 12, key, 100, 0}}}; };
		auto first = planFor(a, aid, 61); auto second = planFor(b, bid, 63); auto third = planFor(c, cid, 65);
		AiBridgeMidiActions::setTestHooks({[&](const AiBridgeMidiNativePlan &plan) { return plan.clipId == bid; }, {}});
		const auto result = AiBridgeMidiActions::execute({third, second, first});
		AiBridgeMidiActions::setTestHooks({});
		QVERIFY(!result.success); QCOMPARE(result.code, QStringLiteral("partial_failure")); QVERIFY(result.preflightSucceeded);
		QCOMPARE(result.failedPlan.clipId, bid); QCOMPARE(result.appliedPlans.size(), 1); QCOMPARE(result.appliedPlans[0].clipId, aid);
		QCOMPARE(result.unappliedPlans.size(), 2); QCOMPARE(result.unappliedPlans[0].clipId, bid); QCOMPARE(result.unappliedPlans[1].clipId, cid);
		QCOMPARE(snapshot(a, aid, generation).notes[0].key, 61); QCOMPARE(snapshot(b, bid, generation).notes[0].key, 62); QCOMPARE(snapshot(c, cid, generation).notes[0].key, 64);
	}
};

QTEST_MAIN(AiBridgeMidiActionsTest)
#include "AiBridgeMidiActionsTest.moc"
