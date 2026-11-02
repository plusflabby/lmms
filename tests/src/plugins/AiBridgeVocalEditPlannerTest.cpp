#include <QtTest>
#include <cmath>
#include "AiBridgeVocalEditPlanner.h"
#include "AiBridgeVocalEditTypes.h"

using namespace lmms;

class AiBridgeVocalEditPlannerTest : public QObject
{
	Q_OBJECT

	static QString digest(char value = 'a') { return QString(64, QChar(value)); }
	static VocalEditLimits limits()
	{
		VocalEditLimits result;
		result.maxSelections = 8;
		result.maxTimingAnchors = 256;
		result.maxGainFadePoints = 1024;
		result.maxCompSegments = 8;
		result.maxCrossfadeFrames = 12000;
		result.minimumGainDb = -48.0;
		result.maximumGainDb = 24.0;
		result.minimumLocalStretchRatio = 0.80;
		result.maximumLocalStretchRatio = 1.25;
		result.maximumAnchorErrorFrames = 1.0;
		return result;
	}
	static VocalEditRequest timingRequest()
	{
		VocalEditRequest request;
		request.recipeVersion = 1;
		request.expectedProjectGeneration = 7;
		request.expectedWorldHash = digest();
		request.selections.push_back({"clip-a", digest('b'), VocalEditSourceBasis::SourcePcm, {0, 48000}});
		request.operations.push_back(VocalTimingMap{{{0, 0, {}}, {48000, 60000, {}}}});
		return request;
	}
	static VocalEditPlanningSnapshot snapshot(int rate=48000, int tempo=120)
	{
		VocalEditPlanningSnapshot value;
		value.generation = 7;
		value.worldHash = digest();
		value.world = {tempo, rate, 48, false};
		value.sources.push_back({"clip-a", digest('b'), 48000, rate, 1, true});
		return value;
	}

private slots:
	void mapsExplicitSourceFramesToOutputFrames()
	{
		const auto request = timingRequest();
		const auto result = AiBridgeVocalEditPlanner::plan(request, snapshot(), limits());
		QCOMPARE(result.code, QStringLiteral("ok"));
		QVERIFY(result.value);
		QCOMPARE(result.value->recipeHash, canonicalVocalEditRecipeHash(request));
		QCOMPARE(result.value->anchorPreviews.size(), size_t(2));
		QCOMPARE(result.value->anchorPreviews[0].outputFrame, qint64(0));
		QCOMPARE(result.value->anchorPreviews[1].outputFrame, qint64(60000));
		QCOMPARE(result.value->anchorPreviews[1].roundingErrorFrames, 0.0);
	}

	void reportsTickConversionRemainder()
	{
		auto request = timingRequest();
		request.selections[0].sourceFrames = {0, 1000};
		request.operations.clear();
		request.operations.push_back(VocalTimingMap{{{0, 0, {}}, {1000, {}, 2}}});
		auto world = snapshot(44100, 127);
		world.sources[0].sourceFrames = 1000;
		const auto result = AiBridgeVocalEditPlanner::plan(request, world, limits());
		QCOMPARE(result.code, QStringLiteral("ok"));
		QVERIFY(result.value);
		QCOMPARE(result.value->anchorPreviews[1].targetTick, std::optional<qint64>(2));
		QCOMPARE(result.value->anchorPreviews[1].outputFrame, qint64(868));
		QVERIFY(std::abs(result.value->anchorPreviews[1].roundingErrorFrames) < 0.5);
		QCOMPARE(result.value->anchorPreviews[1].roundingErrorNumerator, qint64(-14));
		QCOMPARE(result.value->anchorPreviews[1].roundingErrorDenominator, qint64(127));
		QCOMPARE(result.value->roundingPolicy, VocalTickFrameRoundingPolicy::NearestTiesUp);
	}

	void refusesAutomationStaleHashesAndUnsupportedRates()
	{
		auto request = timingRequest();
		auto world = snapshot();
		world.world.tempoAutomationPresent = true;
		QCOMPARE(AiBridgeVocalEditPlanner::plan(request, world, limits()).code,
			QStringLiteral("unsupported_playback_world"));

		world = snapshot();
		world.worldHash = digest('c');
		QCOMPARE(AiBridgeVocalEditPlanner::plan(request, world, limits()).code,
			QStringLiteral("plan_stale"));

		world = snapshot();
		world.sources[0].audioStateHash = digest('d');
		QCOMPARE(AiBridgeVocalEditPlanner::plan(request, world, limits()).code,
			QStringLiteral("clip_state_mismatch"));

		world = snapshot(32000, 120);
		world.sources[0].sourceFrames = 48000;
		QCOMPARE(AiBridgeVocalEditPlanner::plan(request, world, limits()).code,
			QStringLiteral("unsupported_sample_rate"));
	}

	void rejectsOutOfRangeStretchAndOutputDuration()
	{
		auto request = timingRequest();
		request.operations.clear();
		request.operations.push_back(VocalTimingMap{{{0, 0, {}}, {48000, 70000, {}}}});
		QCOMPARE(AiBridgeVocalEditPlanner::plan(request, snapshot(), limits()).code,
			QStringLiteral("unsupported_stretch_ratio"));

		request.selections[0].sourceFrames = {0, 5000000};
		request.operations.clear();
		request.operations.push_back(VocalTimingMap{{{0, 0, {}}, {5000000, 6000000, {}}}});
		auto largeSnapshot = snapshot();
		largeSnapshot.sources[0].sourceFrames = 5000000;
		QCOMPARE(AiBridgeVocalEditPlanner::plan(request, largeSnapshot, limits()).code,
			QStringLiteral("resource_limit"));
	}

	void mapsCompTimingInAssembledRenderInputCoordinates()
	{
		auto request = timingRequest();
		request.selections.push_back({"clip-b", digest('c'), VocalEditSourceBasis::SourcePcm, {0, 1000}});
		request.input = VocalCompRecipe{
			{{0, {0, 1000}, {0, 1000}}, {1, {0, 1000}, {900, 1900}}},
			{{0, 1, 100, VocalCrossfadeLaw::LinearAmplitude}}
		};
		request.operations.clear();
		request.operations.push_back(VocalTimingMap{{{0, 0, {}}, {1900, 2280, {}}}});
		auto world = snapshot();
		world.sources.push_back({"clip-b", digest('c'), 1000, 48000, 1, true});

		const auto result = AiBridgeVocalEditPlanner::plan(request, world, limits());
		QCOMPARE(result.code, QStringLiteral("ok"));
		QVERIFY(result.value);
		QCOMPARE(result.value->anchorPreviews.back().renderInputFrame, qint64(1900));
		QCOMPARE(result.value->anchorPreviews.back().outputFrame, qint64(2280));
	}
};

QTEST_APPLESS_MAIN(AiBridgeVocalEditPlannerTest)
#include "AiBridgeVocalEditPlannerTest.moc"
