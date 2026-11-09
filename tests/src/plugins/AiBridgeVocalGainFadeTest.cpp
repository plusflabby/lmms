#include <QtTest>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>
#include "AiBridgeVocalGainFade.h"

using namespace lmms;

class AiBridgeVocalGainFadeTest : public QObject
{
	Q_OBJECT

	static QString process(const VocalEditFrameRange& range,
		const std::vector<VocalEditOperation>& operations,
		qint64 firstFrame,
		std::span<const SampleFrame> input,
		int channels,
		std::span<float> output)
	{
		return AiBridgeVocalGainFade::process(range, operations, firstFrame, input, channels, output);
	}

	static bool near(float actual, double expected, double tolerance = 1e-6)
	{
		return std::abs(double(actual) - expected) <= tolerance;
	}

private slots:
	void zeroDecibelEnvelopeIsBitExactAndDoesNotChangeDuration()
	{
		const std::array<SampleFrame, 4> input{
			SampleFrame(0.0f, -0.0f), SampleFrame(0.125f, -0.25f),
			SampleFrame(1.0f, -1.0f), SampleFrame(0.75f, 0.5f)};
		const std::vector<VocalEditOperation> operations{
			VocalGainEnvelope{{{0, 0.0}, {4, 0.0}}}};
		std::vector<float> output(8, 9.0f);
		QCOMPARE(process({0, 4}, operations, 0, input, 2, output), QStringLiteral("ok"));
		QCOMPARE(output.size(), size_t(8));
		for (size_t frame = 0; frame < input.size(); ++frame)
		{
			QCOMPARE(output[frame * 2], input[frame].left());
			QCOMPARE(output[frame * 2 + 1], input[frame].right());
		}
	}

	void gainEnvelopeChangesOnlyItsExplicitPointSpan()
	{
		const std::array<SampleFrame, 7> input{
			SampleFrame(0.0f), SampleFrame(0.25f), SampleFrame(0.5f),
			SampleFrame(0.75f), SampleFrame(1.0f), SampleFrame(0.125f), SampleFrame(0.33f)};
		const std::vector<VocalEditOperation> operations{
			VocalGainEnvelope{{{2, 0.0}, {4, -6.0}, {6, 0.0}}}};
		std::vector<float> output(input.size(), 9.0f);
		QCOMPARE(process({0, 7}, operations, 0, input, 1, output), QStringLiteral("ok"));
		QCOMPARE(output[0], input[0].left());
		QCOMPARE(output[1], input[1].left());
		QCOMPARE(output[2], input[2].left());
		QVERIFY(near(output[3], double(input[3].left()) * std::pow(10.0, -3.0 / 20.0)));
		QVERIFY(near(output[4], double(input[4].left()) * std::pow(10.0, -6.0 / 20.0)));
		QVERIFY(near(output[5], double(input[5].left()) * std::pow(10.0, -3.0 / 20.0)));
		QCOMPARE(output[6], input[6].left());
	}

	void linearFadeHonorsHalfOpenFramesAndPreservesBreathsOutside()
	{
		const std::array<SampleFrame, 6> input{
			SampleFrame(0.0f), SampleFrame(0.05f), SampleFrame(0.1f),
			SampleFrame(0.2f), SampleFrame(0.4f), SampleFrame(0.0f)};
		const std::vector<VocalEditOperation> operations{
			VocalFade{{0, 4}, -6.0, 0.0, VocalFadeCurve::LinearAmplitude}};
		std::vector<float> output(input.size(), 9.0f);
		QCOMPARE(process({0, 6}, operations, 0, input, 1, output), QStringLiteral("ok"));
		const auto startGain = std::pow(10.0, -6.0 / 20.0);
		QCOMPARE(output[0], input[0].left());
		QVERIFY(near(output[1], double(input[1].left()) * (startGain + (1.0 - startGain) / 3.0)));
		QVERIFY(near(output[2], double(input[2].left()) * (startGain + 2.0 * (1.0 - startGain) / 3.0)));
		QCOMPARE(output[3], input[3].left());
		QCOMPARE(output[4], input[4].left());
		QCOMPARE(output[5], input[5].left());
	}

