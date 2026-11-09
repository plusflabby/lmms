#include <QtTest>

#include "AiBridgeVocalDspBackend.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include <utility>

using namespace lmms;

class AiBridgeVocalSignalsmithBackendTest : public QObject
{
	Q_OBJECT

	static double estimateFrequency(const std::vector<SampleFrame>& samples, size_t start,
		size_t length, int sampleRate, double centerHz, size_t channel = 0)
	{
		constexpr double twoPi = 6.283185307179586476925286766559;
		double bestFrequency = centerHz;
		double bestMagnitude = -1.0;
		for (int step = -250; step <= 250; ++step)
		{
			const double frequency = centerHz + step * 0.02;
			double real = 0.0;
			double imaginary = 0.0;
			for (size_t i = 0; i < length; ++i)
			{
				const auto phase = twoPi * frequency * double(i) / sampleRate;
				real += double(samples[start + i][channel]) * std::cos(phase);
				imaginary += double(samples[start + i][channel]) * std::sin(phase);
			}
			const auto magnitude = real * real + imaginary * imaginary;
			if (magnitude > bestMagnitude)
			{
				bestMagnitude = magnitude;
				bestFrequency = frequency;
			}
		}
		return bestFrequency;
	}

private slots:
	void rendersFinitePitchPreservingBlocksAtTheDeclaredLength()
	{
		constexpr int sampleRate = 48000;
		constexpr int inputFrames = sampleRate;
		constexpr quint64 outputFrames = 56400;
		constexpr double twoPi = 6.283185307179586476925286766559;
		std::vector<SampleFrame> input(static_cast<size_t>(inputFrames));
		for (int i = 0; i < inputFrames; ++i)
		{
			const auto value = float(0.2 * std::sin(twoPi * 220.0 * double(i) / sampleRate));
			input[size_t(i)] = SampleFrame(value, value);
		}

		std::vector<SampleFrame> output;
		std::atomic_bool cancelled{false};
		quint64 nextFrame = 0;
		bool contiguous = true;
		const auto rendered = AiBridgeVocalDspBackend::renderSegment(input, outputFrames,
			sampleRate, 1, 0.80, 1.25, cancelled,
			[&](quint64 firstFrame, std::span<const SampleFrame> block) {
				contiguous = contiguous && firstFrame == nextFrame;
				output.insert(output.end(), block.begin(), block.end());
				nextFrame += quint64(block.size());
				return PcmWaveVisitDecision::Continue;
			});
		QCOMPARE(rendered.code, QStringLiteral("ok"));
		QVERIFY(contiguous);
		QCOMPARE(rendered.metrics.framesWritten, outputFrames);
		QCOMPARE(output.size(), size_t(outputFrames));
		QVERIFY(rendered.metrics.processCalls > 1);
		QVERIFY(rendered.metrics.maximumOutputBlockFrames <= AiBridgeVocalDspBackend::MaximumProcessBlockFrames);
		QVERIFY(rendered.metrics.reservedWorkspaceBytes <= 8ULL * 1024 * 1024);
		const auto frequency = estimateFrequency(output, 6000, 24000, sampleRate, 220.0);
		const auto cents = 1200.0 * std::log2(frequency / 220.0);
		QVERIFY(std::abs(cents) <= 5.0);
		QVERIFY(std::all_of(output.begin(), output.end(), [](const SampleFrame& frame) {
			return std::isfinite(frame.left()) && std::isfinite(frame.right());
		}));
		std::vector<SampleFrame> repeat;
		const auto repeated = AiBridgeVocalDspBackend::renderSegment(input, outputFrames,
			sampleRate, 1, 0.80, 1.25, cancelled,
			[&](quint64, std::span<const SampleFrame> block) {
				repeat.insert(repeat.end(), block.begin(), block.end());
				return PcmWaveVisitDecision::Continue;
			});
		QCOMPARE(repeated.code, QStringLiteral("ok"));
		QVERIFY(repeat.size() == output.size());
		bool deterministic = true;
		for (size_t i = 0; i < output.size(); ++i)
			deterministic = deterministic && output[i].left() == repeat[i].left()
				&& output[i].right() == repeat[i].right();
		QVERIFY(deterministic);
	}

