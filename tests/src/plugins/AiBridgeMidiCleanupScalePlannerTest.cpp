#include <QtTest>

#include "AiBridgeMidiPlanner.h"

class AiBridgeMidiCleanupScalePlannerTest : public QObject
{
	Q_OBJECT

private slots:
	void removesOnlyExactDuplicates()
	{
		lmms::AiBridgeMidiClipSnapshot clip;
		clip.notes = {{0, 12, 60, 100, 0}, {0, 12, 60, 100, 0}, {0, 12, 60, 101, 0}, {0, 13, 60, 100, 0}};
		lmms::AiBridgeMidiCleanupOptions options;
		options.removeExactDuplicates = true;
		const auto result = lmms::planCleanup(clip, {0, 1, 2, 3}, options);
		QVERIFY(result.success);
		QCOMPARE(result.notes.size(), 3);
		QCOMPARE(result.removedCount, 1);
		QCOMPARE(result.notes[0].volume, 100);
		QCOMPARE(result.notes[1].volume, 101);
		QCOMPARE(result.notes[2].lengthTicks, qint64(13));
		options.removeExactDuplicates = false;
		const auto unchanged = lmms::planCleanup(clip, {0, 1, 2, 3}, options);
		QVERIFY(unchanged.success); QCOMPARE(unchanged.code, QStringLiteral("no_effect"));
	}

	void handlesSamePitchOverlapsWithoutAffectingPolyphony()
	{
		lmms::AiBridgeMidiClipSnapshot clip;
		clip.notes = {{0, 96, 60, 100, 0}, {72, 12, 60, 100, 0}, {72, 12, 62, 100, 0}};
		lmms::AiBridgeMidiCleanupOptions options;
		options.samePitchOverlapMode = lmms::AiBridgeMidiOverlapMode::TrimEarlier;
		const auto trimmed = lmms::planCleanup(clip, {0, 1, 2}, options);
		QVERIFY(trimmed.success);
		QCOMPARE(trimmed.notes[0].lengthTicks, qint64(72));
		QCOMPARE(trimmed.notes[2].lengthTicks, qint64(12));
		options.samePitchOverlapMode = lmms::AiBridgeMidiOverlapMode::Reject;
		const auto rejected = lmms::planCleanup(clip, {0, 1, 2}, options);
		QVERIFY(!rejected.success);
		QCOMPARE(rejected.code, QStringLiteral("overlap_rejected"));
		QCOMPARE(rejected.notes[0].lengthTicks, qint64(96));
	}

	void removesOrExtendsShortNotes()
	{
		lmms::AiBridgeMidiClipSnapshot clip;
		clip.notes = {{0, 6, 60, 100, 0}, {12, 12, 61, 100, 0}};
		lmms::AiBridgeMidiCleanupOptions options;
		options.minimumLengthTicks = 12;
		options.shortNoteAction = lmms::AiBridgeMidiShortNoteAction::Remove;
		const auto removed = lmms::planCleanup(clip, {0, 1}, options);
		QVERIFY(removed.success); QCOMPARE(removed.notes.size(), 1); QCOMPARE(removed.removedCount, 1);
		options.shortNoteAction = lmms::AiBridgeMidiShortNoteAction::Extend;
		const auto extended = lmms::planCleanup(clip, {0, 1}, options);
		QVERIFY(extended.success); QCOMPARE(extended.notes[0].lengthTicks, qint64(12));
	}

