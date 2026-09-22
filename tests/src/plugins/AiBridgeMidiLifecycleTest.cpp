#include <QtTest>

#include "AiBridgeMidiContext.h"
#include "AiBridgeProjectContext.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Note.h"
#include "Song.h"

namespace
{
lmms::AiBridgeMidiClipSnapshot snapshotFor(lmms::MidiClip &clip, const QString &clipId,
	quint64 generation)
{
	lmms::AiBridgeMidiClipSnapshot snapshot;
	snapshot.clipId = clipId;
	snapshot.projectGeneration = generation;
	for (const auto *note : clip.notes())
		snapshot.notes.append({note->pos().getTicks(), note->length().getTicks(), note->key(),
			note->getVolume(), note->getPanning()});
	return snapshot;
}
}

class AiBridgeMidiLifecycleTest : public QObject
{
	Q_OBJECT

private slots:
	void initTestCase()
	{
		lmms::Engine::init(true);
	}

	void oldMidiSnapshotTokensFailIndependentlyAfterMutation()
	{
		using namespace lmms;
		InstrumentTrack track(Engine::getSong());
		MidiClip clip(&track);
		clip.addNote(Note(TimePos(24), TimePos(12), 60, 80, 0), false);
		AiBridgeProjectContext project;
		const auto clipId = project.idForClip(&clip);
		const auto before = snapshotFor(clip, clipId, project.projectGeneration());
		const auto oldHash = computeMidiStateHash(before);
		const auto oldRef = makeMidiNoteRefs(before).at(0);

		clip.addNote(Note(TimePos(24), TimePos(36), 64, 90, 0), false);
		const auto current = snapshotFor(clip, clipId, project.projectGeneration());
		AiBridgeMidiSelection predicate;
		predicate.mode = AiBridgeMidiSelectionMode::Predicate;
		predicate.startTick = 0;
		predicate.endTick = 48;
		QCOMPARE(AiBridgeMidiContext::resolveSelection(current, oldHash, predicate).code,
			QStringLiteral("midi_state_mismatch"));

		AiBridgeMidiSelection exact;
		exact.mode = AiBridgeMidiSelectionMode::NoteRefs;
		exact.noteRefs.append(oldRef);
		QCOMPARE(AiBridgeMidiContext::resolveSelection(current, computeMidiStateHash(current), exact).code,
			QStringLiteral("stale_note_ref"));
	}

	void reloadMakesClipIdentityStaleAndReissuesIt()
	{
		using namespace lmms;
		InstrumentTrack track(Engine::getSong());
		MidiClip clip(&track);
		AiBridgeProjectContext project;
		const auto beforeGeneration = project.projectGeneration();
		const auto oldId = project.idForClip(&clip);
		QVERIFY(project.resolveClip(oldId) == &clip);
		QVERIFY(QMetaObject::invokeMethod(Engine::getSong(), "projectAboutToChange", Qt::DirectConnection));
		QVERIFY(project.projectGeneration() > beforeGeneration);
		QVERIFY(project.resolveClip(oldId) == nullptr);
		const auto freshId = project.idForClip(&clip);
		QVERIFY(freshId != oldId);
		QVERIFY(project.resolveClip(freshId) == &clip);
	}

	void deletedClipIdentityNeverResolvesAndRecreationIsFresh()
	{
		using namespace lmms;
		InstrumentTrack track(Engine::getSong());
		AiBridgeProjectContext project;
		QString deletedId;
		{
			auto *clip = new MidiClip(&track);
			deletedId = project.idForClip(clip);
			delete clip;
		}
		QVERIFY(project.resolveClip(deletedId) == nullptr);
		MidiClip recreated(&track);
		const auto recreatedId = project.idForClip(&recreated);
		QVERIFY(recreatedId != deletedId);
		QVERIFY(project.resolveClip(recreatedId) == &recreated);
	}

	void cleanupTestCase()
	{
		lmms::Engine::destroy();
	}
};

QTEST_MAIN(AiBridgeMidiLifecycleTest)
#include "AiBridgeMidiLifecycleTest.moc"
