#include <QtTest>

#include "AiBridgeMidiAnalysis.h"

class AiBridgeMidiAnalysisTest : public QObject
{
	Q_OBJECT

private slots:
	void reportsDescriptiveStatistics()
	{
		lmms::AiBridgeMidiClipSnapshot clip;
		clip.notes = {{0, 24, 60, 80, 0}, {12, 12, 64, 100, 0}, {24, 48, 67, 120, 0}};

		const auto result = lmms::analyzeMidi({clip}, {});
		QVERIFY(result.success);
		QCOMPARE(result.noteCount, 3);
		QCOMPARE(result.distinctOnsetCount, 3);
		QCOMPARE(result.pitchMinimum, 60);
		QCOMPARE(result.pitchMaximum, 67);
		QCOMPARE(result.pitchClassHistogram.size(), 12);
		QCOMPARE(result.pitchClassHistogram[0], 1);
		QCOMPARE(result.pitchClassHistogram[4], 1);
		QCOMPARE(result.pitchClassHistogram[7], 1);
		QCOMPARE(result.volumeMinimum, 80);
		QCOMPARE(result.volumeMaximum, 120);
		QCOMPARE(result.volumeMean, 100.0);
		QCOMPARE(result.lengthMinimumTicks, qint64(12));
		QCOMPARE(result.lengthMaximumTicks, qint64(48));
		QCOMPARE(result.lengthMeanTicks, 28.0);
	}

	void countsExplicitTechnicalDefects()
	{
		lmms::AiBridgeMidiClipSnapshot clip;
		clip.notes = {{100, 10, 61, 100, 0}, {100, 10, 61, 100, 0},
			{0, 24, 60, 100, 0}, {12, 24, 60, 90, 0},
			{50, 1, 62, 100, 0}, {60, 2, 63, 100, 0}};
		lmms::AiBridgeMidiAnalysisOptions options;
		options.shortNoteThresholdTicks = 3;

		const auto result = lmms::analyzeMidi({clip}, options);
		QVERIFY(result.success);
		QCOMPARE(result.exactDuplicateCount, 1);
		QCOMPARE(result.samePitchOverlapCount, 1);
		QCOMPARE(result.shortNoteCount, 2);
	}

	void measuresGridDeviationWithEarlierTieBreak()
	{
		lmms::AiBridgeMidiClipSnapshot clip;
		clip.notes = {{0, 1, 60, 100, 0}, {12, 1, 61, 100, 0},
			{25, 1, 62, 100, 0}, {47, 1, 63, 100, 0}};
		lmms::AiBridgeMidiAnalysisOptions options;
		options.gridTicks = 24;

		const auto result = lmms::analyzeMidi({clip}, options);
		QVERIFY(result.success);
		QVERIFY(result.gridDeviation.has_value());
		QCOMPARE(result.gridDeviation->exactOnGridCount, 1);
		QCOMPARE(result.gridDeviation->meanAbsoluteDeviationTicks, 3.5);
		QCOMPARE(result.gridDeviation->maximumAbsoluteDeviationTicks, qint64(12));
	}

	void rejectsAggregateInputAboveAnalysisCap()
	{
		QVector<lmms::AiBridgeMidiClipSnapshot> clips;
		for (int index = 0; index < 4097; ++index)
		{
			lmms::AiBridgeMidiClipSnapshot clip;
			clip.notes = {{index, 1, 60, 100, 0}};
			clips.append(clip);
		}

		const auto result = lmms::analyzeMidi(clips, {});
		QVERIFY(!result.success);
		QCOMPARE(result.code, QStringLiteral("note_limit_exceeded"));
	}
};

QTEST_MAIN(AiBridgeMidiAnalysisTest)
#include "AiBridgeMidiAnalysisTest.moc"
