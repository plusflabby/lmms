#include "AiBridgeVocalEditPlanner.h"

#include "AiBridgeVocalDspBackend.h"
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

QString vocalEditWorldHash(const VocalEditWorld& world)
{
	if (world.tempoBpm < 10 || world.tempoBpm > 999 || !supportedRate(world.outputRate)
		|| world.ticksPerQuarterNote != 48 || world.timeSignatureNumerator < 1
		|| world.timeSignatureNumerator > 32 || world.timeSignatureDenominator < 1
		|| world.timeSignatureDenominator > 32)
	{
		return {};
	}
	QByteArray bytes;
	QDataStream stream(&bytes, QIODevice::WriteOnly);
	stream.setVersion(QDataStream::Qt_5_15);
	stream.setByteOrder(QDataStream::BigEndian);
	stream << QStringLiteral("lmms-vocal-edit-world-v1") << world.tempoBpm << world.outputRate
		<< world.ticksPerQuarterNote << world.timeSignatureNumerator
		<< world.timeSignatureDenominator << world.tempoAutomationPresent;
	return finishHash(bytes);
}

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
	if ((snapshot.world.tempoAutomationPresent && timingMap(request)) || snapshot.world.tempoBpm <= 0
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
	const bool compInput = std::holds_alternative<VocalCompRecipe>(request.input);
	const bool hasTiming = timingMap(request) != nullptr;
	const bool hasGainFade = std::any_of(request.operations.begin(), request.operations.end(),
		[](const VocalEditOperation& operation) {
			return !std::holds_alternative<VocalTimingMap>(operation);
		});
	if (hasTiming)
	{
		const auto stretchRenderer = QStringLiteral("lmms-")
			+ QString::fromLatin1(AiBridgeVocalDspBackend::RendererId);
		plan.rendererId = compInput
			? hasGainFade ? QStringLiteral("lmms-native-comp-gain-fade-")
					+ QString::fromLatin1(AiBridgeVocalDspBackend::RendererId)
				: QStringLiteral("lmms-native-comp-") + QString::fromLatin1(AiBridgeVocalDspBackend::RendererId)
			: hasGainFade ? QStringLiteral("lmms-gain-fade-")
					+ QString::fromLatin1(AiBridgeVocalDspBackend::RendererId)
				: stretchRenderer;
		plan.rendererVersion = QString::fromLatin1(AiBridgeVocalDspBackend::RendererVersion);
	}
	else if (compInput)
	{
		plan.rendererId = QStringLiteral("lmms-native-comp-gain-fade");
		plan.rendererVersion = QStringLiteral("1");
	}
	else
	{
		plan.rendererId = QStringLiteral("lmms-native-gain-fade");
		plan.rendererVersion = QStringLiteral("1");
	}
	QByteArray planBytes;
	QDataStream planStream(&planBytes, QIODevice::WriteOnly);
	planStream.setVersion(QDataStream::Qt_5_15);
	planStream.setByteOrder(QDataStream::BigEndian);
	planStream << QStringLiteral("lmms-vocal-edit-plan-v2") << plan.recipeHash << plan.rendererId
		<< plan.rendererVersion << snapshot.generation
		<< snapshot.worldHash << snapshot.world.tempoBpm << snapshot.world.outputRate
		<< snapshot.world.ticksPerQuarterNote << snapshot.world.tempoAutomationPresent
		<< snapshot.world.timeSignatureNumerator << snapshot.world.timeSignatureDenominator
		<< quint8(plan.roundingPolicy);
	for (const auto& source : snapshot.sources)
	{
		planStream << source.clipId << source.audioStateHash << source.sourceFrames
			<< source.sampleRate << source.channels << source.sourceVerified;
	}
	if (const auto* map = timingMap(request))
	{
		constexpr size_t previewLimit = 128;
		const auto domain = renderInputRange(request);
		const auto maximumFrames = qint64(snapshot.world.outputRate) * qint64(VocalLimits::seconds);
		std::vector<VocalEditAnchorPreview> mapped;
		mapped.reserve(map->anchors.size() + map->protectedRenderInputFrames.size() * 2);
		for (const auto& anchor : map->anchors)
		{
			VocalEditAnchorPreview preview;
			preview.renderInputFrame = anchor.renderInputFrame;
			preview.targetTick = anchor.targetTick;
			if (anchor.outputFrame)
			{
				preview.requestedOutputFrame = *anchor.outputFrame;
			}
			else if (!tickToFrame(*anchor.targetTick, snapshot.world, preview.outputFrame,
					preview.roundingErrorNumerator, preview.roundingErrorDenominator,
					preview.roundingErrorFrames))
			{
				result.code = QStringLiteral("resource_limit");
				return result;
			}
			else
			{
				preview.requestedOutputFrame = preview.outputFrame;
			}
			if (preview.requestedOutputFrame > maximumFrames)
			{
				result.code = QStringLiteral("resource_limit");
				return result;
			}
			if (std::abs(preview.roundingErrorFrames) > limits.maximumAnchorErrorFrames)
			{
				result.code = QStringLiteral("timing_anchor_out_of_tolerance");
				return result;
			}
			const auto identityFrame = anchor.renderInputFrame - domain.inFrame;
			const auto requestedDelta = preview.requestedOutputFrame - identityFrame;
			const auto strengthOutput = double(identityFrame) + double(requestedDelta) * map->strength;
			if (!std::isfinite(strengthOutput) || strengthOutput < 0.0
				|| strengthOutput > double(maximumFrames))
			{
				result.code = QStringLiteral("resource_limit");
				return result;
			}
			preview.outputFrame = qint64(std::floor(strengthOutput + 0.5));
			preview.strengthRoundingErrorFrames = strengthOutput - double(preview.outputFrame);
			preview.preAnchorCrossfadeFrames = anchor.preAnchorCrossfadeFrames;
			preview.crossfadeLaw = anchor.crossfadeLaw;
			mapped.push_back(preview);
		}
		if (mapped.front().requestedOutputFrame != 0 || mapped.front().outputFrame != 0)
		{
			result.code = QStringLiteral("invalid_parameter");
			return result;
		}

		auto makeProtectedBoundary = [&](qint64 inputFrame) -> bool {
			const auto outputFrame = inputFrame - domain.inFrame;
			auto found = std::lower_bound(mapped.begin(), mapped.end(), inputFrame,
				[](const VocalEditAnchorPreview& candidate, qint64 frame) {
					return candidate.renderInputFrame < frame;
				});
			if (found != mapped.end() && found->renderInputFrame == inputFrame)
			{
				return found->outputFrame == outputFrame;
			}
			VocalEditAnchorPreview synthetic;
			synthetic.renderInputFrame = inputFrame;
			synthetic.outputFrame = outputFrame;
			synthetic.requestedOutputFrame = outputFrame;
			synthetic.syntheticProtectedBoundary = true;
			mapped.insert(found, synthetic);
			return true;
		};
		for (const auto& range : map->protectedRenderInputFrames)
		{
			if (!makeProtectedBoundary(range.inFrame) || !makeProtectedBoundary(range.outFrame))
			{
				result.code = QStringLiteral("protected_range_not_preserved");
				return result;
			}
		}

		for (size_t i = 0; i + 1 < mapped.size(); ++i)
		{
			auto& anchor = mapped[i];
			const auto& next = mapped[i + 1];
			for (const auto& range : map->protectedRenderInputFrames)
			{
				const bool overlaps = anchor.renderInputFrame < range.outFrame
					&& next.renderInputFrame > range.inFrame;
				if (overlaps)
				{
					if (anchor.renderInputFrame < range.inFrame || next.renderInputFrame > range.outFrame
						|| next.outputFrame - anchor.outputFrame != next.renderInputFrame - anchor.renderInputFrame)
					{
						result.code = QStringLiteral("protected_range_not_preserved");
						return result;
					}
					anchor.copyProtectedInterval = true;
				}
			}
			if (anchor.copyProtectedInterval && next.preAnchorCrossfadeFrames > 0)
			{
				result.code = QStringLiteral("protected_range_not_preserved");
				return result;
			}
		}

		for (size_t i = 1; i + 1 < mapped.size(); ++i)
		{
			const auto& previous = mapped[i - 1];
			const auto& current = mapped[i];
			const auto& next = mapped[i + 1];
			if (current.preAnchorCrossfadeFrames > limits.maxCrossfadeFrames)
			{
				result.code = QStringLiteral("resource_limit");
				return result;
			}
			if (current.preAnchorCrossfadeFrames > (current.outputFrame - previous.outputFrame) / 2
				|| current.preAnchorCrossfadeFrames > (next.outputFrame - current.outputFrame) / 2
				|| (mapped[i - 1].copyProtectedInterval && current.preAnchorCrossfadeFrames > 0))
			{
				result.code = mapped[i - 1].copyProtectedInterval && current.preAnchorCrossfadeFrames > 0
					? QStringLiteral("protected_range_not_preserved") : QStringLiteral("invalid_parameter");
				return result;
			}
		}
		for (size_t i = 1; i < mapped.size(); ++i)
		{
			const auto& previous = mapped[i - 1];
			const auto& current = mapped[i];
			if (current.outputFrame <= previous.outputFrame)
			{
				result.code = QStringLiteral("invalid_parameter");
				return result;
			}
			const auto sourceDelta = current.renderInputFrame - previous.renderInputFrame;
			const auto outputDelta = current.outputFrame - previous.outputFrame;
			const double ratio = static_cast<double>(outputDelta) / static_cast<double>(sourceDelta);
			if (!std::isfinite(ratio) || ratio < map->minimumLocalStretchRatio
				|| ratio > map->maximumLocalStretchRatio)
			{
				result.code = QStringLiteral("unsupported_stretch_ratio");
				return result;
			}
			if (current.outputFrame > maximumFrames)
			{
				result.code = QStringLiteral("resource_limit");
				return result;
			}
		}
		plan.resolvedAnchors = mapped;
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
	planStream << quint32(plan.resolvedAnchors.size());
	for (const auto& anchor : plan.resolvedAnchors)
	{
		planStream << anchor.renderInputFrame << anchor.requestedOutputFrame << anchor.outputFrame
			<< bool(anchor.targetTick) << anchor.targetTick.value_or(0)
			<< anchor.roundingErrorNumerator << anchor.roundingErrorDenominator
			<< anchor.strengthRoundingErrorFrames << anchor.preAnchorCrossfadeFrames
			<< quint8(anchor.crossfadeLaw) << anchor.copyProtectedInterval
			<< anchor.syntheticProtectedBoundary;
	}
	plan.planHash = finishHash(planBytes);
	result.code = QStringLiteral("ok");
	result.value = std::move(plan);
	return result;
}

} // namespace lmms
