#include "AiBridgeVocalEditPlanner.h"

#include "AiBridgeVocalTypes.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

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

bool supportedRate(int rate)
{
	return rate == 44100 || rate == 48000;
}

QString finishHash(const QByteArray& data)
{
	return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

bool tickToFrame(qint64 tick, const VocalEditWorld& world, qint64& frame,
	qint64& errorNumerator, qint64& errorDenominator, double& error)
{
	if (tick < 0 || world.tempoBpm <= 0 || world.ticksPerQuarterNote <= 0
		|| !supportedRate(world.outputRate))
	{
		return false;
	}
	const quint64 numeratorFactor = quint64(world.outputRate) * 60;
	const quint64 denominator = quint64(world.tempoBpm) * quint64(world.ticksPerQuarterNote);
	const quint64 maximumFrames = quint64(world.outputRate) * VocalLimits::seconds;
	const quint64 tickValue = quint64(tick);
	const quint64 wholeTicks = tickValue / denominator;
	if (wholeTicks > maximumFrames / numeratorFactor)
	{
		return false;
	}
	const quint64 wholeFrames = wholeTicks * numeratorFactor;
	const quint64 tickRemainder = tickValue % denominator;
	quint64 fractionalFrames = 0;
	quint64 fractionalRemainder = 0;
	for (int bit = 63; bit >= 0; --bit)
	{
		fractionalFrames *= 2;
		fractionalRemainder *= 2;
		if ((numeratorFactor >> bit) & 1)
		{
			fractionalRemainder += tickRemainder;
		}
		fractionalFrames += fractionalRemainder / denominator;
		fractionalRemainder %= denominator;
	}
	const quint64 floorFrames = wholeFrames + fractionalFrames;
	if (floorFrames > maximumFrames)
	{
		return false;
	}
	const quint64 halfThreshold = denominator / 2 + denominator % 2;
	const bool roundsUp = fractionalRemainder >= halfThreshold;
	const quint64 roundedFrames = floorFrames + quint64(roundsUp);
	if (roundedFrames > maximumFrames || roundedFrames > quint64(std::numeric_limits<qint64>::max()))
	{
		return false;
	}
	frame = qint64(roundedFrames);
	errorNumerator = roundsUp ? qint64(denominator - fractionalRemainder)
		: -qint64(fractionalRemainder);
	errorDenominator = qint64(denominator);
	if (errorNumerator == 0)
	{
		errorDenominator = 1;
	}
	else
	{
		const auto divisor = std::gcd(quint64(std::abs(errorNumerator)), quint64(errorDenominator));
		errorNumerator /= qint64(divisor);
		errorDenominator /= qint64(divisor);
	}
	error = static_cast<double>(errorNumerator) / static_cast<double>(errorDenominator);
	return std::isfinite(error);
}

const VocalTimingMap* timingMap(const VocalEditRequest& request)
{
	for (const auto& operation : request.operations)
	{
		if (const auto* map = std::get_if<VocalTimingMap>(&operation))
		{
			return map;
		}
	}
	return nullptr;
}

VocalEditFrameRange renderInputRange(const VocalEditRequest& request)
{
	if (const auto* direct = std::get_if<VocalDirectInput>(&request.input))
	{
		return request.selections[direct->selectionIndex].sourceFrames;
	}
	qint64 endFrame = 0;
	for (const auto& segment : std::get<VocalCompRecipe>(request.input).segments)
	{
		endFrame = std::max(endFrame, segment.outputFrames.outFrame);
	}
	return {0, endFrame};
}

quint64 totalSelectedInputFrames(const VocalEditRequest& request)
{
	if (const auto* direct = std::get_if<VocalDirectInput>(&request.input))
	{
		const auto& range = request.selections[direct->selectionIndex].sourceFrames;
		return quint64(range.outFrame - range.inFrame);
	}
	quint64 total = 0;
	for (const auto& segment : std::get<VocalCompRecipe>(request.input).segments)
	{
		const auto frames = quint64(segment.sourceFrames.outFrame - segment.sourceFrames.inFrame);
		if (frames > std::numeric_limits<quint64>::max() - total)
		{
			return std::numeric_limits<quint64>::max();
		}
		total += frames;
	}
	return total;
}

} // namespace

