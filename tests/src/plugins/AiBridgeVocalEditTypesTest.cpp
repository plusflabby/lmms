#include <QtTest>
#include <algorithm>
#include <limits>
#include "AiBridgeVocalEditTypes.h"
#include "AiBridgeVocalTypes.h"

using namespace lmms;

class AiBridgeVocalEditTypesTest : public QObject
{
	Q_OBJECT

	static QString digest(char value = 'a')
	{
		return QString(64, QChar(value));
	}

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
		return result;
	}

	static VocalEditRequest directRequest()
	{
		VocalEditRequest request;
		request.recipeVersion = 1;
		request.expectedProjectGeneration = 7;
		request.expectedWorldHash = digest();
		request.selections.push_back({"clip-a", digest('b'), VocalEditSourceBasis::SourcePcm, {100, 48100}});
		request.operations.push_back(VocalGainEnvelope{{{100, -6.0}, {48100, -6.0}}});
		return request;
	}

private slots:
	void parsesOnlyCanonicalNonnegativeFrames()
	{
		QCOMPARE(parseVocalEditFrame(QStringLiteral("0")), std::optional<qint64>(0));
		QCOMPARE(parseVocalEditFrame(QStringLiteral("9223372036854775807")),
			std::optional<qint64>(std::numeric_limits<qint64>::max()));
		for (const auto& text : {QString{}, QStringLiteral("00"), QStringLiteral("+1"), QStringLiteral("-1"),
			QStringLiteral("9223372036854775808"), QString(20, QChar('9'))})
		{
			QVERIFY(!parseVocalEditFrame(text));
		}
	}

	void acceptsExplicitBoundedDirectGainRecipe()
	{
		const auto request = directRequest();
		QCOMPARE(validateVocalEditRequest(request, limits()), QStringLiteral("ok"));
		const auto hash = canonicalVocalEditRecipeHash(request);
		QCOMPARE(hash.size(), 64);
		QVERIFY(std::all_of(hash.begin(), hash.end(), [](QChar c){return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');}));
	}

	void recipeHashChangesWhenAnEditDecisionChanges()
	{
		auto first = directRequest();
		auto second = directRequest();
		std::get<VocalGainEnvelope>(second.operations.front()).points[1].gainDb = -5.5;
		QVERIFY(canonicalVocalEditRecipeHash(first) != canonicalVocalEditRecipeHash(second));
		QCOMPARE(canonicalVocalEditRecipeHash(first), canonicalVocalEditRecipeHash(directRequest()));
	}

	void rejectsDuplicateSelectionsAndInvalidSourceRanges()
	{
		auto request = directRequest();
		request.selections.push_back(request.selections.front());
		QCOMPARE(validateVocalEditRequest(request, limits()), QStringLiteral("invalid_parameter"));

		request = directRequest();
		request.selections[0].sourceFrames = {48100, 100};
		QCOMPARE(validateVocalEditRequest(request, limits()), QStringLiteral("invalid_parameter"));
	}

	void rejectsNonfiniteAndUnorderedGainEnvelopePoints()
	{
		auto request = directRequest();
		std::get<VocalGainEnvelope>(request.operations.front()).points[1].gainDb =
			std::numeric_limits<double>::infinity();
		QCOMPARE(validateVocalEditRequest(request, limits()), QStringLiteral("invalid_parameter"));

		request = directRequest();
		std::get<VocalGainEnvelope>(request.operations.front()).points[1].renderInputFrame = 100;
		QCOMPARE(validateVocalEditRequest(request, limits()), QStringLiteral("invalid_parameter"));
	}

	void validatesExplicitFadeRangeAndOperationOrder()
	{
		auto request = directRequest();
		request.operations.clear();
		request.operations.push_back(VocalFade{{100, 500}, 0.0, -12.0, VocalFadeCurve::LinearAmplitude});
		QCOMPARE(validateVocalEditRequest(request, limits()), QStringLiteral("ok"));

		std::get<VocalFade>(request.operations.front()).renderInputFrames.inFrame = 99;
		QCOMPARE(validateVocalEditRequest(request, limits()), QStringLiteral("invalid_parameter"));

		request.operations.clear();
		request.operations.push_back(VocalTimingMap{{{100, 0, {}}, {48100, 60000, {}}}, {{200, 300}}});
		request.operations.push_back(VocalFade{{100, 500}, 0.0, -12.0, VocalFadeCurve::LinearAmplitude});
		QCOMPARE(validateVocalEditRequest(request, limits()), QStringLiteral("invalid_parameter"));
	}

	void validatesStrictFrameMapAndMutuallyExclusiveTargetUnits()
	{
		auto request = directRequest();
		request.operations.clear();
		request.operations.push_back(VocalTimingMap{{{100, 0, {}}, {48100, 60000, {}}}});
		QCOMPARE(validateVocalEditRequest(request, limits()), QStringLiteral("ok"));

		request.operations.clear();
		request.operations.push_back(VocalTimingMap{{{100, 0, {}}, {48100, 60000, 960}}});
		QCOMPARE(validateVocalEditRequest(request, limits()), QStringLiteral("invalid_parameter"));

		request.operations.clear();
		request.operations.push_back(VocalTimingMap{{{100, 0, {}}, {48100, 0, {}}}});
		QCOMPARE(validateVocalEditRequest(request, limits()), QStringLiteral("invalid_parameter"));

		auto strictLimits = limits();
		strictLimits.maximumAnchorErrorFrames = 1.01;
		QCOMPARE(validateVocalEditRequest(directRequest(), strictLimits), QStringLiteral("invalid_parameter"));
	}

	void validatesCompSelectionsAndDeclaredOverlap()
	{
		auto request = directRequest();
		request.operations.clear();
		request.selections.push_back({"clip-b", digest('c'), VocalEditSourceBasis::SourcePcm, {0, 1000}});
		request.input = VocalCompRecipe{
			{{0, {100, 1100}, {0, 1000}}, {1, {0, 1000}, {900, 1900}}},
			{{0, 1, 100, VocalCrossfadeLaw::LinearAmplitude}}
		};
		QCOMPARE(validateVocalEditRequest(request, limits()), QStringLiteral("ok"));

		std::get<VocalCompRecipe>(request.input).boundaries[0].overlapFrames = 12001;
		QCOMPARE(validateVocalEditRequest(request, limits()), QStringLiteral("resource_limit"));
	}

	void rejectsDuplicateTimingMapsAndUnsafeClipIdentifiers()
	{
		auto request = directRequest();
		request.operations.clear();
		const VocalTimingMap map{{{100, 0, {}}, {48100, 60000, {}}}};
		request.operations.push_back(map);
		request.operations.push_back(map);
		QCOMPARE(validateVocalEditRequest(request, limits()), QStringLiteral("invalid_parameter"));

		request = directRequest();
		request.selections[0].clipId = QStringLiteral("../source.wav");
		QCOMPARE(validateVocalEditRequest(request, limits()), QStringLiteral("invalid_parameter"));
	}

	void rejectsCompSegmentsThatCrossNonadjacentSections()
	{
		auto request = directRequest();
		request.operations.clear();
		request.selections.push_back({"clip-b", digest('c'), VocalEditSourceBasis::SourcePcm, {0, 1000}});
		request.input = VocalCompRecipe{
			{{0, {100, 1100}, {0, 1000}}, {1, {0, 1000}, {100, 200}}, {0, {1100, 2100}, {150, 1150}}},
			{{0, 1, 900, VocalCrossfadeLaw::LinearAmplitude}, {1, 2, 50, VocalCrossfadeLaw::LinearAmplitude}}
		};
		QCOMPARE(validateVocalEditRequest(request, limits()), QStringLiteral("invalid_parameter"));
	}
};

QTEST_APPLESS_MAIN(AiBridgeVocalEditTypesTest)
#include "AiBridgeVocalEditTypesTest.moc"
