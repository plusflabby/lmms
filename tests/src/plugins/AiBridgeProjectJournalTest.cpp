#include <QtTest>

#include "Engine.h"
#include "DetuningHelper.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Note.h"
#include "ProjectJournal.h"
#include "Song.h"

class AiBridgeProjectJournalTest : public QObject
{
	Q_OBJECT

private slots:
	void initTestCase()
	{
		lmms::Engine::init(true);
	}

	void cleanupTestCase()
	{
		lmms::Engine::destroy();
	}

	void groupedMidiMutationRestoresThroughOneNativeCheckpoint()
	{
		using namespace lmms;
		Engine::projectJournal()->clearJournal();
		Song *song = Engine::getSong();
		InstrumentTrack track(song);
		MidiClip clip(&track);
		Note initial(TimePos(120), TimePos(0), 60, 140, -20);
		initial.setType(Note::Type::Step);
		clip.addNote(initial, false);
		const auto original = clip.notes().at(0)->clone();

		Engine::projectJournal()->clearJournal();
		clip.addJournalCheckPoint();
		Note added(TimePos(240), TimePos(480), 72, 80, 30);
		clip.addNote(added, false);
		clip.notes().at(0)->setKey(61);
		clip.rearrangeAllNotes();
		QVERIFY(Engine::projectJournal()->canUndo());

		Engine::projectJournal()->undo();
		QCOMPARE(clip.notes().size(), std::size_t(1));
		QCOMPARE(clip.notes().at(0)->pos().getTicks(), original->pos().getTicks());
		QCOMPARE(clip.notes().at(0)->length().getTicks(), original->length().getTicks());
		QCOMPARE(clip.notes().at(0)->key(), original->key());
		QCOMPARE(clip.notes().at(0)->getVolume(), original->getVolume());
		QCOMPARE(clip.notes().at(0)->getPanning(), original->getPanning());
		QCOMPARE(clip.notes().at(0)->type(), original->type());
		delete original;
	}

	void noOpPathDoesNotCreateJournalEntry()
	{
		using namespace lmms;
		Engine::getSong()->clearProject();
		Engine::projectJournal()->clearJournal();
		const bool modifiedBefore = Engine::getSong()->isModified();
		QVERIFY(!Engine::projectJournal()->canUndo());
		QCOMPARE(Engine::getSong()->isModified(), modifiedBefore);
	}

	void nativeNoteCopyPreservesExcludedDetuningState()
	{
		using namespace lmms;
		auto detuning = std::make_shared<DetuningHelper>();
		Note original(TimePos(120), TimePos(0), 60, 100, 0, detuning);
		original.setType(Note::Type::Step);
		Note copy(original);
		QCOMPARE(copy.type(), original.type());
		QVERIFY(copy.detuning() != nullptr);
		QCOMPARE(copy.detuning().get(), original.detuning().get());
	}
};

QTEST_MAIN(AiBridgeProjectJournalTest)
#include "AiBridgeProjectJournalTest.moc"
