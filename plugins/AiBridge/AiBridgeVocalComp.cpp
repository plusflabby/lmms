#include "AiBridgeVocalComp.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace lmms {
namespace {

bool validRange(const VocalEditFrameRange& range)
{
	return range.inFrame >= 0 && range.outFrame > range.inFrame;
}

const VocalCompBoundary* boundaryFor(
	const VocalCompRecipe& recipe,
	size_t leftIndex,
	size_t rightIndex)
{
	for (const auto& boundary : recipe.boundaries)
	{
		if (boundary.leftSegmentIndex == leftIndex && boundary.rightSegmentIndex == rightIndex)
		{
			return &boundary;
		}
	}
	return nullptr;
}

bool validateRecipe(const VocalCompRecipe& recipe, std::span<const VocalCompSourceView> sources)
{
	if (recipe.segments.empty() || recipe.segments.front().outputFrames.inFrame != 0
		|| recipe.boundaries.size() >= recipe.segments.size())
	{
		return false;
	}
	for (const auto& source : sources)
	{
		if (source.firstSourceFrame < 0 || source.frames.empty()
			|| source.frames.size() > size_t(std::numeric_limits<qint64>::max())
			|| source.firstSourceFrame > std::numeric_limits<qint64>::max() - qint64(source.frames.size()))
		{
			return false;
		}
	}
	for (size_t i = 0; i < recipe.segments.size(); ++i)
	{
		const auto& segment = recipe.segments[i];
		if (segment.selectionIndex >= sources.size() || !validRange(segment.sourceFrames)
			|| !validRange(segment.outputFrames)
			|| segment.sourceFrames.outFrame - segment.sourceFrames.inFrame
				!= segment.outputFrames.outFrame - segment.outputFrames.inFrame)
		{
			return false;
		}
		const auto& source = sources[segment.selectionIndex];
		const auto sourceEnd = source.firstSourceFrame + qint64(source.frames.size());
		if (segment.sourceFrames.inFrame < source.firstSourceFrame || segment.sourceFrames.outFrame > sourceEnd)
		{
			return false;
		}
		if (i)
		{
			const auto& previous = recipe.segments[i - 1];
			if (segment.outputFrames.inFrame < previous.outputFrames.inFrame
				|| segment.outputFrames.inFrame > previous.outputFrames.outFrame)
			{
				return false;
			}
			if (segment.outputFrames.inFrame < previous.outputFrames.outFrame)
			{
				const auto overlap = previous.outputFrames.outFrame - segment.outputFrames.inFrame;
				const auto* boundary = boundaryFor(recipe, i - 1, i);
				const auto previousLength = previous.outputFrames.outFrame - previous.outputFrames.inFrame;
				const auto currentLength = segment.outputFrames.outFrame - segment.outputFrames.inFrame;
				if (!boundary || boundary->overlapFrames != overlap || overlap > previousLength / 2
					|| overlap > currentLength / 2
					|| (boundary->law != VocalCrossfadeLaw::LinearAmplitude
						&& boundary->law != VocalCrossfadeLaw::EqualPower))
				{
					return false;
				}
			}
		}
	}
	for (const auto& boundary : recipe.boundaries)
	{
		if (boundary.leftSegmentIndex >= recipe.segments.size()
			|| boundary.rightSegmentIndex != boundary.leftSegmentIndex + 1)
		{
			return false;
		}
		const auto& left = recipe.segments[boundary.leftSegmentIndex];
		const auto& right = recipe.segments[boundary.rightSegmentIndex];
		if (right.outputFrames.inFrame >= left.outputFrames.outFrame
			|| boundary.overlapFrames != left.outputFrames.outFrame - right.outputFrames.inFrame)
		{
			return false;
		}
	}
	return true;
}

bool readFrame(const VocalCompSegment& segment, const VocalCompSourceView& source,
	qint64 outputFrame, SampleFrame& sample)
{
	const auto sourceFrame = segment.sourceFrames.inFrame + outputFrame - segment.outputFrames.inFrame;
	const auto offset = sourceFrame - source.firstSourceFrame;
	if (offset < 0 || quint64(offset) >= quint64(source.frames.size())) return false;
	sample = source.frames[size_t(offset)];
	return std::isfinite(sample.left()) && std::isfinite(sample.right());
}

} // namespace

