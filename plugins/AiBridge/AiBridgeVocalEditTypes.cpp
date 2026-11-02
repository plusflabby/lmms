#include "AiBridgeVocalEditTypes.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace lmms {
namespace {

bool isDigest(const QString& value)
{
	if (value.size() != 64)
	{
		return false;
	}
	for (const auto ch : value)
	{
		if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
		{
			return false;
		}
	}
	return true;
}

bool isSafeClipId(const QString& value)
{
	if (value.isEmpty() || value.size() > 256)
	{
		return false;
	}
	for (const auto ch : value)
	{
		const bool alphaNumeric = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
			|| (ch >= '0' && ch <= '9');
		if (!alphaNumeric && ch != '-' && ch != '_' && ch != '.' && ch != ':')
		{
			return false;
		}
	}
	return true;
}

bool isValidRange(const VocalEditFrameRange& range)
{
	return range.inFrame >= 0 && range.outFrame > range.inFrame;
}

bool validLimits(const VocalEditLimits& limits)
{
	return limits.maxSelections > 0 && limits.maxSelections <= 8
		&& limits.maxTimingAnchors <= 256
		&& limits.maxGainFadePoints <= 1024
		&& limits.maxCompSegments <= 8
		&& limits.maxCrossfadeFrames >= 0
		&& std::isfinite(limits.minimumGainDb) && std::isfinite(limits.maximumGainDb)
		&& limits.minimumGainDb <= limits.maximumGainDb
		&& std::isfinite(limits.minimumLocalStretchRatio)
		&& std::isfinite(limits.maximumLocalStretchRatio)
		&& limits.minimumLocalStretchRatio > 0.0
		&& limits.minimumLocalStretchRatio <= limits.maximumLocalStretchRatio
		&& std::isfinite(limits.maximumAnchorErrorFrames)
		&& limits.maximumAnchorErrorFrames >= 0.0 && limits.maximumAnchorErrorFrames <= 1.0;
}

std::optional<VocalEditFrameRange> renderInputRange(const VocalEditRequest& request)
{
	if (const auto* direct = std::get_if<VocalDirectInput>(&request.input))
	{
		if (direct->selectionIndex >= request.selections.size())
		{
			return {};
		}
		return request.selections[direct->selectionIndex].sourceFrames;
	}
	const auto& comp = std::get<VocalCompRecipe>(request.input);
	if (comp.segments.empty())
	{
		return {};
	}
	qint64 endFrame = 0;
	for (const auto& segment : comp.segments)
	{
		endFrame = std::max(endFrame, segment.outputFrames.outFrame);
	}
	if (endFrame <= 0)
	{
		return {};
	}
	return VocalEditFrameRange{0, endFrame};
}

void configure(QDataStream& stream)
{
	stream.setVersion(QDataStream::Qt_5_15);
	stream.setByteOrder(QDataStream::BigEndian);
	stream.setFloatingPointPrecision(QDataStream::DoublePrecision);
}

double canonical(double value)
{
	return value == 0.0 ? 0.0 : value;
}

void writeRange(QDataStream& out, const VocalEditFrameRange& range)
{
	out << range.inFrame << range.outFrame;
}

void writeInput(QDataStream& out, const VocalEditInput& input)
{
	out << quint8(input.index());
	if (const auto* direct = std::get_if<VocalDirectInput>(&input))
	{
		out << quint64(direct->selectionIndex);
		return;
	}
	const auto& comp = std::get<VocalCompRecipe>(input);
	out << quint32(comp.segments.size());
	for (const auto& segment : comp.segments)
	{
		out << quint64(segment.selectionIndex);
		writeRange(out, segment.sourceFrames);
		writeRange(out, segment.outputFrames);
	}
	out << quint32(comp.boundaries.size());
	for (const auto& boundary : comp.boundaries)
	{
		out << quint64(boundary.leftSegmentIndex) << quint64(boundary.rightSegmentIndex)
			<< boundary.overlapFrames << quint8(boundary.law);
	}
}

void writeOperation(QDataStream& out, const VocalEditOperation& operation)
{
	out << quint8(operation.index());
	std::visit([&out](const auto& value) {
		using T = std::decay_t<decltype(value)>;
		if constexpr (std::is_same_v<T, VocalGainEnvelope>)
		{
			out << quint32(value.points.size());
			for (const auto& point : value.points)
			{
				out << point.renderInputFrame << canonical(point.gainDb);
			}
		}
		else if constexpr (std::is_same_v<T, VocalFade>)
		{
			writeRange(out, value.renderInputFrames);
			out << canonical(value.startGainDb) << canonical(value.endGainDb) << quint8(value.curve);
		}
		else
		{
			out << quint32(value.anchors.size());
			for (const auto& anchor : value.anchors)
			{
				out << anchor.renderInputFrame << bool(anchor.outputFrame) << anchor.outputFrame.value_or(0)
					<< bool(anchor.targetTick) << anchor.targetTick.value_or(0);
			}
			out << quint32(value.protectedRenderInputFrames.size());
			for (const auto& range : value.protectedRenderInputFrames)
			{
				writeRange(out, range);
			}
		}
	}, operation);
}

} // namespace

