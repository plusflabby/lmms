#include <QtTest>

#include <algorithm>

#include "AiBridgeMidiContext.h"

class AiBridgeMidiContextTest : public QObject
{
	Q_OBJECT

private slots:
	void reportsFrozenSourceBackedCapabilities()
	{
		const auto capabilities = lmms::AiBridgeMidiContext::getCapabilities();
		QCOMPARE(capabilities.keyMinimum, 0);
		QCOMPARE(capabilities.keyMaximum, 127);
		QCOMPARE(capabilities.volumeMinimum, 0);
		QCOMPARE(capabilities.volumeMaximum, 200);
		QCOMPARE(capabilities.panningMinimum, -100);
		QCOMPARE(capabilities.panningMaximum, 100);
		QCOMPARE(capabilities.minimumLengthTicks, qint64(1));
		QVERIFY(capabilities.notePanningSupported);
		QCOMPARE(capabilities.maxClipsPerMutation, 32);
		QCOMPARE(capabilities.maxSelectedNotesPerMutation, 2048);
		QCOMPARE(capabilities.pageLimit, 512);
		QCOMPARE(capabilities.maxPreviewEntries, 128);
		QCOMPARE(capabilities.maxAnalysisNotes, 4096);
		QCOMPARE(capabilities.humanizeDistributions, QStringList({QStringLiteral("triangular"), QStringLiteral("uniform")}));
	}

	void pagesCanonicalCopiedSnapshotsWithoutChangingStateHash()
	{
		lmms::AiBridgeMidiClipSnapshot snapshot;
		snapshot.clipId = QStringLiteral("clip:page");
		snapshot.projectGeneration = 2;
		for (int index = 0; index < 513; ++index) snapshot.notes.append({512 - index, 1, 60, 100, 0});

		const auto first = lmms::AiBridgeMidiContext::pageNotes(snapshot, 0, 512);
		const auto second = lmms::AiBridgeMidiContext::pageNotes(snapshot, 512, 512);
		QCOMPARE(first.total, 513);
		QCOMPARE(first.notes.size(), 512);
		QVERIFY(first.hasMore);
		QCOMPARE(second.notes.size(), 1);
		QVERIFY(!second.hasMore);
		QCOMPARE(first.midiStateHash, second.midiStateHash);
		QCOMPARE(first.notes.front().positionTicks, qint64(0));

		auto reordered = snapshot;
		std::reverse(reordered.notes.begin(), reordered.notes.end());
		QCOMPARE(lmms::AiBridgeMidiContext::pageNotes(reordered, 0, 512).midiStateHash,
			first.midiStateHash);
	}

	void resolvesExactAndPredicateSelectionAndRejectsStaleState()
	{
		lmms::AiBridgeMidiClipSnapshot snapshot;
		snapshot.clipId = QStringLiteral("clip:select");
		snapshot.projectGeneration = 2;
		snapshot.notes = {{0, 24, 60, 100, 0}, {24, 12, 64, 80, -5}};
		const auto page = lmms::AiBridgeMidiContext::pageNotes(snapshot, 0, 2);

		lmms::AiBridgeMidiSelection exact;
		exact.noteRefs = {page.noteRefs[1]};
		const auto exactResult = lmms::AiBridgeMidiContext::resolveSelection(snapshot, page.midiStateHash, exact);
		QVERIFY(exactResult.success);
		QCOMPARE(exactResult.canonicalIndices, QVector<int>({1}));

		lmms::AiBridgeMidiSelection predicate;
		predicate.mode = lmms::AiBridgeMidiSelectionMode::Predicate;
		predicate.startTick = 0;
		predicate.endTick = 24;
		predicate.keyMin = 60;
		predicate.keyMax = 60;
		predicate.volumeMin = 100;
		predicate.volumeMax = 100;
		predicate.lengthMin = 24;
		predicate.lengthMax = 24;
		const auto predicateResult = lmms::AiBridgeMidiContext::resolveSelection(snapshot, page.midiStateHash, predicate);
		QVERIFY(predicateResult.success);
		QCOMPARE(predicateResult.canonicalIndices, QVector<int>({0}));

		const auto stale = lmms::AiBridgeMidiContext::resolveSelection(snapshot, QStringLiteral("stale"), exact);
		QVERIFY(!stale.success);
		QCOMPARE(stale.code, QStringLiteral("midi_state_mismatch"));

		auto changed = snapshot;
		changed.notes[1].volume++;
		const auto staleRef = lmms::AiBridgeMidiContext::resolveSelection(changed,
			lmms::computeMidiStateHash(changed), exact);
		QVERIFY(!staleRef.success);
		QCOMPARE(staleRef.code, QStringLiteral("stale_note_ref"));
	}

