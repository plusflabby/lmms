#include <QtTest>

#include <algorithm>

#include "AiBridgeMidiTypes.h"

class AiBridgeMidiTypesTest : public QObject
{
	Q_OBJECT

private slots:
	void canonicalSnapshotsHashVisibleStateIndependentOfInputOrder()
	{
		lmms::AiBridgeMidiClipSnapshot first;
		first.clipId = QStringLiteral("clip:alpha");
		first.projectGeneration = 7;
		first.notes = {
			{96, 24, 64, 80, 0},
			{0, 48, 60, 100, -20},
		};
		auto reordered = first;
		std::swap(reordered.notes[0], reordered.notes[1]);

		const auto canonical = lmms::canonicalizeMidiNotes(first.notes);
		QCOMPARE(canonical[0].positionTicks, qint64(0));
		QCOMPARE(canonical[1].positionTicks, qint64(96));
		QCOMPARE(lmms::computeMidiStateHash(first), lmms::computeMidiStateHash(reordered));

		auto changed = first;
		changed.notes[0].volume++;
		QVERIFY(lmms::computeMidiStateHash(first) != lmms::computeMidiStateHash(changed));
		changed = first;
		changed.notes[0].positionTicks++;
		QVERIFY(lmms::computeMidiStateHash(first) != lmms::computeMidiStateHash(changed));
		changed = first;
		changed.notes[0].lengthTicks++;
		QVERIFY(lmms::computeMidiStateHash(first) != lmms::computeMidiStateHash(changed));
		changed = first;
		changed.notes[0].key++;
		QVERIFY(lmms::computeMidiStateHash(first) != lmms::computeMidiStateHash(changed));
		changed = first;
		changed.notes[0].panning++;
		QVERIFY(lmms::computeMidiStateHash(first) != lmms::computeMidiStateHash(changed));
	}

	void noteReferencesAreOpaqueSnapshotBoundAndDistinctForDuplicates()
	{
		lmms::AiBridgeMidiClipSnapshot snapshot;
		snapshot.clipId = QStringLiteral("clip:alpha");
		snapshot.projectGeneration = 7;
		snapshot.notes = {{0, 24, 60, 100, 0}, {0, 24, 60, 100, 0}};

		const auto refs = lmms::makeMidiNoteRefs(snapshot);
		QCOMPARE(refs.size(), 2);
		QVERIFY(refs[0] != refs[1]);
		QVERIFY(!refs[0].contains(QStringLiteral("0x")));
		QVERIFY(!refs[0].contains(QStringLiteral("index")));
		QVERIFY(lmms::resolveMidiNoteRef(snapshot, refs[0]).ok);

		auto changed = snapshot;
		changed.notes[0].volume = 99;
		const auto stale = lmms::resolveMidiNoteRef(changed, refs[0]);
		QVERIFY(!stale.ok);
		QCOMPARE(stale.code, QStringLiteral("stale_note_ref"));
	}

	void selectionAndCentralizedCapsRejectMixedSelection()
	{
		QCOMPARE(lmms::AiBridgeMidiLimits::maxClipsPerMutation, 32);
		QCOMPARE(lmms::AiBridgeMidiLimits::maxSelectedNotesPerMutation, 2048);
		QCOMPARE(lmms::AiBridgeMidiLimits::maxNotesPerPage, 512);
		QCOMPARE(lmms::AiBridgeMidiLimits::maxPreviewEntries, 128);
		QCOMPARE(lmms::AiBridgeMidiLimits::maxAnalysisNotes, 4096);

		lmms::AiBridgeMidiSelection selection;
		selection.noteRefs = {QStringLiteral("note-ref")};
		selection.keyMin = 60;
		QCOMPARE(selection.validate().code, QStringLiteral("invalid_selection"));
	}
};

QTEST_MAIN(AiBridgeMidiTypesTest)
#include "AiBridgeMidiTypesTest.moc"