VocalEditPlanResult AiBridgeVocalEditPlanner::plan(
	const VocalEditRequest& request,
	const VocalEditPlanningSnapshot& snapshot,
	const VocalEditLimits& limits)
{
	VocalEditPlanResult result{QStringLiteral("invalid_parameter"), {}};
	if (validateVocalEditRequest(request, limits) != QStringLiteral("ok"))
	{
		return result;
	}
	if (!snapshot.generation || snapshot.generation != request.expectedProjectGeneration
		|| snapshot.worldHash != request.expectedWorldHash || !isDigest(snapshot.worldHash))
	{
		result.code = QStringLiteral("plan_stale");
		return result;
	}
	if (snapshot.world.tempoAutomationPresent || snapshot.world.tempoBpm <= 0
		|| snapshot.world.ticksPerQuarterNote != 48
		|| snapshot.world.timeSignatureNumerator < 1 || snapshot.world.timeSignatureNumerator > 32
		|| snapshot.world.timeSignatureDenominator < 1 || snapshot.world.timeSignatureDenominator > 32
		|| snapshot.sources.size() != request.selections.size())
	{
		result.code = QStringLiteral("unsupported_playback_world");
		return result;
	}
	if (!supportedRate(snapshot.world.outputRate))
	{
		result.code = QStringLiteral("unsupported_sample_rate");
		return result;
	}

	int commonChannels = 0;
	for (size_t i = 0; i < request.selections.size(); ++i)
	{
		const auto& selection = request.selections[i];
		const auto& source = snapshot.sources[i];
		if (selection.basis != VocalEditSourceBasis::SourcePcm)
		{
			result.code = QStringLiteral("unsupported_source_basis");
			return result;
		}
		if (source.clipId != selection.clipId || source.audioStateHash != selection.expectedAudioHash
			|| !source.sourceVerified || !isDigest(source.audioStateHash)
			|| quint64(selection.sourceFrames.outFrame) > source.sourceFrames)
		{
			result.code = QStringLiteral("clip_state_mismatch");
			return result;
		}
		if (!supportedRate(source.sampleRate))
		{
			result.code = QStringLiteral("unsupported_sample_rate");
			return result;
		}
		if (source.sampleRate != snapshot.world.outputRate || (source.channels != 1 && source.channels != 2))
		{
			result.code = QStringLiteral("unsupported_playback_world");
			return result;
		}
		if (commonChannels && commonChannels != source.channels)
		{
			result.code = QStringLiteral("unsupported_playback_world");
			return result;
		}
		commonChannels = source.channels;
	}
	if (totalSelectedInputFrames(request) > quint64(snapshot.world.outputRate) * VocalLimits::seconds)
	{
		result.code = QStringLiteral("resource_limit");
		return result;
	}
	const auto processingRange = renderInputRange(request);
	if (quint64(processingRange.outFrame - processingRange.inFrame)
		> quint64(snapshot.world.outputRate) * VocalLimits::seconds)
	{
		result.code = QStringLiteral("resource_limit");
		return result;
	}
	VocalEditPlan plan;
	plan.recipeHash = canonicalVocalEditRecipeHash(request);
	if (plan.recipeHash.size() != 64)
	{
		return result;
	}
	plan.projectGeneration = snapshot.generation;
	plan.worldHash = snapshot.worldHash;
	QByteArray planBytes;
	QDataStream planStream(&planBytes, QIODevice::WriteOnly);
	planStream.setVersion(QDataStream::Qt_5_15);
	planStream.setByteOrder(QDataStream::BigEndian);
	planStream << QStringLiteral("lmms-vocal-edit-plan-v1") << plan.recipeHash << snapshot.generation
		<< snapshot.worldHash << snapshot.world.tempoBpm << snapshot.world.outputRate
		<< snapshot.world.ticksPerQuarterNote << snapshot.world.tempoAutomationPresent
		<< snapshot.world.timeSignatureNumerator << snapshot.world.timeSignatureDenominator
		<< quint8(plan.roundingPolicy);
	for (const auto& source : snapshot.sources)
	{
		planStream << source.clipId << source.audioStateHash << source.sourceFrames
			<< source.sampleRate << source.channels << source.sourceVerified;
	}
	plan.planHash = finishHash(planBytes);

	if (const auto* map = timingMap(request))
	{
		constexpr size_t previewLimit = 128;
		std::vector<VocalEditAnchorPreview> mapped;
		mapped.reserve(map->anchors.size());
		for (const auto& anchor : map->anchors)
		{
			VocalEditAnchorPreview preview;
			preview.renderInputFrame = anchor.renderInputFrame;
			preview.targetTick = anchor.targetTick;
			if (anchor.outputFrame)
			{
				preview.outputFrame = *anchor.outputFrame;
			}
			else if (!tickToFrame(*anchor.targetTick, snapshot.world, preview.outputFrame,
					preview.roundingErrorNumerator, preview.roundingErrorDenominator,
					preview.roundingErrorFrames))
			{
				result.code = QStringLiteral("resource_limit");
				return result;
			}
			if (preview.outputFrame > qint64(snapshot.world.outputRate) * qint64(VocalLimits::seconds))
			{
				result.code = QStringLiteral("resource_limit");
				return result;
			}
			if (std::abs(preview.roundingErrorFrames) > limits.maximumAnchorErrorFrames)
			{
				result.code = QStringLiteral("timing_anchor_out_of_tolerance");
				return result;
			}
			if (!mapped.empty())
			{
				const auto& previous = mapped.back();
				if (preview.outputFrame <= previous.outputFrame)
				{
					result.code = QStringLiteral("invalid_parameter");
					return result;
				}
				const auto sourceDelta = anchor.renderInputFrame - previous.renderInputFrame;
				const auto outputDelta = preview.outputFrame - previous.outputFrame;
				const double ratio = static_cast<double>(outputDelta) / static_cast<double>(sourceDelta);
				if (!std::isfinite(ratio) || ratio < limits.minimumLocalStretchRatio
					|| ratio > limits.maximumLocalStretchRatio)
				{
					result.code = QStringLiteral("unsupported_stretch_ratio");
					return result;
				}
			}
			mapped.push_back(preview);
		}
		if (mapped.size() > previewLimit)
		{
			plan.anchorPreviews.insert(plan.anchorPreviews.end(), mapped.begin(), mapped.begin() + previewLimit);
			plan.anchorPreviewTruncated = true;
			plan.omittedAnchorCount += mapped.size() - previewLimit;
		}
		else
		{
			plan.anchorPreviews.insert(plan.anchorPreviews.end(), mapped.begin(), mapped.end());
		}
	}
	result.code = QStringLiteral("ok");
	result.value = std::move(plan);
	return result;
}

} // namespace lmms