	void cancellationStopsBetweenBoundedBackendBlocks()
	{
		constexpr int sampleRate = 48000;
		std::vector<SampleFrame> input(size_t(sampleRate), SampleFrame(0.125f));
		std::atomic_bool cancelled{false};
		quint64 blocks = 0;
		const auto rendered = AiBridgeVocalDspBackend::renderSegment(input, 56400,
			sampleRate, 1, 0.80, 1.25, cancelled,
			[&](quint64, std::span<const SampleFrame>) {
				++blocks;
				cancelled.store(true, std::memory_order_release);
				return PcmWaveVisitDecision::Continue;
			});
		QCOMPARE(rendered.code, QStringLiteral("cancelled"));
		QCOMPARE(blocks, quint64(1));
		QCOMPARE(rendered.metrics.processCalls, quint64(2));
		QVERIFY(rendered.metrics.maximumUninterruptibleInputFrames <= 16 * 1024);
		QVERIFY(rendered.metrics.framesWritten < 56400);
	}

	void maximumDurationStereoStreamsBoundedBlocks()
	{
		constexpr int sampleRate = 48000;
		constexpr quint64 inputFrames = quint64(sampleRate) * 120;
		constexpr quint64 outputFrames = inputFrames - 2400;
		std::vector<SampleFrame> input(static_cast<size_t>(inputFrames), SampleFrame(0.01f, -0.02f));
		std::atomic_bool cancelled{false};
		quint64 nextFrame = 0;
		bool contiguous = true;
		bool finite = true;
		const auto rendered = AiBridgeVocalDspBackend::renderSegment(input, outputFrames,
			sampleRate, 2, 0.80, 1.25, cancelled,
			[&](quint64 firstFrame, std::span<const SampleFrame> block) {
				contiguous = contiguous && firstFrame == nextFrame;
				for (const auto& frame : block)
					finite = finite && std::isfinite(frame.left()) && std::isfinite(frame.right());
				nextFrame += quint64(block.size());
				return PcmWaveVisitDecision::Continue;
			});
		QCOMPARE(rendered.code, QStringLiteral("ok"));
		QVERIFY(contiguous && finite);
		QCOMPARE(rendered.metrics.framesWritten, outputFrames);
		QVERIFY(rendered.metrics.processCalls > 1000);
		QVERIFY(rendered.metrics.maximumOutputBlockFrames <= AiBridgeVocalDspBackend::MaximumProcessBlockFrames);
		QCOMPARE(rendered.metrics.reservedWorkspaceBytes,
			AiBridgeVocalDspBackend::FixedWorkspaceChargeBytes);
	}