	void limitsCleanupMutationsToSelectedNotesWhileUsingFullClipContext()
	{
		lmms::AiBridgeMidiClipSnapshot duplicates;
		duplicates.notes = {{0, 12, 60, 100, 0}, {0, 12, 60, 100, 0}, {0, 12, 60, 100, 0}};
		lmms::AiBridgeMidiCleanupOptions duplicateOptions;
		duplicateOptions.removeExactDuplicates = true;
		const auto selectedDuplicate = lmms::planCleanup(duplicates, {1, 2}, duplicateOptions);
		QVERIFY(selectedDuplicate.success);
		QCOMPARE(selectedDuplicate.notes.size(), 1);
		QCOMPARE(selectedDuplicate.removedCount, 2);

		lmms::AiBridgeMidiClipSnapshot overlap;
		overlap.notes = {{0, 96, 60, 100, 0}, {72, 12, 60, 100, 0}};
		lmms::AiBridgeMidiCleanupOptions overlapOptions;
		overlapOptions.samePitchOverlapMode = lmms::AiBridgeMidiOverlapMode::TrimEarlier;
		const auto immutableEarlier = lmms::planCleanup(overlap, {1}, overlapOptions);
		QVERIFY(immutableEarlier.success);
		QCOMPARE(immutableEarlier.code, QStringLiteral("no_effect"));
		QCOMPARE(immutableEarlier.notes[0].lengthTicks, qint64(96));
		const auto selectedEarlier = lmms::planCleanup(overlap, {0}, overlapOptions);
		QVERIFY(selectedEarlier.success);
		QCOMPARE(selectedEarlier.notes[0].lengthTicks, qint64(72));

		overlapOptions.samePitchOverlapMode = lmms::AiBridgeMidiOverlapMode::Reject;
		const auto selectedLaterRejected = lmms::planCleanup(overlap, {1}, overlapOptions);
		QVERIFY(!selectedLaterRejected.success);
		QCOMPARE(selectedLaterRejected.code, QStringLiteral("overlap_rejected"));

		lmms::AiBridgeMidiCleanupOptions shortOptions;
		shortOptions.minimumLengthTicks = 12;
		shortOptions.shortNoteAction = lmms::AiBridgeMidiShortNoteAction::Extend;
		lmms::AiBridgeMidiClipSnapshot shortNotes;
		shortNotes.notes = {{0, 6, 60, 100, 0}, {12, 6, 61, 100, 0}};
		const auto scopedShort = lmms::planCleanup(shortNotes, {1}, shortOptions);
		QVERIFY(scopedShort.success);
		QCOMPARE(scopedShort.notes[0].lengthTicks, qint64(6));
		QCOMPARE(scopedShort.notes[1].lengthTicks, qint64(12));
	}

	void validatesExplicitPitchClasses()
	{
		lmms::AiBridgeMidiClipSnapshot clip;
		clip.notes = {{0, 12, 61, 100, 0}};
		lmms::AiBridgeMidiScaleConstraintOptions options;
		QVERIFY(!lmms::planScaleConstraint(clip, {0}, options).success);
		options.allowedPitchClasses = {0, 12};
		QVERIFY(!lmms::planScaleConstraint(clip, {0}, options).success);
		options.allowedPitchClasses = {0, 0};
		QVERIFY(!lmms::planScaleConstraint(clip, {0}, options).success);
	}

	void constrainsPitchClassesWithDirectionsTiesAndBounds()
	{
		lmms::AiBridgeMidiClipSnapshot clip;
		clip.notes = {{0, 12, 61, 100, 0}, {12, 12, 0, 100, 0}, {24, 12, 127, 100, 0}, {36, 12, 62, 100, 0}};
		lmms::AiBridgeMidiScaleConstraintOptions options;
		options.allowedPitchClasses = {0, 2, 5, 7, 10};
		options.direction = lmms::AiBridgeMidiScaleDirection::Nearest;
		options.nearestTieBreak = lmms::AiBridgeMidiNearestTieBreak::Down;
		QCOMPARE(lmms::planScaleConstraint(clip, {0}, options).notes[0].key, 60);
		options.nearestTieBreak = lmms::AiBridgeMidiNearestTieBreak::Up;
		QCOMPARE(lmms::planScaleConstraint(clip, {0}, options).notes[0].key, 62);
		options.direction = lmms::AiBridgeMidiScaleDirection::Up;
		QCOMPARE(lmms::planScaleConstraint(clip, {0, 1, 2, 3}, options).notes[0].key, 62);
		QCOMPARE(lmms::planScaleConstraint(clip, {0, 1, 2, 3}, options).notes[1].key, 0);
		QCOMPARE(lmms::planScaleConstraint(clip, {0, 1, 2, 3}, options).notes[2].key, 127);
		QCOMPARE(lmms::planScaleConstraint(clip, {0, 1, 2, 3}, options).notes[3].key, 62);
		options.direction = lmms::AiBridgeMidiScaleDirection::Down;
		QCOMPARE(lmms::planScaleConstraint(clip, {0}, options).notes[0].key, 60);
		options.allowedPitchClasses = {0};
		options.direction = lmms::AiBridgeMidiScaleDirection::Up;
		QCOMPARE(lmms::planScaleConstraint(clip, {2}, options).notes[2].key, 127);
		options.direction = lmms::AiBridgeMidiScaleDirection::Down;
		QCOMPARE(lmms::planScaleConstraint(clip, {1}, options).notes[1].key, 0);
		const auto unchanged = lmms::planScaleConstraint(clip, {1}, options);
		QVERIFY(unchanged.success); QCOMPARE(unchanged.code, QStringLiteral("no_effect"));
	}
};

QTEST_MAIN(AiBridgeMidiCleanupScalePlannerTest)
#include "AiBridgeMidiCleanupScalePlannerTest.moc"