QString AiBridgeVocalComp::process(
	const VocalCompRecipe& recipe,
	std::span<const VocalCompSourceView> sources,
	qint64 firstOutputFrame,
	std::span<SampleFrame> output)
{
	if (firstOutputFrame < 0 || output.size() > size_t(std::numeric_limits<qint64>::max())
		|| firstOutputFrame > std::numeric_limits<qint64>::max() - qint64(output.size())
		|| !validateRecipe(recipe, sources))
	{
		return QStringLiteral("invalid_parameter");
	}
	if (output.empty()) return QStringLiteral("ok");
	const auto endOutputFrame = firstOutputFrame + qint64(output.size());
	size_t activeSegment = 0;
	for (qint64 outputFrame = firstOutputFrame; outputFrame < endOutputFrame; ++outputFrame)
	{
		while (activeSegment + 1 < recipe.segments.size()
			&& outputFrame >= recipe.segments[activeSegment].outputFrames.outFrame)
		{
			++activeSegment;
		}
		const auto& left = recipe.segments[activeSegment];
		if (outputFrame < left.outputFrames.inFrame || outputFrame >= left.outputFrames.outFrame)
		{
			return QStringLiteral("invalid_parameter");
		}
		SampleFrame sample;
		if (activeSegment + 1 < recipe.segments.size())
		{
			const auto& right = recipe.segments[activeSegment + 1];
			if (outputFrame >= right.outputFrames.inFrame && outputFrame < left.outputFrames.outFrame)
			{
				const auto* boundary = boundaryFor(recipe, activeSegment, activeSegment + 1);
				if (!boundary || !readFrame(left, sources[left.selectionIndex], outputFrame, sample))
				{
					return QStringLiteral("invalid_parameter");
				}
				SampleFrame rightSample;
				if (!readFrame(right, sources[right.selectionIndex], outputFrame, rightSample))
				{
					return QStringLiteral("invalid_parameter");
				}
				const auto overlapOffset = outputFrame - right.outputFrames.inFrame;
				const auto blend = (double(overlapOffset) + 0.5) / double(boundary->overlapFrames);
				double leftWeight = 1.0 - blend;
				double rightWeight = blend;
				if (boundary->law == VocalCrossfadeLaw::EqualPower)
				{
					const auto angle = blend * std::numbers::pi_v<double> / 2.0;
					leftWeight = std::cos(angle);
					rightWeight = std::sin(angle);
				}
				const auto mixedLeft = double(sample.left()) * leftWeight + double(rightSample.left()) * rightWeight;
				const auto mixedRight = double(sample.right()) * leftWeight + double(rightSample.right()) * rightWeight;
				if (!std::isfinite(mixedLeft) || !std::isfinite(mixedRight)
					|| std::abs(mixedLeft) > std::numeric_limits<float>::max()
					|| std::abs(mixedRight) > std::numeric_limits<float>::max())
				{
					return QStringLiteral("invalid_audio");
				}
				sample = SampleFrame(float(mixedLeft), float(mixedRight));
			}
			else if (!readFrame(left, sources[left.selectionIndex], outputFrame, sample))
			{
				return QStringLiteral("invalid_audio");
			}
		}
		else if (!readFrame(left, sources[left.selectionIndex], outputFrame, sample))
		{
			return QStringLiteral("invalid_audio");
		}
		output[size_t(outputFrame - firstOutputFrame)] = sample;
	}
	return QStringLiteral("ok");
}

} // namespace lmms