	void timingMapCrossfadesBeforeAnchorsAndKeepsFrameTargets()
	{
		constexpr int sampleRate = 48000;
		std::vector<SampleFrame> input(static_cast<size_t>(sampleRate));
		for (size_t i = 0; i < input.size(); ++i) input[i] = SampleFrame(0.0f);
		input[24000] = SampleFrame(0.7f);
		const std::array<VocalDspTimingAnchor, 3> anchors{{
			{0, 0, 0, VocalCrossfadeLaw::LinearAmplitude, false},
			{24000, 26400, 840, VocalCrossfadeLaw::LinearAmplitude, false},
			{48000, 56400, 0, VocalCrossfadeLaw::LinearAmplitude, false}}};
		std::vector<SampleFrame> output;
		std::atomic_bool cancelled{false};
		quint64 nextFrame = 0;
		bool contiguous = true;
		const auto rendered = AiBridgeVocalDspBackend::renderTimingMap(input, 0, anchors, {},
			sampleRate, 1, 0.80, 1.25, 12000, cancelled,
			[&](quint64 firstFrame, std::span<const SampleFrame> block) {
				contiguous = contiguous && firstFrame == nextFrame;
				output.insert(output.end(), block.begin(), block.end());
				nextFrame += quint64(block.size());
				return PcmWaveVisitDecision::Continue;
			});
		QCOMPARE(rendered.code, QStringLiteral("ok"));
		QVERIFY(contiguous);
		QCOMPARE(output.size(), size_t(56400));
		QCOMPARE(rendered.metrics.framesWritten, quint64(56400));
		const auto peak = std::max_element(output.begin(), output.end(), [](const auto& a, const auto& b) {
			return std::abs(a.left()) < std::abs(b.left());
		});
		const auto peakFrame = qint64(std::distance(output.begin(), peak));
		QVERIFY(std::abs(peakFrame - 26400) <= 1);
	}

	void multipleAnchorsRemainWithinOneFrameAcrossLocalRatioChanges()
	{
		constexpr int sampleRate = 48000;
		const std::array<VocalDspTimingAnchor, 4> anchors{{
			{0, 0, 0, VocalCrossfadeLaw::LinearAmplitude, false},
			{48000, 52800, 840, VocalCrossfadeLaw::LinearAmplitude, false},
			{120000, 112800, 840, VocalCrossfadeLaw::LinearAmplitude, false},
			{192000, 198000, 0, VocalCrossfadeLaw::LinearAmplitude, false}}};
		for (const auto [sourceAnchor, targetOutput] :
			{std::pair<qint64, qint64>{48000, 52800}, {120000, 112800}})
		{
			std::vector<SampleFrame> input(192000, SampleFrame(0.0f));
			input[size_t(sourceAnchor)] = SampleFrame(0.7f);
			std::vector<SampleFrame> output;
			std::atomic_bool cancelled{false};
			const auto rendered = AiBridgeVocalDspBackend::renderTimingMap(input, 0, anchors, {},
				sampleRate, 1, 0.80, 1.25, 12000, cancelled,
				[&](quint64, std::span<const SampleFrame> block) {
					output.insert(output.end(), block.begin(), block.end());
					return PcmWaveVisitDecision::Continue;
				});
			QCOMPARE(rendered.code, QStringLiteral("ok"));
			QCOMPARE(output.size(), size_t(198000));
			const auto peak = std::max_element(output.begin(), output.end(), [](const auto& a, const auto& b) {
				return std::abs(a.left()) < std::abs(b.left());
			});
			const auto peakFrame = qint64(std::distance(output.begin(), peak));
			QVERIFY(std::abs(peakFrame - targetOutput) <= 1);
		}
	}

	void multipleAnchorRatiosPreservePitchAwayFromDeclaredJoins()
	{
		constexpr int sampleRate = 48000;
		constexpr double twoPi = 6.283185307179586476925286766559;
		std::vector<SampleFrame> input(192000);
		for (size_t i = 0; i < input.size(); ++i)
		{
			const auto value = float(0.2 * std::sin(twoPi * 220.0 * double(i) / sampleRate));
			input[i] = SampleFrame(value, value);
		}
		const std::array<VocalDspTimingAnchor, 4> anchors{{
			{0, 0, 0, VocalCrossfadeLaw::LinearAmplitude, false},
			{48000, 52800, 840, VocalCrossfadeLaw::LinearAmplitude, false},
			{120000, 112800, 840, VocalCrossfadeLaw::LinearAmplitude, false},
			{192000, 198000, 0, VocalCrossfadeLaw::LinearAmplitude, false}}};
		std::vector<SampleFrame> output;
		std::atomic_bool cancelled{false};
		const auto rendered = AiBridgeVocalDspBackend::renderTimingMap(input, 0, anchors, {},
			sampleRate, 1, 0.80, 1.25, 12000, cancelled,
			[&](quint64, std::span<const SampleFrame> block) {
				output.insert(output.end(), block.begin(), block.end());
				return PcmWaveVisitDecision::Continue;
			});
		QCOMPARE(rendered.code, QStringLiteral("ok"));
		QCOMPARE(output.size(), size_t(198000));
		for (const auto [start, count] : {std::pair<size_t, size_t>{6000, 18000},
			std::pair<size_t, size_t>{65000, 20000}, std::pair<size_t, size_t>{140000, 20000}})
		{
			const auto frequency = estimateFrequency(output, start, count, sampleRate, 220.0);
			const auto cents = 1200.0 * std::log2(frequency / 220.0);
			QVERIFY(std::abs(cents) <= 5.0);
		}
	}

