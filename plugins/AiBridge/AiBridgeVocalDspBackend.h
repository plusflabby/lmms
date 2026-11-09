#ifndef LMMS_AI_BRIDGE_VOCAL_DSP_BACKEND_H
#define LMMS_AI_BRIDGE_VOCAL_DSP_BACKEND_H

#include "SampleDecoder.h"
#include "SampleFrame.h"
#include "AiBridgeVocalEditTypes.h"
#include <atomic>
#include <span>

namespace lmms {

struct VocalDspMetrics
{
	quint64 framesWritten = 0;
	quint64 processCalls = 0;
	quint64 maximumOutputBlockFrames = 0;
	quint64 maximumProcessCallMicroseconds = 0;
	quint64 maximumUninterruptibleInputFrames = 0;
	quint64 reservedWorkspaceBytes = 0;
};

struct VocalDspRenderResult
{
	QString code;
	VocalDspMetrics metrics;
};

struct VocalDspTimingAnchor
{
	qint64 inputFrame = 0;
	qint64 outputFrame = 0;
	qint64 preAnchorCrossfadeFrames = 0;
	VocalCrossfadeLaw crossfadeLaw = VocalCrossfadeLaw::LinearAmplitude;
	bool copyProtectedInterval = false;
};

// Bounded worker-only adapter for the pinned Signalsmith Stretch implementation.
// It preserves the requested output frame count, streams fixed-size blocks, and
// never owns a Song, clip, QObject, file, or device.
class AiBridgeVocalDspBackend
{
public:
	inline static constexpr char RendererId[] = "signalsmith-stretch";
	inline static constexpr char RendererVersion[] = "1.3.2-88061fd4eda5-0.6.6-7bb1c8f3";
	inline static constexpr char StretchVersion[] = "1.3.2";
	inline static constexpr char StretchSourceCommit[] = "88061fd4eda56a2353c5d65c74aa3f4ccf76f9da";
	inline static constexpr char LinearVersion[] = "0.6.6";
	inline static constexpr char LinearDependencyCommit[] = "7bb1c8f36794d6980080ce817c91f8da8fe56a66";
	static constexpr quint64 MaximumProcessBlockFrames = 4096;
	static constexpr quint64 FixedWorkspaceChargeBytes = 8ULL * 1024 * 1024;

	static quint64 reservedWorkspaceBytes(int sampleRate, int channels);
	// Returns the fixed preset's minimum segment context, or -1 if unsupported.
	static int minimumInputFrames(int sampleRate, int channels, double outputPerInputRatio);
	static VocalDspRenderResult renderSegment(
		std::span<const SampleFrame> input,
		quint64 outputFrames,
		int sampleRate,
		int channels,
		double minimumOutputPerInputRatio,
		double maximumOutputPerInputRatio,
		const std::atomic_bool& cancelled,
		const PcmWaveChunkVisitor& visitor);
	static VocalDspRenderResult renderTimingMap(
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
		const PcmWaveChunkVisitor& visitor);
};

} // namespace lmms

#endif