std::optional<qint64> parseVocalEditFrame(const QString& text)
{
	if (text.isEmpty() || text.size() > 19 || (text.size() > 1 && text[0] == '0'))
	{
		return {};
	}
	quint64 value = 0;
	for (const auto ch : text)
	{
		if (ch < '0' || ch > '9')
		{
			return {};
		}
		const auto digit = quint64(ch.unicode() - '0');
		const auto maximum = quint64(std::numeric_limits<qint64>::max());
		if (value > (maximum - digit) / 10)
		{
			return {};
		}
		value = value * 10 + digit;
	}
	return qint64(value);
}

QString validateVocalEditRequest(const VocalEditRequest& request, const VocalEditLimits& limits)
{
	if (!validLimits(limits) || request.recipeVersion != 1 || request.expectedProjectGeneration == 0
		|| !isDigest(request.expectedWorldHash))
	{
		return QStringLiteral("invalid_parameter");
	}
	if (request.selections.empty() || request.selections.size() > limits.maxSelections)
	{
		return request.selections.empty() ? QStringLiteral("invalid_parameter") : QStringLiteral("resource_limit");
	}
	for (size_t i = 0; i < request.selections.size(); ++i)
	{
		const auto& selection = request.selections[i];
		if (!isSafeClipId(selection.clipId) || !isDigest(selection.expectedAudioHash)
			|| !isValidRange(selection.sourceFrames)
			|| (selection.basis != VocalEditSourceBasis::SourcePcm
				&& selection.basis != VocalEditSourceBasis::PlaybackReference))
		{
			return QStringLiteral("invalid_parameter");
		}
		for (size_t j = 0; j < i; ++j)
		{
			if (request.selections[j].clipId == selection.clipId)
			{
				return QStringLiteral("invalid_parameter");
			}
		}
	}

	const auto inputRange = renderInputRange(request);
	if (!inputRange || !isValidRange(*inputRange))
	{
		return QStringLiteral("invalid_parameter");
	}
	std::vector<bool> referencedSelections(request.selections.size(), false);
	if (const auto* direct = std::get_if<VocalDirectInput>(&request.input))
	{
		if (request.selections.size() != 1 || direct->selectionIndex != 0)
		{
			return QStringLiteral("invalid_parameter");
		}
		referencedSelections[direct->selectionIndex] = true;
	}
	else
	{
		const auto& comp = std::get<VocalCompRecipe>(request.input);
		if (comp.segments.size() > limits.maxCompSegments)
		{
			return QStringLiteral("resource_limit");
		}
		if (comp.segments.empty())
		{
			return QStringLiteral("invalid_parameter");
		}
		if (comp.boundaries.size() >= comp.segments.size())
		{
			return QStringLiteral("invalid_parameter");
		}
		for (size_t i = 0; i < comp.segments.size(); ++i)
		{
			const auto& segment = comp.segments[i];
			if (segment.selectionIndex >= request.selections.size()
				|| !isValidRange(segment.sourceFrames) || !isValidRange(segment.outputFrames))
			{
				return QStringLiteral("invalid_parameter");
			}
			referencedSelections[segment.selectionIndex] = true;
			const auto& selected = request.selections[segment.selectionIndex].sourceFrames;
			if (segment.sourceFrames.inFrame < selected.inFrame || segment.sourceFrames.outFrame > selected.outFrame
				|| (i && segment.outputFrames.inFrame < comp.segments[i - 1].outputFrames.inFrame))
			{
				return QStringLiteral("invalid_parameter");
			}
		}
		if (std::find(referencedSelections.begin(), referencedSelections.end(), false) != referencedSelections.end())
		{
			return QStringLiteral("invalid_parameter");
		}
		size_t boundaryIndex = 0;
		for (size_t i = 0; i + 1 < comp.segments.size(); ++i)
		{
			const auto& left = comp.segments[i];
			for (size_t j = i + 1; j < comp.segments.size(); ++j)
			{
				const auto& right = comp.segments[j];
				if (right.outputFrames.inFrame >= left.outputFrames.outFrame)
				{
					break;
				}
				if (j != i + 1)
				{
					return QStringLiteral("invalid_parameter");
				}
				const auto overlap = left.outputFrames.outFrame - right.outputFrames.inFrame;
				if (boundaryIndex >= comp.boundaries.size())
				{
					return QStringLiteral("invalid_parameter");
				}
				const auto& boundary = comp.boundaries[boundaryIndex++];
				if (boundary.leftSegmentIndex != i || boundary.rightSegmentIndex != j
					|| boundary.overlapFrames != overlap || boundary.overlapFrames <= 0
					|| boundary.overlapFrames > limits.maxCrossfadeFrames
					|| (boundary.law != VocalCrossfadeLaw::LinearAmplitude
						&& boundary.law != VocalCrossfadeLaw::EqualPower))
				{
					return boundary.overlapFrames > limits.maxCrossfadeFrames
						? QStringLiteral("resource_limit") : QStringLiteral("invalid_parameter");
				}
			}
		}
		if (boundaryIndex != comp.boundaries.size())
		{
			return QStringLiteral("invalid_parameter");
		}
	}

	if (request.operations.empty() && std::holds_alternative<VocalDirectInput>(request.input))
	{
		return QStringLiteral("invalid_parameter");
	}
	bool sawTimingMap = false;
	for (const auto& operation : request.operations)
	{
		const auto code = std::visit([&](const auto& value) -> QString {
			using T = std::decay_t<decltype(value)>;
			if constexpr (std::is_same_v<T, VocalGainEnvelope>)
			{
				if (sawTimingMap)
				{
					return QStringLiteral("invalid_parameter");
				}
				if (value.points.size() > limits.maxGainFadePoints)
				{
					return QStringLiteral("resource_limit");
				}
				if (value.points.size() < 2)
				{
					return QStringLiteral("invalid_parameter");
				}
				qint64 previous = -1;
				for (const auto& point : value.points)
				{
					if (point.renderInputFrame < inputRange->inFrame || point.renderInputFrame > inputRange->outFrame
						|| point.renderInputFrame <= previous || !std::isfinite(point.gainDb)
						|| point.gainDb < limits.minimumGainDb || point.gainDb > limits.maximumGainDb)
					{
						return QStringLiteral("invalid_parameter");
					}
					previous = point.renderInputFrame;
				}
				return QStringLiteral("ok");
			}
			else if constexpr (std::is_same_v<T, VocalFade>)
			{
				if (sawTimingMap || !isValidRange(value.renderInputFrames)
					|| value.renderInputFrames.inFrame < inputRange->inFrame
					|| value.renderInputFrames.outFrame > inputRange->outFrame
					|| !std::isfinite(value.startGainDb) || !std::isfinite(value.endGainDb)
					|| value.startGainDb < limits.minimumGainDb || value.startGainDb > limits.maximumGainDb
					|| value.endGainDb < limits.minimumGainDb || value.endGainDb > limits.maximumGainDb
					|| (value.curve != VocalFadeCurve::LinearAmplitude
						&& value.curve != VocalFadeCurve::EqualPower))
				{
					return QStringLiteral("invalid_parameter");
				}
				return QStringLiteral("ok");
			}
			else
			{
				if (sawTimingMap)
				{
					return QStringLiteral("invalid_parameter");
				}
				sawTimingMap = true;
				if (value.anchors.size() > limits.maxTimingAnchors)
				{
					return QStringLiteral("resource_limit");
				}
				if (value.anchors.size() < 2)
				{
					return QStringLiteral("invalid_parameter");
				}
				if (value.anchors.front().renderInputFrame != inputRange->inFrame
					|| value.anchors.back().renderInputFrame != inputRange->outFrame)
				{
					return QStringLiteral("invalid_parameter");
				}
				qint64 previousInput = -1;
				std::optional<qint64> previousOutput;
				std::optional<qint64> previousTick;
				for (const auto& anchor : value.anchors)
				{
					if (anchor.renderInputFrame <= previousInput || anchor.renderInputFrame < inputRange->inFrame
						|| anchor.renderInputFrame > inputRange->outFrame
						|| bool(anchor.outputFrame) == bool(anchor.targetTick))
					{
						return QStringLiteral("invalid_parameter");
					}
					if (anchor.outputFrame)
					{
						if (*anchor.outputFrame < 0 || (previousOutput && *anchor.outputFrame <= *previousOutput))
						{
							return QStringLiteral("invalid_parameter");
						}
						previousOutput = *anchor.outputFrame;
					}
					else
					{
						if (*anchor.targetTick < 0 || (previousTick && *anchor.targetTick <= *previousTick))
						{
							return QStringLiteral("invalid_parameter");
						}
						previousTick = *anchor.targetTick;
					}
					previousInput = anchor.renderInputFrame;
				}
				qint64 previousRangeEnd = -1;
				for (const auto& range : value.protectedRenderInputFrames)
				{
					if (!isValidRange(range) || range.inFrame < inputRange->inFrame
						|| range.outFrame > inputRange->outFrame || range.inFrame < previousRangeEnd)
					{
						return QStringLiteral("invalid_parameter");
					}
					previousRangeEnd = range.outFrame;
				}
				return QStringLiteral("ok");
			}
		}, operation);
		if (code != QStringLiteral("ok"))
		{
			return code;
		}
	}
	return QStringLiteral("ok");
}

QString canonicalVocalEditRecipeHash(const VocalEditRequest& request)
{
	QByteArray data;
	QDataStream out(&data, QIODevice::WriteOnly);
	configure(out);
	out << QStringLiteral("lmms-vocal-edit-recipe-v1") << request.recipeVersion
		<< request.expectedProjectGeneration << request.expectedWorldHash
		<< quint32(request.selections.size());
	for (const auto& selection : request.selections)
	{
		out << selection.clipId << selection.expectedAudioHash << quint8(selection.basis);
		writeRange(out, selection.sourceFrames);
	}
	writeInput(out, request.input);
	out << quint32(request.operations.size());
	for (const auto& operation : request.operations)
	{
		writeOperation(out, operation);
	}
	return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

} // namespace lmms