	void supportsBothProposedRatioEndpoints()
	{
		constexpr int sampleRate = 48000;
		std::vector<SampleFrame> input(48000, SampleFrame(0.0f));
		input[24000] = SampleFrame(0.7f);
		const std::array<VocalDspTimingAnchor, 3> anchors{{
			{0, 0, 0, VocalCrossfadeLaw::LinearAmplitude, false},
			{24000, 19200, 840, VocalCrossfadeLaw::LinearAmplitude, false},
			{48000, 49200, 0, VocalCrossfadeLaw::LinearAmplitude, false}}};
		std::vector<SampleFrame> output;
		std::atomic_bool cancelled{false};
		const auto rendered = AiBridgeVocalDspBackend::renderTimingMap(input, 0, anchors, {},
			sampleRate, 1, 0.80, 1.25, 12000, cancelled,
			[&](quint64, std::span<const SampleFrame> block) {
				output.insert(output.end(), block.begin(), block.end());
				return PcmWaveVisitDecision::Continue;
			});
		QCOMPARE(rendered.code, QStringLiteral("ok"));
		QCOMPARE(output.size(), size_t(49200));
		const auto peak = std::max_element(output.begin(), output.end(), [](const auto& a, const auto& b) {
			return std::abs(a.left()) < std::abs(b.left());
		});
		const auto peakFrame = qint64(std::distance(output.begin(), peak));
		QVERIFY(std::abs(peakFrame - 19200) <= 1);
	}

	void preservesPitchAt44100HzAndInBothStereoChannels()
	{
		constexpr double twoPi = 6.283185307179586476925286766559;
		std::vector<SampleFrame> mono(44100);
		for (size_t i = 0; i < mono.size(); ++i)
		{
			const auto value = float(0.2 * std::sin(twoPi * 330.0 * double(i) / 44100.0));
			mono[i] = SampleFrame(value, value);
		}
		std::vector<SampleFrame> monoOutput;
		std::atomic_bool cancelled{false};
		const auto monoResult = AiBridgeVocalDspBackend::renderSegment(mono, 52920,
			44100, 1, 0.80, 1.25, cancelled,
			[&](quint64, std::span<const SampleFrame> block) {
				monoOutput.insert(monoOutput.end(), block.begin(), block.end());
				return PcmWaveVisitDecision::Continue;
			});
		QCOMPARE(monoResult.code, QStringLiteral("ok"));
		QCOMPARE(monoOutput.size(), size_t(52920));
		const auto monoCents = 1200.0 * std::log2(
			estimateFrequency(monoOutput, 6000, 24000, 44100, 330.0) / 330.0);
		QVERIFY(std::abs(monoCents) <= 5.0);

		std::vector<SampleFrame> stereo(48000);
		for (size_t i = 0; i < stereo.size(); ++i)
		{
			stereo[i] = SampleFrame(float(0.2 * std::sin(twoPi * 220.0 * double(i) / 48000.0)),
				float(0.15 * std::sin(twoPi * 330.0 * double(i) / 48000.0)));
		}
		std::vector<SampleFrame> stereoOutput;
		const auto stereoResult = AiBridgeVocalDspBackend::renderSegment(stereo, 56400,
			48000, 2, 0.80, 1.25, cancelled,
			[&](quint64, std::span<const SampleFrame> block) {
				stereoOutput.insert(stereoOutput.end(), block.begin(), block.end());
				return PcmWaveVisitDecision::Continue;
			});
		QCOMPARE(stereoResult.code, QStringLiteral("ok"));
		QCOMPARE(stereoOutput.size(), size_t(56400));
		const auto leftCents = 1200.0 * std::log2(
			estimateFrequency(stereoOutput, 6000, 24000, 48000, 220.0, 0) / 220.0);
		const auto rightCents = 1200.0 * std::log2(
			estimateFrequency(stereoOutput, 6000, 24000, 48000, 330.0, 1) / 330.0);
		QVERIFY(std::abs(leftCents) <= 5.0);
		QVERIFY(std::abs(rightCents) <= 5.0);
	}