	void rejectsMixedSelectionAndEnforcesSelectedNoteCapBeforeConsumers()
	{
		lmms::AiBridgeMidiClipSnapshot snapshot;
		snapshot.clipId = QStringLiteral("clip:limits");
		snapshot.projectGeneration = 2;
		for (int index = 0; index < 2049; ++index) snapshot.notes.append({index, 1, 60, 100, 0});
		const auto page = lmms::AiBridgeMidiContext::pageNotes(snapshot, 0, 1);

		lmms::AiBridgeMidiSelection mixed;
		mixed.noteRefs = {page.noteRefs.front()};
		mixed.startTick = 0;
		mixed.endTick = 1;
		const auto mixedResult = lmms::AiBridgeMidiContext::resolveSelection(snapshot,
			page.midiStateHash, mixed);
		QVERIFY(!mixedResult.success);
		QCOMPARE(mixedResult.code, QStringLiteral("invalid_selection"));

		lmms::AiBridgeMidiSelection all;
		all.mode = lmms::AiBridgeMidiSelectionMode::Predicate;
		all.keyMin = 0;
		all.keyMax = 127;
		const auto oversized = lmms::AiBridgeMidiContext::resolveSelection(snapshot,
			page.midiStateHash, all);
		QVERIFY(!oversized.success);
		QCOMPARE(oversized.code, QStringLiteral("note_limit_exceeded"));
	}

	void appliesExactKeysAsAnAndedPredicateFilter()
	{
		lmms::AiBridgeMidiClipSnapshot snapshot;
		snapshot.clipId = QStringLiteral("clip:keys");
		snapshot.projectGeneration = 3;
		snapshot.notes = {{0, 12, 36, 90, 0}, {12, 24, 42, 100, 0},
			{24, 12, 46, 110, 0}, {24, 12, 48, 110, 0}};
		const auto hash = lmms::computeMidiStateHash(snapshot);

		lmms::AiBridgeMidiSelection keys;
		keys.mode = lmms::AiBridgeMidiSelectionMode::Predicate;
		keys.keys = {36, 46};
		const auto exactKeys = lmms::AiBridgeMidiContext::resolveSelection(snapshot, hash, keys);
		QVERIFY(exactKeys.success);
		QCOMPARE(exactKeys.canonicalIndices, QVector<int>({0, 2}));

		keys.startTick = 12;
		keys.endTick = 25;
		const auto tickAndKeys = lmms::AiBridgeMidiContext::resolveSelection(snapshot, hash, keys);
		QVERIFY(tickAndKeys.success);
		QCOMPARE(tickAndKeys.canonicalIndices, QVector<int>({2}));

		keys.volumeMin = 111;
		keys.volumeMax = 120;
		const auto allFilters = lmms::AiBridgeMidiContext::resolveSelection(snapshot, hash, keys);
		QVERIFY(allFilters.success);
		QCOMPARE(allFilters.canonicalIndices, QVector<int>());

		lmms::AiBridgeMidiSelection empty;
		empty.mode = lmms::AiBridgeMidiSelectionMode::Predicate;
		QVERIFY(!empty.validate().success());
		empty.keys = {36, 128};
		QVERIFY(!empty.validate().success());
	}
};

QTEST_MAIN(AiBridgeMidiContextTest)
#include "AiBridgeMidiContextTest.moc"
