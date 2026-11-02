#include "AiBridgeVocalGainFade.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <type_traits>

namespace lmms {
namespace {

bool validRange(const VocalEditFrameRange& range)
{
	return range.inFrame >= 0 && range.outFrame > range.inFrame;
}

bool validEnvelope(const VocalGainEnvelope& envelope, const VocalEditFrameRange& domain)
{
	if (envelope.points.size() < 2) return false;
	qint64 previous = -1;
	for (const auto& point : envelope.points)
	{
		if (point.renderInputFrame < domain.inFrame || point.renderInputFrame > domain.outFrame
			|| point.renderInputFrame <= previous || !std::isfinite(point.gainDb))
		{
			return false;
		}
		previous = point.renderInputFrame;
	}
	return !(envelope.points.front().renderInputFrame > domain.inFrame
		&& envelope.points.front().gainDb != 0.0)
		&& !(envelope.points.back().renderInputFrame < domain.outFrame
			&& envelope.points.back().gainDb != 0.0);
}

bool validFade(const VocalFade& fade, const VocalEditFrameRange& domain)
{
	return validRange(fade.renderInputFrames)
		&& fade.renderInputFrames.inFrame >= domain.inFrame
		&& fade.renderInputFrames.outFrame <= domain.outFrame
		&& std::isfinite(fade.startGainDb) && std::isfinite(fade.endGainDb)
		&& !(fade.renderInputFrames.inFrame > domain.inFrame && fade.startGainDb != 0.0)
		&& !(fade.renderInputFrames.outFrame < domain.outFrame && fade.endGainDb != 0.0)
		&& (fade.curve == VocalFadeCurve::LinearAmplitude
			|| fade.curve == VocalFadeCurve::EqualPower);
}

double amplitudeFromDb(double gainDb)
{
	return std::pow(10.0, gainDb / 20.0);
}

double envelopeAmplitude(const VocalGainEnvelope& envelope, qint64 frame)
{
	if (frame < envelope.points.front().renderInputFrame
		|| frame > envelope.points.back().renderInputFrame)
	{
		return 1.0;
	}
	const auto upper = std::upper_bound(envelope.points.begin(), envelope.points.end(), frame,
		[](qint64 value, const VocalGainPoint& point) {
			return value < point.renderInputFrame;
		});
	if (upper == envelope.points.begin()) return amplitudeFromDb(upper->gainDb);
	if (upper == envelope.points.end()) return amplitudeFromDb(envelope.points.back().gainDb);
	const auto& right = *upper;
	const auto& left = *(upper - 1);
	const auto fraction = double(frame - left.renderInputFrame)
		/ double(right.renderInputFrame - left.renderInputFrame);
	return amplitudeFromDb(left.gainDb + (right.gainDb - left.gainDb) * fraction);
}

double fadeAmplitude(const VocalFade& fade, qint64 frame)
{
	if (frame < fade.renderInputFrames.inFrame || frame >= fade.renderInputFrames.outFrame)
	{
		return 1.0;
	}
	const auto count = fade.renderInputFrames.outFrame - fade.renderInputFrames.inFrame;
	const auto fraction = count == 1 ? 1.0
		: double(frame - fade.renderInputFrames.inFrame) / double(count - 1);
	const auto start = amplitudeFromDb(fade.startGainDb);
	const auto end = amplitudeFromDb(fade.endGainDb);
	if (fade.curve == VocalFadeCurve::LinearAmplitude)
	{
		return start + (end - start) * fraction;
	}
	const auto angle = fraction * std::numbers::pi_v<double> / 2.0;
	return start * std::cos(angle) + end * std::sin(angle);
}

} // namespace

QString AiBridgeVocalGainFade::process(
	const VocalEditFrameRange& renderInputFrames,
	const std::vector<VocalEditOperation>& operations,
	qint64 firstRenderInputFrame,
	std::span<const SampleFrame> input,
	int channels,
	std::span<float> interleavedOutput)
{
	if (!validRange(renderInputFrames) || firstRenderInputFrame < renderInputFrames.inFrame
		|| (channels != 1 && channels != 2)
		|| input.size() > std::size_t(std::numeric_limits<qint64>::max())
		|| firstRenderInputFrame > std::numeric_limits<qint64>::max() - qint64(input.size())
		|| firstRenderInputFrame + qint64(input.size()) > renderInputFrames.outFrame
		|| input.size() > std::numeric_limits<std::size_t>::max() / std::size_t(channels)
		|| interleavedOutput.size() != input.size() * std::size_t(channels))
	{
		return QStringLiteral("invalid_parameter");
	}

	for (const auto& operation : operations)
	{
		const auto valid = std::visit([&](const auto& value) {
			using T = std::decay_t<decltype(value)>;
			if constexpr (std::is_same_v<T, VocalGainEnvelope>)
			{
				return validEnvelope(value, renderInputFrames);
			}
			else if constexpr (std::is_same_v<T, VocalFade>)
			{
				return validFade(value, renderInputFrames);
			}
			else
			{
				// Timing maps require the separately qualified B4 renderer.
				return false;
			}
		}, operation);
		if (!valid) return QStringLiteral("invalid_parameter");
	}

	for (std::size_t index = 0; index < input.size(); ++index)
	{
		const auto left = double(input[index].left());
		const auto right = double(input[index].right());
		if (!std::isfinite(left) || (channels == 2 && !std::isfinite(right)))
		{
			return QStringLiteral("invalid_audio");
		}
		auto gain = 1.0;
		const auto frame = firstRenderInputFrame + qint64(index);
		for (const auto& operation : operations)
		{
			const auto operationGain = std::visit([frame](const auto& value) {
				using T = std::decay_t<decltype(value)>;
				if constexpr (std::is_same_v<T, VocalGainEnvelope>)
				{
					return envelopeAmplitude(value, frame);
				}
				else if constexpr (std::is_same_v<T, VocalFade>)
				{
					return fadeAmplitude(value, frame);
				}
				else
				{
					return std::numeric_limits<double>::quiet_NaN();
				}
			}, operation);
			gain *= operationGain;
		}
		const auto outputLeft = left * gain;
		const auto outputRight = right * gain;
		if (!std::isfinite(gain) || !std::isfinite(outputLeft)
			|| (channels == 2 && !std::isfinite(outputRight))
			|| std::abs(outputLeft) > std::numeric_limits<float>::max()
			|| (channels == 2 && std::abs(outputRight) > std::numeric_limits<float>::max()))
		{
			return QStringLiteral("invalid_audio");
		}
		interleavedOutput[index * std::size_t(channels)] = float(outputLeft);
		if (channels == 2)
		{
			interleavedOutput[index * 2 + 1] = float(outputRight);
		}
	}
	return QStringLiteral("ok");
}

} // namespace lmms