	void equalPowerAndSingleFrameFadeHaveDeterministicEndpoints()
	{
		const std::array<SampleFrame, 3> input{SampleFrame(1.0f), SampleFrame(1.0f), SampleFrame(1.0f)};
		const std::vector<VocalEditOperation> equalPower{
			VocalFade{{0, 3}, -48.0, 0.0, VocalFadeCurve::EqualPower}};
		std::vector<float> output(input.size());
		QCOMPARE(process({0, 3}, equalPower, 0, input, 1, output), QStringLiteral("ok"));
		QVERIFY(near(output[0], std::pow(10.0, -48.0 / 20.0)));
		QVERIFY(near(output[2], 1.0));

		const std::array<SampleFrame, 1> oneInput{SampleFrame(1.0f)};
		const std::vector<VocalEditOperation> oneFrame{
			VocalFade{{0, 1}, -12.0, -3.0, VocalFadeCurve::LinearAmplitude}};
		std::array<float, 1> oneOutput{};
		QCOMPARE(process({0, 1}, oneFrame, 0, oneInput, 1, oneOutput), QStringLiteral("ok"));
		QVERIFY(near(oneOutput[0], std::pow(10.0, -3.0 / 20.0)));
	}

	void overrangeIsReportedByMeasurementAndNeverClipped()
	{
		const std::array<SampleFrame, 2> input{SampleFrame(0.8f, -0.75f), SampleFrame(0.0f, 0.0f)};
		const std::vector<VocalEditOperation> operations{
			VocalGainEnvelope{{{0, 6.0}, {2, 6.0}}}};
		std::vector<float> output(4);
		QCOMPARE(process({0, 2}, operations, 0, input, 2, output), QStringLiteral("ok"));
		QVERIFY(output[0] > 1.0f);
		QVERIFY(output[1] < -1.0f);
		QVERIFY(std::isfinite(output[0]) && std::isfinite(output[1]));
		QCOMPARE(output[2], 0.0f);
		QCOMPARE(output[3], 0.0f);
	}

	void rejectsNonfiniteInputAndShapeMismatches()
	{
		const std::array<SampleFrame, 2> input{
			SampleFrame(std::numeric_limits<float>::quiet_NaN(), 0.0f), SampleFrame(0.0f, 0.0f)};
		const std::vector<VocalEditOperation> operations{
			VocalGainEnvelope{{{0, 0.0}, {2, 0.0}}}};
		std::vector<float> output(4);
		QCOMPARE(process({0, 2}, operations, 0, input, 2, output), QStringLiteral("invalid_audio"));
		QCOMPARE(process({0, 2}, operations, 0, input, 2, std::span<float>(output.data(), 3)),
			QStringLiteral("invalid_parameter"));
		QCOMPARE(process({0, 2}, operations, -1, input, 2, output), QStringLiteral("invalid_parameter"));
	}

	void rejectsGainAndFadeStepsAtInteriorEditBoundaries()
	{
		const std::array<SampleFrame, 8> input{SampleFrame(0.1f), SampleFrame(0.1f), SampleFrame(0.1f),
			SampleFrame(0.1f), SampleFrame(0.1f), SampleFrame(0.1f), SampleFrame(0.1f), SampleFrame(0.1f)};
		std::array<float, 8> output{};
		const std::vector<VocalEditOperation> abruptGain{
			VocalGainEnvelope{{{2, -6.0}, {6, 0.0}}}};
		QCOMPARE(process({0, 8}, abruptGain, 0, input, 1, output), QStringLiteral("invalid_parameter"));
		const std::vector<VocalEditOperation> abruptFade{
			VocalFade{{2, 6}, -6.0, 0.0, VocalFadeCurve::LinearAmplitude}};
		QCOMPARE(process({0, 8}, abruptFade, 0, input, 1, output), QStringLiteral("invalid_parameter"));
	}

	void processesAFullTwoMinuteVocalInFixedSizeStereoBlocks()
	{
		constexpr qint64 frames = 48000LL * 120;
		const VocalEditFrameRange domain{0, frames};
		const std::vector<VocalEditOperation> operations{
			VocalGainEnvelope{{{0, 0.0}, {frames, 0.0}}}};
		std::array<SampleFrame, 4096> input;
		input.fill(SampleFrame(0.125f, -0.25f));
		std::vector<float> output(input.size() * 2);
		qint64 processed = 0;
		while (processed < frames)
		{
			const auto count = size_t(std::min<qint64>(qint64(input.size()), frames - processed));
			QCOMPARE(process(domain, operations, processed,
				std::span<const SampleFrame>(input.data(), count), 2,
				std::span<float>(output.data(), count * 2)), QStringLiteral("ok"));
			QCOMPARE(output[0], 0.125f);
			QCOMPARE(output[1], -0.25f);
			QCOMPARE(output[count * 2 - 2], 0.125f);
			QCOMPARE(output[count * 2 - 1], -0.25f);
			processed += qint64(count);
		}
		QCOMPARE(processed, frames);
	}
};

QTEST_APPLESS_MAIN(AiBridgeVocalGainFadeTest)
#include "AiBridgeVocalGainFadeTest.moc"