	void protectedConsonantRangeIsCopiedWithoutTimeStretching()
	{
		constexpr int sampleRate = 48000;
		std::vector<SampleFrame> input(static_cast<size_t>(sampleRate));
		for (size_t i = 0; i < input.size(); ++i)
		{
			const auto value = float(i % 257) / 257.0f;
			input[i] = SampleFrame(value, -value);
		}
		const std::array<VocalDspTimingAnchor, 4> anchors{{
			{0, 0, 0, VocalCrossfadeLaw::LinearAmplitude, false},
			{12000, 14400, 0, VocalCrossfadeLaw::LinearAmplitude, true},
			{16000, 18400, 0, VocalCrossfadeLaw::LinearAmplitude, false},
			{48000, 54000, 0, VocalCrossfadeLaw::LinearAmplitude, false}}};
		const std::array<VocalEditFrameRange, 1> protectedRanges{{{12000, 16000}}};
		std::vector<SampleFrame> output;
		std::atomic_bool cancelled{false};
		const auto rendered = AiBridgeVocalDspBackend::renderTimingMap(input, 0, anchors, protectedRanges,
			sampleRate, 2, 0.80, 1.25, 12000, cancelled,
			[&](quint64, std::span<const SampleFrame> block) {
				output.insert(output.end(), block.begin(), block.end());
				return PcmWaveVisitDecision::Continue;
			});
		QCOMPARE(rendered.code, QStringLiteral("ok"));
		QCOMPARE(output.size(), size_t(54000));
		bool exact = true;
		for (size_t i = 0; i < 4000; ++i)
			exact = exact && output[14400 + i].left() == input[12000 + i].left()
				&& output[14400 + i].right() == input[12000 + i].right();
		QVERIFY(exact);
	}

	void rejectsUnsafeRatiosAndInsufficientSegmentContext()
	{
		std::vector<SampleFrame> input(48000, SampleFrame(0.1f));
		std::atomic_bool cancelled{false};
		const PcmWaveChunkVisitor discard = [](quint64, std::span<const SampleFrame>) {
			return PcmWaveVisitDecision::Continue;
		};
		const auto ratio = AiBridgeVocalDspBackend::renderSegment(input, 24000,
			48000, 1, 0.80, 1.25, cancelled, discard);
		QCOMPARE(ratio.code, QStringLiteral("unsupported_stretch_ratio"));

		std::vector<SampleFrame> shortInput(128, SampleFrame(0.1f));
		const auto shortSegment = AiBridgeVocalDspBackend::renderSegment(shortInput, 129,
			48000, 1, 0.80, 1.25, cancelled, discard);
		QCOMPARE(shortSegment.code, QStringLiteral("stretch_segment_too_short"));
	}
};

QTEST_GUILESS_MAIN(AiBridgeVocalSignalsmithBackendTest)
#include "AiBridgeVocalSignalsmithBackendTest.moc"
