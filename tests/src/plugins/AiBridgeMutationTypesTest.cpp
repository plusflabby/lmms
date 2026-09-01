#include <QtTest>

#include "AiBridgeMutationTypes.h"

class AiBridgeMutationTypesTest : public QObject
{
	Q_OBJECT

private slots:
	void canonicalHashIsStableForEquivalentCopiedState()
	{
		lmms::AiBridgeMutationState first;
		first.clip_length_ticks = 480;
		first.auto_resize = true;
		lmms::AiBridgeCopiedNote copiedNote;
		copiedNote.position_ticks = 0;
		copiedNote.length_ticks = 120;
		copiedNote.key = 60;
		copiedNote.volume = 100;
		copiedNote.panning = 0;
		copiedNote.note_type = QStringLiteral("regular");
		first.notes.append(copiedNote);
		lmms::AiBridgeMutationState second;
		second.clip_length_ticks = 480;
		second.auto_resize = true;
		second.notes.append(copiedNote);
		QCOMPARE(lmms::AiBridgeMutationTypes::contentHash(first), lmms::AiBridgeMutationTypes::contentHash(second));
	}

	void rejectsKeyOutsideMidiDomain()
	{
		lmms::AiBridgeCopiedNote note;
		note.position_ticks = 0;
		note.length_ticks = 120;
		note.key = 128;
		note.volume = 100;
		note.panning = 0;
		note.note_type = QStringLiteral("regular");
		const auto result = lmms::AiBridgeMutationTypes::validateNoteInput(note);
		QCOMPARE(result.code, QStringLiteral("invalid_note"));
	}

	void canonicalHashIncludesCopiedStateAndIgnoresInputOrder()
	{
		lmms::AiBridgeCopiedNote firstNote;
		firstNote.position_ticks = 240;
		firstNote.length_ticks = 120;
		firstNote.key = 64;
		firstNote.volume = 90;
		firstNote.panning = -10;
		firstNote.note_type = QStringLiteral("step");
		lmms::AiBridgeCopiedNote secondNote;
		secondNote.position_ticks = 0;
		secondNote.length_ticks = 240;
		secondNote.key = 60;
		secondNote.volume = 100;
		secondNote.panning = 0;
		secondNote.note_type = QStringLiteral("regular");

		lmms::AiBridgeMutationState first;
		first.clip_length_ticks = 480;
		first.notes.append(firstNote);
		first.notes.append(secondNote);
		lmms::AiBridgeMutationState reordered;
		reordered.clip_length_ticks = 480;
		reordered.notes.append(secondNote);
		reordered.notes.append(firstNote);
		QCOMPARE(lmms::AiBridgeMutationTypes::contentHash(first),
			lmms::AiBridgeMutationTypes::contentHash(reordered));

		reordered.auto_resize = true;
		QVERIFY(lmms::AiBridgeMutationTypes::contentHash(first) !=
			lmms::AiBridgeMutationTypes::contentHash(reordered));
	}

	void acceptsBoundaryVolumeAndPanning()
	{
		lmms::AiBridgeCopiedNote note;
		note.length_ticks = 1;
		note.key = 0;
		note.volume = 200;
		note.panning = -100;
		QVERIFY(lmms::AiBridgeMutationTypes::validateNoteInput(note).success());
		note.volume = 201;
		QCOMPARE(lmms::AiBridgeMutationTypes::validateNoteInput(note).code,
			QStringLiteral("invalid_note"));
		note.volume = 200;
		note.panning = 101;
		QCOMPARE(lmms::AiBridgeMutationTypes::validateNoteInput(note).code,
			QStringLiteral("invalid_note"));
	}

	void validatesBoundedSelectionAndMatchesCopiedNote()
	{
		lmms::AiBridgeNoteSelection selection;
		selection.position_start = 0;
		selection.position_end = 480;
		selection.key_min = 60;
		selection.key_max = 72;
		const auto validation = lmms::AiBridgeMutationTypes::validateSelection(selection, true);
		QVERIFY(validation.success());

		lmms::AiBridgeCopiedNote note;
		note.position_ticks = 120;
		note.length_ticks = 120;
		note.key = 64;
		note.volume = 100;
		note.panning = 0;
		note.note_type = QStringLiteral("regular");
		QVERIFY(lmms::AiBridgeMutationTypes::matchesSelection(note, selection, 3));

		selection.position_end = 0;
		QVERIFY(!lmms::AiBridgeMutationTypes::validateSelection(selection, true).success());
	}

	void rejectsExactOnlyMutationSelection()
	{
		lmms::AiBridgeNoteSelection selection;
		selection.exact = lmms::AiBridgeExactNote{};
		const auto validation = lmms::AiBridgeMutationTypes::validateSelection(selection, true);
		QCOMPARE(validation.code, QStringLiteral("selection_requires_range"));
	}
};

QTEST_MAIN(AiBridgeMutationTypesTest)
#include "AiBridgeMutationTypesTest.moc"
