#include "ManagedVocalPlayback.h"

#include "TimePos.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <numeric>
#include <utility>

namespace lmms {
namespace {
bool supportedRate(int rate)
{
	return rate == 44100 || rate == 48000;
}
}

ManagedVocalVisualExtent managedVocalVisualExtent(quint64 mediaFrames, int sampleRate, int tempoBpm)
{
	if (!mediaFrames || !supportedRate(sampleRate) || tempoBpm < 10 || tempoBpm > 999
		|| mediaFrames > quint64(sampleRate) * 120)
	{
		return {QStringLiteral("invalid_parameter"), 0, 0, 0, 0, 1};
	}
	const quint64 idealTickNumerator = mediaFrames * quint64(tempoBpm) * 48;
	const quint64 idealTickDenominator = quint64(sampleRate) * 60;
	auto ticks = idealTickNumerator / idealTickDenominator
		+ quint64(idealTickNumerator % idealTickDenominator != 0);
	if (ticks > quint64(std::numeric_limits<tick_t>::max()))
	{
		return {QStringLiteral("resource_limit"), 0, 0, 0, 0, 1};
	}
	const auto nativeFramesPerTick = sampleRate * 60.0f * 4 / DefaultTicksPerBar / tempoBpm;
	auto nativeCoverage = TimePos(int(ticks)).frames(nativeFramesPerTick);
	while (nativeCoverage < mediaFrames && ticks < quint64(std::numeric_limits<tick_t>::max()))
	{
		++ticks;
		nativeCoverage = TimePos(int(ticks)).frames(nativeFramesPerTick);
	}
	while (ticks > 1)
	{
		const auto previousCoverage = TimePos(int(ticks - 1)).frames(nativeFramesPerTick);
		if (previousCoverage < mediaFrames) break;
		--ticks;
		nativeCoverage = previousCoverage;
	}
	if (nativeCoverage < mediaFrames || ticks > quint64(std::numeric_limits<tick_t>::max()))
	{
		return {QStringLiteral("unrepresentable_audio_boundary"), 0, 0, 0, 0, 1};
	}
	const auto numerator = qint64(ticks * quint64(sampleRate) * 60)
		- qint64(idealTickNumerator);
	qint64 residualNumerator = numerator;
	qint64 residualDenominator = qint64(quint64(tempoBpm) * 48);
	const auto magnitude = residualNumerator < 0
		? quint64(-(residualNumerator + 1)) + 1 : quint64(residualNumerator);
	const auto divisor = std::gcd(magnitude, quint64(residualDenominator));
	if (divisor)
	{
		residualNumerator /= qint64(divisor);
		residualDenominator /= qint64(divisor);
	}
	return {QStringLiteral("ok"), qint64(ticks), nativeCoverage,
		nativeCoverage - mediaFrames, residualNumerator, residualDenominator};
}

ManagedVocalPlaybackPreparation makeManagedVocalPlaybackView(
	std::vector<float> interleavedSamples,
	int sampleRate,
	int channels,
	const ManagedVocalPlaybackWorld& world,
	bool reversed,
	bool autoResize,
	std::shared_ptr<const ManagedVocalPlaybackGuard> guard,
	quint64 expectedEpoch,
	float amplification)
{
	try
	{
		auto samples = std::make_shared<const std::vector<float>>(std::move(interleavedSamples));
		return makeManagedVocalPlaybackView(std::move(samples), sampleRate, channels, world,
			reversed, autoResize, std::move(guard), expectedEpoch, amplification);
	}
	catch (const std::bad_alloc&)
	{
		return {QStringLiteral("resource_limit"), {}};
	}
}

ManagedVocalPlaybackPreparation makeManagedVocalPlaybackView(
	std::shared_ptr<const std::vector<float>> interleavedSamples,
	int sampleRate,
	int channels,
	const ManagedVocalPlaybackWorld& world,
	bool reversed,
	bool autoResize,
	std::shared_ptr<const ManagedVocalPlaybackGuard> guard,
	quint64 expectedEpoch,
	float amplification)
{
	if (!supportedRate(sampleRate) || sampleRate != world.outputRate || world.tempoBpm <= 0
		|| world.ticksPerQuarterNote != 48 || world.timeSignatureNumerator < 1
		|| world.timeSignatureNumerator > 32 || world.timeSignatureDenominator < 1
		|| world.timeSignatureDenominator > 32 || world.tempoAutomationPresent)
	{
		return {QStringLiteral("unsupported_playback_world"), {}};
	}
	if (reversed || autoResize)
	{
		return {QStringLiteral("unsupported_clip_state"), {}};
	}
	if ((channels != 1 && channels != 2) || !interleavedSamples || interleavedSamples->empty()
		|| interleavedSamples->size() % std::size_t(channels) != 0
		|| !guard || !expectedEpoch || !std::isfinite(amplification)
		|| guard->epoch.load(std::memory_order_acquire) != expectedEpoch)
	{
		return {QStringLiteral("invalid_audio_samples"), {}};
	}
	if (std::any_of(interleavedSamples->begin(), interleavedSamples->end(), [](float sample) {
		return !std::isfinite(sample);
	}))
	{
		return {QStringLiteral("invalid_audio_samples"), {}};
	}
	const auto frames = interleavedSamples->size() / std::size_t(channels);
	if (frames > quint64(sampleRate) * 120
		|| frames > std::numeric_limits<quint64>::max() / quint64(channels))
	{
		return {QStringLiteral("resource_limit"), {}};
	}
	try
	{
		auto view = std::make_shared<ManagedVocalPlaybackView>();
		view->interleavedSamples = std::move(interleavedSamples);
		view->guard = std::move(guard);
		view->expectedEpoch = expectedEpoch;
		view->frames = quint64(frames);
		view->sampleRate = sampleRate;
		view->channels = channels;
		view->amplification = amplification;
		return {QStringLiteral("ok"), std::move(view)};
	}
	catch (const std::bad_alloc&)
	{
		return {QStringLiteral("resource_limit"), {}};
	}
}

void renderManagedVocalPlayback(
	const ManagedVocalPlaybackView& view,
	qint64 firstFrame,
	std::span<SampleFrame> destination)
{
	std::fill(destination.begin(), destination.end(), SampleFrame{});
	if (!view.interleavedSamples || !view.guard || !view.expectedEpoch || firstFrame < 0
		|| quint64(firstFrame) >= view.frames
		|| view.guard->epoch.load(std::memory_order_acquire) != view.expectedEpoch
		|| (view.channels != 1 && view.channels != 2)
		|| !std::isfinite(view.amplification)
		|| view.interleavedSamples->size() != std::size_t(view.frames) * std::size_t(view.channels))
	{
		return;
	}
	const auto available = view.frames - quint64(firstFrame);
	const auto count = std::min<quint64>(available, quint64(destination.size()));
	const auto sourceIndex = std::size_t(firstFrame) * std::size_t(view.channels);
	for (quint64 frame = 0; frame < count; ++frame)
	{
		const auto sampleIndex = sourceIndex + std::size_t(frame) * std::size_t(view.channels);
		if (view.channels == 1)
		{
			destination[std::size_t(frame)] = SampleFrame((*view.interleavedSamples)[sampleIndex]
				* view.amplification);
		}
		else
		{
			destination[std::size_t(frame)] = SampleFrame((*view.interleavedSamples)[sampleIndex],
				(*view.interleavedSamples)[sampleIndex + 1]);
			destination[std::size_t(frame)] *= view.amplification;
		}
	}
}

} // namespace lmms
