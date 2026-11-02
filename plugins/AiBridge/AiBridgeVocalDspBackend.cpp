#include "AiBridgeVocalDspBackend.h"
#include "AiBridgeVocalComp.h"
#include "AiBridgeVocalTypes.h"

#include "signalsmith-stretch.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <new>
#include <mutex>
#include <optional>
#include <vector>

namespace lmms {
namespace {

constexpr quint64 maximumInputSeconds = 120;
using SignalsmithStretchVersionProbe = signalsmith::stretch::SignalsmithStretch<float>;
static_assert(SignalsmithStretchVersionProbe::version[0] == 1
	&& SignalsmithStretchVersionProbe::version[1] == 3
	&& SignalsmithStretchVersionProbe::version[2] == 2);
struct LatencyInfo
{
	int input = 0;
	int output = 0;
};
std::mutex latencyMutex;
std::array<std::optional<LatencyInfo>, 4> latencyCache;

struct FrameChannelView
{
	std::span<const SampleFrame> frames;
	size_t channel = 0;
	float operator[](int index) const noexcept
	{
		return channel == 0 ? frames[size_t(index)].left() : frames[size_t(index)].right();
	}
};

struct FrameChannelsView
{
	std::span<const SampleFrame> frames;
	FrameChannelView operator[](int channel) const noexcept
	{
		return {frames, size_t(channel)};
	}
};

struct ZeroChannel
{
	float operator[](int) const noexcept { return 0.0f; }
};

struct ZeroChannels
{
	ZeroChannel operator[](int) const noexcept { return {}; }
};

bool supportedRate(int sampleRate)
{
	return sampleRate == 44100 || sampleRate == 48000;
}

VocalDspRenderResult failure(const QString& code, VocalDspMetrics metrics = {})
{
	return {code, metrics};
}

} // namespace

quint64 AiBridgeVocalDspBackend::reservedWorkspaceBytes(int sampleRate, int channels)
{
	if (!supportedRate(sampleRate) || (channels != 1 && channels != 2)) return 0;
	// The vendored 1.3.2 preset has fixed sample-rate/channel dimensions. The 8 MiB
	// reservation covers the measured <= 1.1 MiB stretcher workspace plus planar,
	// frame, and latency-tail scratch with allocator margin.
	return FixedWorkspaceChargeBytes;
}

VocalDspRenderResult AiBridgeVocalDspBackend::renderSegment(
	std::span<const SampleFrame> input,
	quint64 outputFrames,
	int sampleRate,
	int channels,
	double minimumOutputPerInputRatio,
	double maximumOutputPerInputRatio,
	const std::atomic_bool& cancelled,
	const PcmWaveChunkVisitor& visitor)
{
	VocalDspMetrics metrics;
	if (!supportedRate(sampleRate)) return failure(QStringLiteral("unsupported_sample_rate"));
	if ((channels != 1 && channels != 2) || input.empty() || !outputFrames || !visitor
		|| !std::isfinite(minimumOutputPerInputRatio) || !std::isfinite(maximumOutputPerInputRatio)
		|| minimumOutputPerInputRatio <= 0.0 || minimumOutputPerInputRatio > maximumOutputPerInputRatio)
	{
		return failure(QStringLiteral("invalid_parameter"));
	}
	const auto maximumFrames = quint64(sampleRate) * maximumInputSeconds;
	if (input.size() > maximumFrames || outputFrames > maximumFrames
		|| input.size() > size_t(std::numeric_limits<int>::max())
		|| outputFrames > quint64(std::numeric_limits<int>::max()))
	{
		return failure(QStringLiteral("resource_limit"));
	}
	const auto outputPerInput = double(outputFrames) / double(input.size());
	if (!std::isfinite(outputPerInput) || outputPerInput < minimumOutputPerInputRatio
		|| outputPerInput > maximumOutputPerInputRatio)
	{
		return failure(QStringLiteral("unsupported_stretch_ratio"));
	}
	const auto minimumInput = minimumInputFrames(sampleRate, channels, outputPerInput);
	if (minimumInput < 0) return failure(QStringLiteral("unsupported_sample_rate"));
	if (input.size() != size_t(outputFrames) && input.size() < size_t(minimumInput))
		return failure(QStringLiteral("stretch_segment_too_short"));
	metrics.reservedWorkspaceBytes = reservedWorkspaceBytes(sampleRate, channels);
	if (!metrics.reservedWorkspaceBytes) return failure(QStringLiteral("invalid_parameter"));
	if (cancelled.load(std::memory_order_acquire)) return failure(QStringLiteral("cancelled"), metrics);
	for (const auto& frame : input)
	{
		if (!std::isfinite(frame.left()) || (channels == 2 && !std::isfinite(frame.right())))
		{
			return failure(QStringLiteral("invalid_audio"), metrics);
		}
	}

	if (outputFrames == quint64(input.size()))
	{
		for (quint64 first = 0; first < outputFrames;)
		{
			if (cancelled.load(std::memory_order_acquire)) return failure(QStringLiteral("cancelled"), metrics);
			const auto count = size_t(std::min<quint64>(MaximumProcessBlockFrames, outputFrames - first));
			metrics.maximumOutputBlockFrames = std::max(metrics.maximumOutputBlockFrames, quint64(count));
			const auto decision = visitor(first, input.subspan(size_t(first), count));
			if (decision != PcmWaveVisitDecision::Continue)
			{
				return failure(decision == PcmWaveVisitDecision::Cancelled
					? QStringLiteral("cancelled") : QStringLiteral("resource_limit"), metrics);
			}
			metrics.framesWritten += quint64(count);
			first += quint64(count);
		}
		return {QStringLiteral("ok"), metrics};
	}

	try
	{
		using Stretch = signalsmith::stretch::SignalsmithStretch<float>;
		Stretch stretcher(0x534c4d4d);
		stretcher.presetDefault(channels, float(sampleRate), false);
		stretcher.setTransposeFactor(1.0f);
		std::array<std::vector<float>, 2> outputPlanes;
		for (int channel = 0; channel < channels; ++channel)
		{
			outputPlanes[size_t(channel)].resize(size_t(MaximumProcessBlockFrames));
		}
		std::vector<SampleFrame> frameBlock(static_cast<size_t>(MaximumProcessBlockFrames));
		std::array<std::vector<float>, 2> tailPlanes;
		for (int channel = 0; channel < channels; ++channel)
		{
			tailPlanes[size_t(channel)].resize(size_t(stretcher.intervalSamples()));
		}

		const double playbackRate = double(input.size()) / double(outputFrames);
		const int inputFrames = int(input.size());
		const int outputCount = int(outputFrames);
		const int seekLength = stretcher.outputSeekLength(playbackRate);
		if (inputFrames < seekLength) return failure(QStringLiteral("stretch_segment_too_short"), metrics);
		const FrameChannelsView source{input};
		const auto preRollStart = std::chrono::steady_clock::now();
		stretcher.outputSeek(source, seekLength);
		const auto preRollEnd = std::chrono::steady_clock::now();
		metrics.processCalls = 1;
		metrics.maximumProcessCallMicroseconds = quint64(
			std::chrono::duration_cast<std::chrono::microseconds>(preRollEnd - preRollStart).count());
		metrics.maximumUninterruptibleInputFrames = quint64(seekLength);
		const int processOutputFrames = outputCount - int(double(seekLength) / playbackRate);
		const int processInputFrames = inputFrames - seekLength;
		if (processOutputFrames <= 0 || processInputFrames < 0)
		{
			return failure(QStringLiteral("stretch_segment_too_short"), metrics);
		}

		auto emitPlanar = [&](quint64 firstOutputFrame, size_t count,
			const std::array<std::vector<float>, 2>& planes) -> QString {
			if (cancelled.load(std::memory_order_acquire)) return QStringLiteral("cancelled");
			for (size_t i = 0; i < count; ++i)
			{
				const auto left = planes[0][i];
				const auto right = channels == 2 ? planes[1][i] : left;
				if (!std::isfinite(left) || !std::isfinite(right)) return QStringLiteral("invalid_audio");
				frameBlock[i] = SampleFrame(left, right);
			}
			const auto decision = visitor(firstOutputFrame,
				std::span<const SampleFrame>(frameBlock.data(), count));
			if (decision != PcmWaveVisitDecision::Continue)
			{
				return decision == PcmWaveVisitDecision::Cancelled
					? QStringLiteral("cancelled") : QStringLiteral("resource_limit");
			}
			metrics.framesWritten += quint64(count);
			metrics.maximumOutputBlockFrames = std::max(metrics.maximumOutputBlockFrames, quint64(count));
			return QStringLiteral("ok");
		};

		int inputDone = 0;
		int outputDone = 0;
		while (outputDone < processOutputFrames)
		{
			if (cancelled.load(std::memory_order_acquire)) return failure(QStringLiteral("cancelled"), metrics);
			const int outputBlockFrames = std::min<int>(int(MaximumProcessBlockFrames),
				processOutputFrames - outputDone);
			int targetInputFrames = int((qint64(outputDone + outputBlockFrames) * processInputFrames
				+ processOutputFrames / 2) / processOutputFrames);
			if (outputDone + outputBlockFrames == processOutputFrames) targetInputFrames = processInputFrames;
			targetInputFrames = std::min(targetInputFrames, processInputFrames);
			const int inputBlockFrames = targetInputFrames - inputDone;
			if (inputBlockFrames <= 0) return failure(QStringLiteral("stretch_backend_failed"), metrics);
			const FrameChannelsView blockInput{input.subspan(size_t(seekLength + inputDone), size_t(inputBlockFrames))};
			std::array<float*, 2> blockOutput{};
			for (int channel = 0; channel < channels; ++channel)
			{
				blockOutput[size_t(channel)] = outputPlanes[size_t(channel)].data();
			}
			const auto before = std::chrono::steady_clock::now();
			stretcher.process(blockInput, inputBlockFrames, blockOutput.data(), outputBlockFrames);
			const auto after = std::chrono::steady_clock::now();
			metrics.maximumProcessCallMicroseconds = std::max(metrics.maximumProcessCallMicroseconds,
				quint64(std::chrono::duration_cast<std::chrono::microseconds>(after - before).count()));
			++metrics.processCalls;
			const auto emitted = emitPlanar(quint64(outputDone), size_t(outputBlockFrames), outputPlanes);
			if (emitted != QStringLiteral("ok")) return failure(emitted, metrics);
			inputDone += inputBlockFrames;
			outputDone += outputBlockFrames;
		}
		if (inputDone != processInputFrames) return failure(QStringLiteral("stretch_backend_failed"), metrics);
		if (cancelled.load(std::memory_order_acquire)) return failure(QStringLiteral("cancelled"), metrics);

		const int tailFrames = outputCount - processOutputFrames;
		const int boundedTailOutput = std::max(0, tailFrames - stretcher.intervalSamples());
		if (boundedTailOutput)
		{
			const int zeroInputFrames = int(double(boundedTailOutput) * playbackRate);
			int zeroInputDone = 0;
			int zeroOutputDone = 0;
			const ZeroChannels zeroInput;
			while (zeroOutputDone < boundedTailOutput)
			{
				if (cancelled.load(std::memory_order_acquire)) return failure(QStringLiteral("cancelled"), metrics);
				const int outputBlockFrames = std::min<int>(int(MaximumProcessBlockFrames),
					boundedTailOutput - zeroOutputDone);
				int targetInputFrames = int((qint64(zeroOutputDone + outputBlockFrames) * zeroInputFrames
					+ boundedTailOutput / 2) / boundedTailOutput);
				if (zeroOutputDone + outputBlockFrames == boundedTailOutput) targetInputFrames = zeroInputFrames;
				targetInputFrames = std::min(targetInputFrames, zeroInputFrames);
				const int inputBlockFrames = targetInputFrames - zeroInputDone;
				if (inputBlockFrames <= 0) return failure(QStringLiteral("stretch_backend_failed"), metrics);
				std::array<float*, 2> blockOutput{};
				for (int channel = 0; channel < channels; ++channel)
				{
					blockOutput[size_t(channel)] = outputPlanes[size_t(channel)].data();
				}
				const auto before = std::chrono::steady_clock::now();
				stretcher.process(zeroInput, inputBlockFrames, blockOutput.data(), outputBlockFrames);
				const auto after = std::chrono::steady_clock::now();
				metrics.maximumProcessCallMicroseconds = std::max(metrics.maximumProcessCallMicroseconds,
					quint64(std::chrono::duration_cast<std::chrono::microseconds>(after - before).count()));
				++metrics.processCalls;
				const auto emitted = emitPlanar(quint64(processOutputFrames + zeroOutputDone),
					size_t(outputBlockFrames), outputPlanes);
				if (emitted != QStringLiteral("ok")) return failure(emitted, metrics);
				zeroInputDone += inputBlockFrames;
				zeroOutputDone += outputBlockFrames;
			}
			if (zeroInputDone != zeroInputFrames) return failure(QStringLiteral("stretch_backend_failed"), metrics);
		}
		if (cancelled.load(std::memory_order_acquire)) return failure(QStringLiteral("cancelled"), metrics);
		const int finalTailFrames = tailFrames - boundedTailOutput;
		if (finalTailFrames < 0 || finalTailFrames > stretcher.intervalSamples())
			return failure(QStringLiteral("stretch_backend_failed"), metrics);
		if (finalTailFrames)
		{
			std::array<float*, 2> tailOutput{};
			for (int channel = 0; channel < channels; ++channel)
			{
				tailOutput[size_t(channel)] = tailPlanes[size_t(channel)].data();
			}
			stretcher.flush(tailOutput.data(), finalTailFrames, float(playbackRate));
			for (int i = 0; i < finalTailFrames; ++i)
			{
				const auto left = tailPlanes[0][size_t(i)];
				const auto right = channels == 2 ? tailPlanes[1][size_t(i)] : left;
				if (!std::isfinite(left) || !std::isfinite(right))
					return failure(QStringLiteral("invalid_audio"), metrics);
				frameBlock[size_t(i)] = SampleFrame(left, right);
			}
			const auto decision = visitor(quint64(processOutputFrames + boundedTailOutput),
				std::span<const SampleFrame>(frameBlock.data(), size_t(finalTailFrames)));
			if (decision != PcmWaveVisitDecision::Continue)
			{
				return failure(decision == PcmWaveVisitDecision::Cancelled
					? QStringLiteral("cancelled") : QStringLiteral("resource_limit"), metrics);
			}
			metrics.framesWritten += quint64(finalTailFrames);
			metrics.maximumOutputBlockFrames = std::max(metrics.maximumOutputBlockFrames, quint64(finalTailFrames));
		}
		if (metrics.framesWritten != outputFrames) return failure(QStringLiteral("asset_integrity_failed"), metrics);
		return {QStringLiteral("ok"), metrics};
	}
	catch (const std::bad_alloc&)
	{
		return failure(QStringLiteral("resource_limit"), metrics);
	}
	catch (...)
	{
		return failure(QStringLiteral("stretch_backend_failed"), metrics);
	}
}

int AiBridgeVocalDspBackend::minimumInputFrames(int sampleRate, int channels, double outputPerInputRatio)
{
	if (!supportedRate(sampleRate) || (channels != 1 && channels != 2)
		|| !std::isfinite(outputPerInputRatio) || outputPerInputRatio <= 0.0)
	{
		return -1;
	}
	const auto index = size_t(sampleRate == 44100 ? 0 : 2) + size_t(channels - 1);
	std::optional<LatencyInfo> cached;
	{
		std::lock_guard lock(latencyMutex);
		cached = latencyCache[index];
	}
	if (!cached)
	{
		try
		{
			using Stretch = signalsmith::stretch::SignalsmithStretch<float>;
			Stretch stretcher(0x534c4d4d);
			stretcher.presetDefault(channels, float(sampleRate), false);
			LatencyInfo measured{stretcher.inputLatency(), stretcher.outputLatency()};
			if (measured.input < 0 || measured.output < 0) return -1;
			std::lock_guard lock(latencyMutex);
			if (!latencyCache[index]) latencyCache[index] = measured;
			cached = latencyCache[index];
		}
		catch (...)
		{
			return -1;
		}
	}
	const auto playbackRate = float(1.0 / outputPerInputRatio);
	const auto seekFrames = float(cached->input) + playbackRate * float(cached->output);
	if (!std::isfinite(seekFrames) || seekFrames < 0.0f
		|| seekFrames > float(std::numeric_limits<int>::max()))
	{
		return -1;
	}
	return int(seekFrames);
}

VocalDspRenderResult AiBridgeVocalDspBackend::renderTimingMap(
	std::span<const SampleFrame> input,
	qint64 inputFirstFrame,
	std::span<const VocalDspTimingAnchor> anchors,
	std::span<const VocalEditFrameRange> protectedRanges,
	int sampleRate,
	int channels,
	double minimumOutputPerInputRatio,
	double maximumOutputPerInputRatio,
	qint64 maximumCrossfadeFrames,
	const std::atomic_bool& cancelled,
	const PcmWaveChunkVisitor& visitor)
{
	VocalDspMetrics metrics;
	const auto fixedWorkspaceBytes = reservedWorkspaceBytes(sampleRate, channels);
	if (!fixedWorkspaceBytes || inputFirstFrame < 0 || input.empty() || !visitor
		|| anchors.size() < 2 || anchors.size() > 256 || protectedRanges.size() > 256
		|| maximumCrossfadeFrames < 0
		|| !std::isfinite(minimumOutputPerInputRatio) || !std::isfinite(maximumOutputPerInputRatio)
		|| minimumOutputPerInputRatio <= 0.0 || minimumOutputPerInputRatio > maximumOutputPerInputRatio)
	{
		return failure(QStringLiteral("invalid_parameter"));
	}
	if (input.size() > size_t(std::numeric_limits<qint64>::max())
		|| inputFirstFrame > std::numeric_limits<qint64>::max() - qint64(input.size()))
	{
		return failure(QStringLiteral("resource_limit"));
	}
	const auto inputEndFrame = inputFirstFrame + qint64(input.size());
	const auto maximumFrames = qint64(sampleRate) * qint64(maximumInputSeconds);
	if (quint64(input.size()) > quint64(maximumFrames) || anchors.front().inputFrame != inputFirstFrame
		|| anchors.front().outputFrame != 0 || anchors.front().preAnchorCrossfadeFrames != 0
		|| anchors.back().inputFrame != inputEndFrame || anchors.back().preAnchorCrossfadeFrames != 0
		|| anchors.back().outputFrame <= 0 || anchors.back().outputFrame > maximumFrames)
	{
		return failure(QStringLiteral("invalid_parameter"));
	}
	if (cancelled.load(std::memory_order_acquire)) return failure(QStringLiteral("cancelled"));
	for (const auto& frame : input)
	{
		if (!std::isfinite(frame.left()) || (channels == 2 && !std::isfinite(frame.right())))
		{
			return failure(QStringLiteral("invalid_audio"));
		}
	}

	struct SegmentPlan
	{
		qint64 inputStart = 0;
		qint64 inputEnd = 0;
		qint64 outputStart = 0;
		qint64 outputEnd = 0;
		qint64 outputFrames = 0;
		bool copyProtected = false;
	};
	std::vector<SegmentPlan> segments;
	try { segments.resize(anchors.size() - 1); }
	catch (const std::bad_alloc&) { return failure(QStringLiteral("resource_limit")); }
	quint64 totalSegmentFrames = 0;
	for (size_t i = 0; i + 1 < anchors.size(); ++i)
	{
		const auto& left = anchors[i];
		const auto& right = anchors[i + 1];
		if (left.inputFrame < inputFirstFrame || right.inputFrame > inputEndFrame
			|| right.inputFrame <= left.inputFrame || right.outputFrame <= left.outputFrame
			|| left.preAnchorCrossfadeFrames < 0
			|| left.preAnchorCrossfadeFrames > maximumCrossfadeFrames
			|| (left.crossfadeLaw != VocalCrossfadeLaw::LinearAmplitude
				&& left.crossfadeLaw != VocalCrossfadeLaw::EqualPower))
		{
			return failure(left.preAnchorCrossfadeFrames > maximumCrossfadeFrames
				? QStringLiteral("resource_limit") : QStringLiteral("invalid_parameter"));
		}
		const auto inputDelta = right.inputFrame - left.inputFrame;
		const auto outputDelta = right.outputFrame - left.outputFrame;
		const auto baseRatio = double(outputDelta) / double(inputDelta);
		if (baseRatio < minimumOutputPerInputRatio || baseRatio > maximumOutputPerInputRatio)
			return failure(QStringLiteral("unsupported_stretch_ratio"));
		if (left.preAnchorCrossfadeFrames > outputDelta / 2
			|| (i && left.preAnchorCrossfadeFrames >
				(left.outputFrame - anchors[i - 1].outputFrame) / 2))
		{
			return failure(QStringLiteral("invalid_parameter"));
		}
		if (left.copyProtectedInterval && inputDelta != outputDelta)
			return failure(QStringLiteral("protected_range_not_preserved"));
		if (left.copyProtectedInterval && right.preAnchorCrossfadeFrames != 0)
			return failure(QStringLiteral("protected_range_not_preserved"));

		qint64 inputStart = left.inputFrame;
		qint64 outputStart = left.outputFrame - left.preAnchorCrossfadeFrames;
		if (left.preAnchorCrossfadeFrames)
		{
			const auto inputOverlap = qint64(std::llround(
				double(left.preAnchorCrossfadeFrames) / baseRatio));
			if (inputOverlap <= 0 || inputOverlap > inputDelta / 2 || inputOverlap > left.inputFrame - inputFirstFrame)
				return failure(QStringLiteral("invalid_parameter"));
			inputStart -= inputOverlap;
		}
		const auto outputLength = right.outputFrame - outputStart;
		const auto inputLength = right.inputFrame - inputStart;
		if (outputStart < 0 || inputLength <= 0 || outputLength <= 0)
			return failure(QStringLiteral("invalid_parameter"));
		const auto realizedRatio = double(outputLength) / double(inputLength);
		if (realizedRatio < minimumOutputPerInputRatio || realizedRatio > maximumOutputPerInputRatio)
			return failure(QStringLiteral("unsupported_stretch_ratio"));
		if (!left.copyProtectedInterval && inputLength != outputLength)
		{
			const auto requiredInput = minimumInputFrames(sampleRate, channels, realizedRatio);
			if (requiredInput < 0) return failure(QStringLiteral("unsupported_sample_rate"));
			if (inputLength < requiredInput) return failure(QStringLiteral("stretch_segment_too_short"));
		}
		quint64 nextTotal = totalSegmentFrames;
		if (quint64(outputLength) > std::numeric_limits<quint64>::max() - nextTotal)
			return failure(QStringLiteral("resource_limit"));
		nextTotal += quint64(outputLength);
		segments[i] = {inputStart, right.inputFrame, outputStart, right.outputFrame, outputLength,
			left.copyProtectedInterval};
		totalSegmentFrames = nextTotal;
	}

	qint64 previousRangeEnd = inputFirstFrame;
	for (const auto& range : protectedRanges)
	{
		if (range.inFrame < inputFirstFrame || range.outFrame <= range.inFrame
			|| range.outFrame > inputEndFrame || range.inFrame < previousRangeEnd)
		{
			return failure(QStringLiteral("invalid_parameter"));
		}
		previousRangeEnd = range.outFrame;
		qint64 cursor = range.inFrame;
		for (size_t i = 0; i + 1 < anchors.size() && cursor < range.outFrame; ++i)
		{
			const auto& left = anchors[i];
			const auto& right = anchors[i + 1];
			if (right.inputFrame <= cursor || left.inputFrame >= range.outFrame) continue;
			if (left.inputFrame != cursor || !left.copyProtectedInterval
				|| right.inputFrame > range.outFrame || right.outputFrame - left.outputFrame != right.inputFrame - left.inputFrame)
			{
				return failure(QStringLiteral("protected_range_not_preserved"));
			}
			cursor = right.inputFrame;
		}
		if (cursor != range.outFrame) return failure(QStringLiteral("protected_range_not_preserved"));
	}
	for (size_t i = 0; i + 1 < anchors.size(); ++i)
	{
		const auto& left = anchors[i];
		const auto& right = anchors[i + 1];
		bool hasProtection = false;
		for (const auto& range : protectedRanges)
		{
			if (left.inputFrame < range.outFrame && right.inputFrame > range.inFrame)
			{
				hasProtection = true;
				break;
			}
		}
		if (hasProtection != left.copyProtectedInterval)
			return failure(QStringLiteral("protected_range_not_preserved"));
	}

	quint64 segmentBytes = 0;
	if (totalSegmentFrames > std::numeric_limits<quint64>::max() / sizeof(SampleFrame))
		return failure(QStringLiteral("resource_limit"));
	segmentBytes = totalSegmentFrames * sizeof(SampleFrame);
	if (segmentBytes > VocalLimits::jobBytes - FixedWorkspaceChargeBytes)
		return failure(QStringLiteral("resource_limit"));
	metrics.reservedWorkspaceBytes = FixedWorkspaceChargeBytes + segmentBytes;
	std::vector<std::vector<SampleFrame>> renderedSegments;
	std::vector<VocalCompSourceView> sourceViews;
	VocalCompRecipe composition;
	std::vector<SampleFrame> outputBlock;
	try
	{
		renderedSegments.resize(segments.size());
		sourceViews.reserve(segments.size());
		composition.segments.reserve(segments.size());
		composition.boundaries.reserve(segments.size() - 1);
		for (size_t i = 0; i < segments.size(); ++i)
			renderedSegments[i].resize(size_t(segments[i].outputFrames));
		outputBlock.resize(size_t(MaximumProcessBlockFrames));
	}
	catch (const std::bad_alloc&)
	{
		return failure(QStringLiteral("resource_limit"), metrics);
	}

	for (size_t i = 0; i < segments.size(); ++i)
	{
		if (cancelled.load(std::memory_order_acquire)) return failure(QStringLiteral("cancelled"), metrics);
		const auto& segment = segments[i];
		const auto inputOffset = size_t(segment.inputStart - inputFirstFrame);
		const auto inputCount = size_t(segment.inputEnd - segment.inputStart);
		const auto segmentInput = input.subspan(inputOffset, inputCount);
		quint64 written = 0;
		const auto segmentResult = renderSegment(segmentInput, quint64(segment.outputFrames),
			sampleRate, channels, minimumOutputPerInputRatio, maximumOutputPerInputRatio, cancelled,
			[&](quint64 first, std::span<const SampleFrame> block) {
				if (first != written || block.size() > renderedSegments[i].size() - size_t(first))
					return PcmWaveVisitDecision::ResourceLimit;
				std::copy(block.begin(), block.end(), renderedSegments[i].begin() + std::ptrdiff_t(first));
				written += quint64(block.size());
				return PcmWaveVisitDecision::Continue;
			});
		if (segmentResult.code != QStringLiteral("ok")) return failure(segmentResult.code, metrics);
		if (written != quint64(segment.outputFrames)) return failure(QStringLiteral("asset_integrity_failed"), metrics);
		metrics.processCalls += segmentResult.metrics.processCalls;
		metrics.maximumOutputBlockFrames = std::max(metrics.maximumOutputBlockFrames,
			segmentResult.metrics.maximumOutputBlockFrames);
		metrics.maximumProcessCallMicroseconds = std::max(metrics.maximumProcessCallMicroseconds,
			segmentResult.metrics.maximumProcessCallMicroseconds);
		metrics.maximumUninterruptibleInputFrames = std::max(metrics.maximumUninterruptibleInputFrames,
			segmentResult.metrics.maximumUninterruptibleInputFrames);
		const auto bufferFrames = quint64(renderedSegments[i].size());
		sourceViews.push_back({0, std::span<const SampleFrame>(renderedSegments[i])});
		composition.segments.push_back({i, {0, qint64(bufferFrames)},
			{segment.outputStart, segment.outputEnd}});
		if (i && anchors[i].preAnchorCrossfadeFrames > 0)
		{
			composition.boundaries.push_back({i - 1, i, anchors[i].preAnchorCrossfadeFrames,
				anchors[i].crossfadeLaw});
		}
	}

	for (quint64 firstOutputFrame = 0; firstOutputFrame < quint64(anchors.back().outputFrame);)
	{
		if (cancelled.load(std::memory_order_acquire)) return failure(QStringLiteral("cancelled"), metrics);
		const auto count = size_t(std::min<quint64>(MaximumProcessBlockFrames,
			quint64(anchors.back().outputFrame) - firstOutputFrame));
		auto block = std::span<SampleFrame>(outputBlock.data(), count);
		const auto assembled = AiBridgeVocalComp::process(composition, sourceViews,
			qint64(firstOutputFrame), block);
		if (assembled != QStringLiteral("ok")) return failure(assembled, metrics);
		const auto decision = visitor(firstOutputFrame, std::span<const SampleFrame>(block.data(), count));
		if (decision != PcmWaveVisitDecision::Continue)
		{
			return failure(decision == PcmWaveVisitDecision::Cancelled
				? QStringLiteral("cancelled") : QStringLiteral("resource_limit"), metrics);
		}
		firstOutputFrame += quint64(count);
		metrics.framesWritten += quint64(count);
		metrics.maximumOutputBlockFrames = std::max(metrics.maximumOutputBlockFrames, quint64(count));
	}
	return {QStringLiteral("ok"), metrics};
}

} // namespace lmms
