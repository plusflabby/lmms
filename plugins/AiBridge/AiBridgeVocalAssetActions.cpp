#include "AiBridgeVocalAssetActions.h"

#include "AiBridgeVocalContext.h"
#include "AiBridgeVocalComp.h"
#include "AiBridgeVocalDspBackend.h"
#include "AiBridgeVocalGainFade.h"
#include "AiBridgeVocalSource.h"
#include "Engine.h"
#include "src/core/vocal/ManagedVocalAssetRoot.h"
#include "src/core/vocal/ManagedVocalAssetStore.h"
#include "src/core/vocal/ManagedVocalProjectOwnerStore.h"
#include "RecordingProjectIdentity.h"
#include "Song.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <type_traits>

namespace lmms {
namespace {

constexpr size_t VocalRenderBlockFrames = 4096;
constexpr quint64 FixedRenderMetadataCharge = 64 * 1024;
constexpr quint64 RenderScratchCharge = 256 * 1024;

bool checkedAdd(quint64& target, quint64 value)
{
	if (value > std::numeric_limits<quint64>::max() - target) return false;
	target += value;
	return true;
}

bool checkedMultiply(quint64 left, quint64 right, quint64& result)
{
	if (left && right > std::numeric_limits<quint64>::max() / left) return false;
	result = left * right;
	return true;
}

bool validUuid(const QString& value)
{
	const auto parsed = QUuid(value);
	return !parsed.isNull() && parsed.toString(QUuid::WithoutBraces) == value;
}

const VocalCompBoundary* boundaryFor(const VocalCompRecipe& recipe, size_t leftIndex, size_t rightIndex)
{
	for (const auto& boundary : recipe.boundaries)
	{
		if (boundary.leftSegmentIndex == leftIndex && boundary.rightSegmentIndex == rightIndex) return &boundary;
	}
	return nullptr;
}

bool sameAnchorPreview(const VocalEditAnchorPreview& left, const VocalEditAnchorPreview& right)
{
	return left.renderInputFrame == right.renderInputFrame && left.outputFrame == right.outputFrame
		&& left.requestedOutputFrame == right.requestedOutputFrame && left.targetTick == right.targetTick
		&& left.roundingErrorFrames == right.roundingErrorFrames
		&& left.roundingErrorNumerator == right.roundingErrorNumerator
		&& left.roundingErrorDenominator == right.roundingErrorDenominator
		&& left.strengthRoundingErrorFrames == right.strengthRoundingErrorFrames
		&& left.preAnchorCrossfadeFrames == right.preAnchorCrossfadeFrames
		&& left.crossfadeLaw == right.crossfadeLaw
		&& left.copyProtectedInterval == right.copyProtectedInterval
		&& left.syntheticProtectedBoundary == right.syntheticProtectedBoundary;
}

bool sameResolvedPlan(const VocalEditPlan& left, const VocalEditPlan& right)
{
	if (left.rendererId != right.rendererId || left.rendererVersion != right.rendererVersion
		|| left.resolvedAnchors.size() != right.resolvedAnchors.size()
		|| left.anchorPreviews.size() != right.anchorPreviews.size()
		|| left.anchorPreviewTruncated != right.anchorPreviewTruncated
		|| left.omittedAnchorCount != right.omittedAnchorCount)
	{
		return false;
	}
	for (size_t i = 0; i < left.resolvedAnchors.size(); ++i)
		if (!sameAnchorPreview(left.resolvedAnchors[i], right.resolvedAnchors[i])) return false;
	for (size_t i = 0; i < left.anchorPreviews.size(); ++i)
		if (!sameAnchorPreview(left.anchorPreviews[i], right.anchorPreviews[i])) return false;
	return true;
}

VocalEditWorld currentWorld()
{
	auto* song = Engine::getSong();
	return {song->getTempo(), int(Engine::audioEngine()->outputSampleRate()),
		DefaultTicksPerBar / 4, song->tempoModel().isAutomatedOrControlled(),
		song->getTimeSigModel().getNumerator(), song->getTimeSigModel().getDenominator()};
}

VocalAnalyzeRequest analysisRequest(const VocalEditRequest& request)
{
	VocalAnalyzeRequest output;
	output.selections.reserve(request.selections.size());
	for (const auto& selection : request.selections)
	{
		output.selections.push_back({selection.clipId, selection.expectedAudioHash,
			{selection.sourceFrames.inFrame, selection.sourceFrames.outFrame}});
	}
	return output;
}

VocalEditPlanningSnapshot planningSnapshot(
	const VocalEditRequest& request,
	const VocalInputSnapshot& inputs,
	const VocalEditWorld& world)
{
	VocalEditPlanningSnapshot snapshot;
	snapshot.generation = inputs.generation;
	snapshot.worldHash = vocalEditWorldHash(world);
	snapshot.world = world;
	snapshot.sources.reserve(inputs.sources.size());
	for (size_t i = 0; i < inputs.sources.size(); ++i)
	{
		const auto& ticket = inputs.sources[i];
	const auto& selection = request.selections[i];
		const bool verified = ticket.expectedInfo.frames > 0 && ticket.expectedInfo.sourceSha256.size() == 32
			&& !ticket.expectedIdentity.isEmpty()
			&& (ticket.sourceStatus == QStringLiteral("available")
				|| ticket.sourceStatus == QStringLiteral("available_unloaded"));
		snapshot.sources.push_back({selection.clipId, selection.expectedAudioHash,
			ticket.expectedInfo.frames, ticket.expectedInfo.sampleRate, ticket.expectedInfo.channels, verified});
	}
	return snapshot;
}

QString validateTicket(const VocalEditPlanTicket& ticket)
{
	if (ticket.sources.generation != ticket.plan.projectGeneration
		|| ticket.sources.inputHash.isEmpty()
		|| vocalInputHash(ticket.sources) != ticket.sources.inputHash
		|| ticket.planning.generation != ticket.plan.projectGeneration
		|| ticket.planning.worldHash != ticket.plan.worldHash
		|| ticket.plan.recipeHash != canonicalVocalEditRecipeHash(ticket.request)
		|| ticket.plan.planHash.isEmpty())
	{
		return QStringLiteral("plan_stale");
	}
	const auto replanned = AiBridgeVocalEditPlanner::plan(ticket.request, ticket.planning, ticket.limits);
	if (replanned.code != QStringLiteral("ok") || !replanned.value
		|| replanned.value->planHash != ticket.plan.planHash
		|| replanned.value->recipeHash != ticket.plan.recipeHash
		|| !sameResolvedPlan(*replanned.value, ticket.plan))
	{
		return QStringLiteral("plan_stale");
	}
	if (ticket.sources.sources.size() != ticket.request.selections.size()
		|| ticket.planning.sources.size() != ticket.request.selections.size())
	{
		return QStringLiteral("plan_stale");
	}
	for (size_t i = 0; i < ticket.request.selections.size(); ++i)
	{
		const auto& selection = ticket.request.selections[i];
		const auto& sourceTicket = ticket.sources.sources[i];
		const auto& sourceSnapshot = ticket.planning.sources[i];
		if (sourceTicket.clipId != selection.clipId || sourceSnapshot.clipId != selection.clipId
			|| sourceSnapshot.audioStateHash != selection.expectedAudioHash
			|| sourceTicket.expectedInfo.sourceSha256.size() != 32
			|| sourceTicket.expectedIdentity.isEmpty()
			|| sourceTicket.expectedInfo.frames != sourceSnapshot.sourceFrames
			|| sourceTicket.expectedInfo.sampleRate != sourceSnapshot.sampleRate
			|| sourceTicket.expectedInfo.channels != sourceSnapshot.channels)
		{
			return QStringLiteral("clip_state_mismatch");
		}
	}
	return QStringLiteral("ok");
}

QJsonObject frameRangeJson(const VocalEditFrameRange& range)
{
	return {{QStringLiteral("in_frame"), QString::number(range.inFrame)},
		{QStringLiteral("out_frame"), QString::number(range.outFrame)}};
}

QJsonObject recipeJson(const VocalEditRequest& request)
{
	QJsonArray selections;
	for (const auto& selection : request.selections)
	{
		selections.append(QJsonObject{{QStringLiteral("clip_id"), selection.clipId},
			{QStringLiteral("expected_audio_state_hash"), selection.expectedAudioHash},
			{QStringLiteral("source_basis"), QStringLiteral("source_pcm")},
			{QStringLiteral("source_frames"), frameRangeJson(selection.sourceFrames)}});
	}
	QJsonArray operations;
	for (const auto& operation : request.operations)
	{
		operations.append(std::visit([](const auto& value) -> QJsonObject {
			using T = std::decay_t<decltype(value)>;
			if constexpr (std::is_same_v<T, VocalGainEnvelope>)
			{
				QJsonArray points;
				for (const auto& point : value.points)
				{
					points.append(QJsonObject{{QStringLiteral("render_input_frame"), QString::number(point.renderInputFrame)},
						{QStringLiteral("gain_db"), point.gainDb}});
				}
				return {{QStringLiteral("kind"), QStringLiteral("gain_envelope")},
					{QStringLiteral("points"), points}};
			}
			else if constexpr (std::is_same_v<T, VocalFade>)
			{
				const auto curve = value.curve == VocalFadeCurve::LinearAmplitude
					? QStringLiteral("linear_amplitude") : QStringLiteral("equal_power");
				return {{QStringLiteral("kind"), QStringLiteral("fade")},
					{QStringLiteral("render_input_frames"), frameRangeJson(value.renderInputFrames)},
					{QStringLiteral("start_gain_db"), value.startGainDb},
					{QStringLiteral("end_gain_db"), value.endGainDb},
					{QStringLiteral("curve"), curve}};
			}
			else if constexpr (std::is_same_v<T, VocalTimingMap>)
			{
				QJsonArray anchors;
				for (const auto& anchor : value.anchors)
				{
					QJsonObject row{{QStringLiteral("render_input_frame"), QString::number(anchor.renderInputFrame)},
						{QStringLiteral("pre_anchor_crossfade_frames"), QString::number(anchor.preAnchorCrossfadeFrames)},
						{QStringLiteral("crossfade_law"), anchor.crossfadeLaw == VocalCrossfadeLaw::LinearAmplitude
							? QStringLiteral("linear_amplitude") : QStringLiteral("equal_power")}};
					if (anchor.outputFrame)
						row.insert(QStringLiteral("target_output_frame"), QString::number(*anchor.outputFrame));
					else
						row.insert(QStringLiteral("target_tick"), QString::number(*anchor.targetTick));
					anchors.append(row);
				}
				QJsonArray protectedRanges;
				for (const auto& range : value.protectedRenderInputFrames)
				{
					protectedRanges.append(frameRangeJson(range));
				}
				return {{QStringLiteral("kind"), QStringLiteral("timing_map")},
					{QStringLiteral("strength"), value.strength},
					{QStringLiteral("minimum_local_stretch_ratio"), value.minimumLocalStretchRatio},
					{QStringLiteral("maximum_local_stretch_ratio"), value.maximumLocalStretchRatio},
					{QStringLiteral("anchors"), anchors},
					{QStringLiteral("protected_render_input_frames"), protectedRanges}};
			}
			else
			{
				return {};
			}
		}, operation));
	}
	QJsonObject recipe{{QStringLiteral("recipe_version"), int(request.recipeVersion)},
		{QStringLiteral("expected_project_generation"), QString::number(request.expectedProjectGeneration)},
		{QStringLiteral("expected_world_hash"), request.expectedWorldHash},
		{QStringLiteral("selections"), selections}, {QStringLiteral("operations"), operations}};
	if (const auto* direct = std::get_if<VocalDirectInput>(&request.input))
	{
		recipe.insert(QStringLiteral("input_kind"), QStringLiteral("direct_source"));
		recipe.insert(QStringLiteral("input_selection_index"), int(direct->selectionIndex));
	}
	else
	{
		const auto& comp = std::get<VocalCompRecipe>(request.input);
		QJsonArray segments;
		for (const auto& segment : comp.segments)
		{
			segments.append(QJsonObject{{QStringLiteral("selection_index"), int(segment.selectionIndex)},
				{QStringLiteral("source_frames"), frameRangeJson(segment.sourceFrames)},
				{QStringLiteral("output_frames"), frameRangeJson(segment.outputFrames)}});
		}
		QJsonArray boundaries;
		for (const auto& boundary : comp.boundaries)
		{
			boundaries.append(QJsonObject{{QStringLiteral("left_segment_index"), int(boundary.leftSegmentIndex)},
				{QStringLiteral("right_segment_index"), int(boundary.rightSegmentIndex)},
				{QStringLiteral("overlap_frames"), QString::number(boundary.overlapFrames)},
				{QStringLiteral("law"), boundary.law == VocalCrossfadeLaw::LinearAmplitude
					? QStringLiteral("linear_amplitude") : QStringLiteral("equal_power")}});
		}
		recipe.insert(QStringLiteral("input_kind"), QStringLiteral("comp"));
		recipe.insert(QStringLiteral("comp"), QJsonObject{{QStringLiteral("segments"), segments},
			{QStringLiteral("boundaries"), boundaries}});
	}
	return recipe;
}

} // namespace

VocalEditWorld captureVocalEditWorld()
{
	if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread()) return {};
	return currentWorld();
}

VocalEditPlanCaptureResult AiBridgeVocalAssetActions::plan(
	const VocalEditRequest& request,
	const VocalEditLimits& limits)
{
	if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread())
	{
		return {QStringLiteral("wrong_thread"), {}};
	}
	if (const auto code = validateVocalEditRequest(request, limits); code != QStringLiteral("ok"))
	{
		return {code, {}};
	}
	const auto captured = captureVocalInputs(analysisRequest(request));
	if (!captured.value)
	{
		return {captured.code, {}};
	}
	const auto world = currentWorld();
	const auto snapshot = planningSnapshot(request, *captured.value, world);
	const auto planned = AiBridgeVocalEditPlanner::plan(request, snapshot, limits);
	if (planned.code != QStringLiteral("ok") || !planned.value)
	{
		return {planned.code, {}};
	}
	return {QStringLiteral("ok"), VocalEditPlanTicket{
		request, snapshot, *planned.value, *captured.value, limits}};
}

ManagedVocalAssetInspectResult AiBridgeVocalAssetActions::inspectVariant(const QString& assetId)
{
	if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread())
		return {QStringLiteral("wrong_thread")};
	auto* song = Engine::getSong();
	if (!song) return {QStringLiteral("project_identity_unverified")};
	const auto independent = song->managedVocalOwner();
	const auto identity = song->recordingProjectIdentity().cachedIdentity();
	const auto projectId = independent ? independent->projectId : identity.id;
	if (!validUuid(projectId)) return {QStringLiteral("project_identity_unverified")};
	auto approval = ManagedVocalAssetRoot::authorizeConfiguredRoot(independent
		? song->recordingProjectIdentity().managedRootLibraryForOwner() : song->recordingProjectIdentity().managedRootLibrary());
	if (!approval.root) return {approval.code};
	std::shared_ptr<void> projectHold;
	if (independent)
	{
		if (approval.root->rootId() != independent->rootId) return {QStringLiteral("project_identity_unverified")};
		ManagedVocalProjectOwnerStore owners(approval.root);
		const auto held = owners.holdVerified(song->projectFileName(), *independent);
		if (!held.hold) return {held.code};
		projectHold = held.hold;
	}
	ManagedVocalAssetStore store(std::move(approval.root));
	return store.inspectById(assetId, projectId);
}

VocalEditRenderInputResult AiBridgeVocalAssetActions::prepareRender(const VocalEditPlanTicket& ticket)
{
	if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread())
	{
		return {QStringLiteral("wrong_thread"), {}};
	}
	if (const auto code = validateTicket(ticket); code != QStringLiteral("ok"))
	{
		return {code, {}};
	}
	if (const auto code = validateVocalSnapshot(ticket.sources); code != QStringLiteral("ok"))
	{
		return {code, {}};
	}
	const auto world = currentWorld();
	if (vocalEditWorldHash(world) != ticket.plan.worldHash)
	{
		return {QStringLiteral("plan_stale"), {}};
	}
	auto* song = Engine::getSong();
	if (!song || song->projectFileName().isEmpty())
	{
		return {QStringLiteral("project_identity_unverified"), {}};
	}
	const auto identity = song->recordingProjectIdentity().cachedIdentity();
	const auto independent = song->managedVocalOwner();
	if (!independent && !identity.id.isEmpty()
		&& (!validUuid(identity.id) || !song->recordingProjectIdentity().sourceIdentityTrusted()))
	{
		return {QStringLiteral("project_identity_unverified"), {}};
	}
	auto approval = ManagedVocalAssetRoot::authorizeConfiguredRoot(independent
		? song->recordingProjectIdentity().managedRootLibraryForOwner() : song->recordingProjectIdentity().managedRootLibrary());
	if (!approval.root)
	{
		return {approval.code, {}};
	}
	if (independent || identity.id.isEmpty())
	{
		if (!independent && song->savedProjectHasRecordingIdentity())
			return {QStringLiteral("project_identity_unverified"), {}};
		ManagedVocalProjectOwnerStore store(approval.root);
		const auto owner = store.ensureLoadedSnapshot(song->projectFileName(), song->projectSourceFileId(), song->projectSourceSha256());
		if (!owner.owner) return {owner.code, {}};
		if (independent && (independent->projectId != owner.owner->projectId || independent->rootId != owner.owner->rootId))
			return {QStringLiteral("project_identity_unverified"), {}};
		const auto held = store.holdSaveAsSource(song->projectFileName(), *owner.owner);
		if (!held.source) return {held.code, {}};
		if (!song->adoptManagedVocalOwner(held.source)) return {QStringLiteral("project_identity_unverified"), {}};
		return {QStringLiteral("ok"), VocalEditRenderInput{ticket, std::move(approval.root), owner.owner->projectId, held.source}};
	}
	return {QStringLiteral("ok"), VocalEditRenderInput{ticket, std::move(approval.root), identity.id}};
}

VocalRenderBudget AiBridgeVocalAssetActions::estimate(const VocalEditPlanTicket& ticket)
{
	VocalRenderBudget result;
	result.code = validateTicket(ticket);
	if (result.code != QStringLiteral("ok")) return result;
	const VocalTimingMap* timingMap = nullptr;
	for (const auto& operation : ticket.request.operations)
		if (const auto* map = std::get_if<VocalTimingMap>(&operation)) timingMap = map;
	const auto sourceBudget = estimateVocalBudget(ticket.sources);
	if (sourceBudget.code != QStringLiteral("ok"))
	{
		result.code = sourceBudget.code;
		return result;
	}
	if (ticket.request.selections.size() != ticket.planning.sources.size()
		|| ticket.request.selections.empty())
	{
		result.code = QStringLiteral("plan_stale");
		return result;
	}
	result.sampleRate = ticket.planning.world.outputRate;
	result.channels = ticket.planning.sources.front().channels;
	for (size_t i = 0; i < ticket.request.selections.size(); ++i)
	{
		const auto& selection = ticket.request.selections[i];
		const auto& source = ticket.planning.sources[i];
		if (selection.basis != VocalEditSourceBasis::SourcePcm || !source.sourceVerified
			|| source.sampleRate != result.sampleRate || source.channels != result.channels
			|| (source.channels != 1 && source.channels != 2))
		{
			result.code = QStringLiteral("clip_state_mismatch");
			return result;
		}
	}
	quint64 compInputFrames = 0;
	quint64 renderInputFrames = 0;
	if (const auto* direct = std::get_if<VocalDirectInput>(&ticket.request.input))
	{
		if (direct->selectionIndex != 0 || ticket.request.selections.size() != 1)
		{
			result.code = QStringLiteral("invalid_parameter");
			return result;
		}
		const auto& selection = ticket.request.selections.front();
		result.outputFrames = quint64(selection.sourceFrames.outFrame - selection.sourceFrames.inFrame);
		renderInputFrames = result.outputFrames;
	}
	else
	{
		const auto& comp = std::get<VocalCompRecipe>(ticket.request.input);
		for (const auto& selection : ticket.request.selections)
		{
			const auto frames = quint64(selection.sourceFrames.outFrame - selection.sourceFrames.inFrame);
			if (!checkedAdd(compInputFrames, frames))
			{
				result.code = QStringLiteral("resource_limit");
				return result;
			}
		}
		for (const auto& segment : comp.segments)
		{
			result.outputFrames = std::max(result.outputFrames, quint64(segment.outputFrames.outFrame));
		}
		renderInputFrames = result.outputFrames;
	}
	result.renderInputFrames = renderInputFrames;
	quint64 timingInputBytes = 0;
	quint64 timingSegmentFrames = 0;
	quint64 timingSegmentBytes = 0;
	quint64 timingWorkspaceBytes = 0;
	if (timingMap)
	{
		if (ticket.plan.resolvedAnchors.size() < 2
			|| ticket.plan.resolvedAnchors.front().renderInputFrame !=
				(std::holds_alternative<VocalDirectInput>(ticket.request.input)
					? ticket.request.selections.front().sourceFrames.inFrame : 0)
			|| ticket.plan.resolvedAnchors.back().outputFrame <= 0)
		{
			result.code = QStringLiteral("plan_stale");
			return result;
		}
		result.outputFrames = quint64(ticket.plan.resolvedAnchors.back().outputFrame);
		if (!checkedMultiply(renderInputFrames, quint64(sizeof(SampleFrame)), timingInputBytes))
		{
			result.code = QStringLiteral("resource_limit");
			return result;
		}
		for (size_t i = 0; i + 1 < ticket.plan.resolvedAnchors.size(); ++i)
		{
			const auto& anchor = ticket.plan.resolvedAnchors[i];
			const auto& next = ticket.plan.resolvedAnchors[i + 1];
			const auto inputDelta = next.renderInputFrame - anchor.renderInputFrame;
			const auto outputDelta = next.outputFrame - anchor.outputFrame;
			const auto baseRatio = double(outputDelta) / double(inputDelta);
			const auto inputOverlap = anchor.preAnchorCrossfadeFrames > 0
				? qint64(std::llround(double(anchor.preAnchorCrossfadeFrames) / baseRatio)) : 0;
			const auto segmentInputFrames = inputDelta + inputOverlap;
			const auto segmentOutputFrames = outputDelta + anchor.preAnchorCrossfadeFrames;
			if (!anchor.copyProtectedInterval && segmentInputFrames != segmentOutputFrames)
			{
				const auto requiredInput = AiBridgeVocalDspBackend::minimumInputFrames(
					result.sampleRate, result.channels, double(segmentOutputFrames) / double(segmentInputFrames));
				if (requiredInput < 0)
				{
					result.code = QStringLiteral("stretch_backend_unavailable");
					return result;
				}
				if (segmentInputFrames < requiredInput)
				{
					result.code = QStringLiteral("stretch_segment_too_short");
					return result;
				}
			}
			const auto segmentFrames = quint64(segmentOutputFrames);
			if (!checkedAdd(timingSegmentFrames, segmentFrames))
			{
				result.code = QStringLiteral("resource_limit");
				return result;
			}
		}
		if (!checkedMultiply(timingSegmentFrames, quint64(sizeof(SampleFrame)), timingSegmentBytes)
			|| timingSegmentBytes > VocalLimits::jobBytes)
		{
			result.code = QStringLiteral("resource_limit");
			return result;
		}
		timingWorkspaceBytes = timingSegmentBytes;
		if (!checkedAdd(timingWorkspaceBytes, AiBridgeVocalDspBackend::FixedWorkspaceChargeBytes))
		{
			result.code = QStringLiteral("resource_limit");
			return result;
		}
	}
	if (result.outputFrames > quint64(result.sampleRate) * VocalLimits::seconds)
	{
		result.code = QStringLiteral("resource_limit");
		return result;
	}
	quint64 samples = 0;
	if (!checkedMultiply(result.outputFrames, quint64(result.channels), samples)
		|| !checkedMultiply(samples, quint64(sizeof(float)), result.stagedBytes)
		|| !checkedAdd(result.stagedBytes, 56)
		|| result.stagedBytes > VocalLimits::stagedOutputBytes)
	{
		result.code = QStringLiteral("resource_limit");
		return result;
	}
	result.sessionBytes = sourceBudget.sessionBytes;
	if (!checkedAdd(result.sessionBytes, FixedRenderMetadataCharge))
	{
		result.code = QStringLiteral("resource_limit");
		return result;
	}
	quint64 blockSampleBytes = 0;
	if (!checkedMultiply(quint64(VocalRenderBlockFrames), quint64(sizeof(SampleFrame)), blockSampleBytes))
	{
		result.code = QStringLiteral("resource_limit");
		return result;
	}
	quint64 blockOutputBytes = 0;
	if (!checkedMultiply(quint64(VocalRenderBlockFrames) * quint64(result.channels),
		quint64(sizeof(float)), blockOutputBytes))
	{
		result.code = QStringLiteral("resource_limit");
		return result;
	}
	quint64 compInputBytes = 0;
	if (!checkedMultiply(compInputFrames, quint64(sizeof(SampleFrame)), compInputBytes))
	{
		result.code = QStringLiteral("resource_limit");
		return result;
	}
	result.jobBytes = sourceBudget.jobBytes;
	if (!checkedAdd(result.jobBytes, FixedRenderMetadataCharge)
		|| !checkedAdd(result.jobBytes, RenderScratchCharge)
		|| !checkedAdd(result.jobBytes, blockSampleBytes)
		|| !checkedAdd(result.jobBytes, blockOutputBytes)
		|| !checkedAdd(result.jobBytes, compInputBytes)
		|| !checkedAdd(result.jobBytes, timingInputBytes)
		|| !checkedAdd(result.jobBytes, timingWorkspaceBytes)
		|| result.sessionBytes > VocalLimits::sessionBytes
		|| result.jobBytes > VocalLimits::jobBytes)
	{
		result.code = QStringLiteral("resource_limit");
		return result;
	}
	result.code = QStringLiteral("ok");
	return result;
}

VocalRenderBudget AiBridgeVocalAssetActions::dryRun(const VocalEditPlanTicket& ticket)
{
	return estimate(ticket);
}

VocalVariantRenderResult AiBridgeVocalAssetActions::render(
	const VocalEditRenderInput& input,
	const std::atomic_bool& cancelled,
	const VocalPhaseCallback& phase)
{
	const auto budget = estimate(input.ticket);
	if (budget.code != QStringLiteral("ok")) return {budget.code, {}, {}, 0};
	const VocalTimingMap* timingMap = nullptr;
	for (const auto& operation : input.ticket.request.operations)
		if (const auto* map = std::get_if<VocalTimingMap>(&operation)) timingMap = map;
	if (!input.assetRoot || !validUuid(input.projectId))
	{
		return {input.assetRoot ? QStringLiteral("project_identity_unverified")
			: QStringLiteral("asset_root_not_configured"), {}, {}, 0};
	}
	if (!input.assetRoot->isAuthorized()) return {QStringLiteral("asset_root_revoked"), {}, {}, 0};
	if (input.managedProjectHold && (!input.managedProjectHold->isAuthorized()
		|| input.managedProjectHold->projectOwner().projectId != input.projectId
		|| input.managedProjectHold->projectOwner().rootId != input.assetRoot->rootId()))
		return {QStringLiteral("project_identity_unverified"), {}, {}, 0};
	if (cancelled.load(std::memory_order_acquire)) return {QStringLiteral("cancelled"), {}, {}, 0};

	ManagedVocalAssetStore store(input.assetRoot, VocalLimits::stagedOutputBytes);
	if (phase) phase(VocalJobPhase::Authorizing);
	auto created = store.beginCreate(budget.outputFrames, budget.sampleRate, budget.channels);
	if (!created.writer) return {created.code, {}, {}, 0};
	std::vector<float> outputBlock;
	try
	{
		outputBlock.resize(VocalRenderBlockFrames * size_t(budget.channels));
	}
	catch (const std::bad_alloc&)
	{
		return {QStringLiteral("resource_limit"), {}, {}, 0};
	}
	if (phase) phase(VocalJobPhase::Scanning);
	QString renderFailure = QStringLiteral("ok");
	quint64 framesWritten = 0;
	quint64 samplesMeasured = 0;
	quint64 sourceSamplesMeasured = 0;
	long double sumSquares = 0.0L;
	long double sourceSumSquares = 0.0L;
	double peak = 0.0;
	double sourcePeak = 0.0;
	struct SegmentMeasurements
	{
		double peak = 0.0;
		long double sumSquares = 0.0L;
		quint64 samples = 0;
	};
	std::vector<SegmentMeasurements> segmentMeasurements;
	VocalDspMetrics dspMetrics;
	auto writeRendered = [&](std::span<const float> outputSpan) -> QString {
		if (cancelled.load(std::memory_order_acquire)) return QStringLiteral("cancelled");
		if (!budget.channels || outputSpan.size() % size_t(budget.channels) != 0)
			return QStringLiteral("invalid_parameter");
		for (const auto sample : outputSpan)
		{
			if (!std::isfinite(sample)) return QStringLiteral("invalid_audio");
			peak = std::max(peak, std::abs(double(sample)));
			sumSquares += static_cast<long double>(sample) * static_cast<long double>(sample);
			++samplesMeasured;
		}
		const auto appended = created.writer->appendFrames(outputSpan);
		if (appended != QStringLiteral("ok")) return appended;
		framesWritten += quint64(outputSpan.size() / size_t(budget.channels));
		return QStringLiteral("ok");
	};
	auto appendProcessed = [&](qint64 firstFrame, std::span<const SampleFrame> inputFrames,
		const VocalEditFrameRange& domain) -> QString {
		if (cancelled.load(std::memory_order_acquire)) return QStringLiteral("cancelled");
		const auto count = inputFrames.size();
		const auto outputSpan = std::span<float>(outputBlock.data(), count * size_t(budget.channels));
		const auto processed = AiBridgeVocalGainFade::process(domain, input.ticket.request.operations,
			firstFrame, inputFrames, budget.channels, outputSpan);
		if (processed != QStringLiteral("ok")) return processed;
		if (phase) phase(VocalJobPhase::Rendering);
		return writeRendered(outputSpan);
	};
	auto measureSource = [&](std::span<const SampleFrame> frames) {
		for (const auto& frame : frames)
		{
			const auto left = double(frame.left());
			sourcePeak = std::max(sourcePeak, std::abs(left));
			sourceSumSquares += static_cast<long double>(left) * static_cast<long double>(left);
			++sourceSamplesMeasured;
			if (budget.channels == 2)
			{
				const auto right = double(frame.right());
				sourcePeak = std::max(sourcePeak, std::abs(right));
				sourceSumSquares += static_cast<long double>(right) * static_cast<long double>(right);
				++sourceSamplesMeasured;
			}
		}
	};
	quint64 expectedSourceSamples = 0;
	std::vector<SampleFrame> renderInputBuffer;
	auto captureSelection = [&](const VocalSourceTicket& sourceTicket, const VocalEditSelection& selection,
		std::vector<SampleFrame>& target) -> QString {
		quint64 copiedFrames = 0;
		const auto scan = scanVocalSource(sourceTicket,
			[&](quint64 firstFrame, std::span<const SampleFrame> decoded) -> PcmWaveVisitDecision {
				if (cancelled.load(std::memory_order_acquire)) return PcmWaveVisitDecision::Cancelled;
				if (firstFrame > quint64(std::numeric_limits<qint64>::max())
					|| decoded.size() > size_t(std::numeric_limits<qint64>::max()))
					return PcmWaveVisitDecision::ResourceLimit;
				const auto decodedBegin = qint64(firstFrame);
				if (qint64(decoded.size()) > std::numeric_limits<qint64>::max() - decodedBegin)
					return PcmWaveVisitDecision::ResourceLimit;
				const auto decodedEnd = decodedBegin + qint64(decoded.size());
				const auto begin = std::max(decodedBegin, selection.sourceFrames.inFrame);
				const auto end = std::min(decodedEnd, selection.sourceFrames.outFrame);
				if (end <= begin) return PcmWaveVisitDecision::Continue;
				const auto sourceOffset = size_t(begin - decodedBegin);
				const auto targetOffset = size_t(begin - selection.sourceFrames.inFrame);
				const auto count = size_t(end - begin);
				if (targetOffset > target.size() || count > target.size() - targetOffset)
					return PcmWaveVisitDecision::ResourceLimit;
				std::copy_n(decoded.begin() + std::ptrdiff_t(sourceOffset), count,
					target.begin() + std::ptrdiff_t(targetOffset));
				copiedFrames += quint64(count);
				return PcmWaveVisitDecision::Continue;
			}, cancelled);
		if (cancelled.load(std::memory_order_acquire) || scan.code == QStringLiteral("cancelled"))
			return QStringLiteral("cancelled");
		if (scan.code != QStringLiteral("ok")) return scan.code;
		return copiedFrames == quint64(target.size()) ? QStringLiteral("ok")
			: QStringLiteral("asset_integrity_failed");
	};
	if (timingMap)
	{
		try
		{
			renderInputBuffer.resize(size_t(budget.renderInputFrames));
		}
		catch (const std::bad_alloc&)
		{
			return {QStringLiteral("resource_limit"), {}, {}, budget.jobBytes};
		}
		VocalEditFrameRange renderInputDomain;
		if (const auto* direct = std::get_if<VocalDirectInput>(&input.ticket.request.input))
		{
			const auto& selection = input.ticket.request.selections[direct->selectionIndex];
			const auto& sourceTicket = input.ticket.sources.sources[direct->selectionIndex];
			renderInputDomain = selection.sourceFrames;
			const auto captured = captureSelection(sourceTicket, selection, renderInputBuffer);
			if (captured != QStringLiteral("ok")) return {captured, {}, {}, budget.jobBytes};
			measureSource(renderInputBuffer);
			expectedSourceSamples = quint64(renderInputBuffer.size()) * quint64(budget.channels);
		}
		else
		{
			const auto& comp = std::get<VocalCompRecipe>(input.ticket.request.input);
			std::vector<std::vector<SampleFrame>> selectedBuffers;
			std::vector<VocalCompSourceView> sourceViews;
			std::vector<SampleFrame> compBlock;
			try
			{
				selectedBuffers.resize(input.ticket.request.selections.size());
				for (size_t i = 0; i < selectedBuffers.size(); ++i)
				{
					const auto& range = input.ticket.request.selections[i].sourceFrames;
					selectedBuffers[i].resize(size_t(range.outFrame - range.inFrame));
				}
				sourceViews.reserve(selectedBuffers.size());
				compBlock.resize(VocalRenderBlockFrames);
				segmentMeasurements.resize(comp.segments.size());
			}
			catch (const std::bad_alloc&)
			{
				return {QStringLiteral("resource_limit"), {}, {}, budget.jobBytes};
			}
			for (size_t i = 0; i < selectedBuffers.size(); ++i)
			{
				const auto captured = captureSelection(input.ticket.sources.sources[i],
					input.ticket.request.selections[i], selectedBuffers[i]);
				if (captured != QStringLiteral("ok")) return {captured, {}, {}, budget.jobBytes};
				sourceViews.push_back({input.ticket.request.selections[i].sourceFrames.inFrame,
					std::span<const SampleFrame>(selectedBuffers[i])});
			}
			for (size_t i = 0; i < comp.segments.size(); ++i)
			{
				const auto& segment = comp.segments[i];
				const auto& selection = input.ticket.request.selections[segment.selectionIndex];
				const auto offset = size_t(segment.sourceFrames.inFrame - selection.sourceFrames.inFrame);
				const auto count = size_t(segment.sourceFrames.outFrame - segment.sourceFrames.inFrame);
				const auto frames = std::span<const SampleFrame>(selectedBuffers[segment.selectionIndex]).subspan(offset, count);
				for (const auto& frame : frames)
				{
					const auto left = double(frame.left());
					segmentMeasurements[i].peak = std::max(segmentMeasurements[i].peak, std::abs(left));
					segmentMeasurements[i].sumSquares += static_cast<long double>(left) * static_cast<long double>(left);
					++segmentMeasurements[i].samples;
					if (budget.channels == 2)
					{
						const auto right = double(frame.right());
						segmentMeasurements[i].peak = std::max(segmentMeasurements[i].peak, std::abs(right));
						segmentMeasurements[i].sumSquares += static_cast<long double>(right) * static_cast<long double>(right);
						++segmentMeasurements[i].samples;
					}
				}
				sourcePeak = std::max(sourcePeak, segmentMeasurements[i].peak);
				sourceSumSquares += segmentMeasurements[i].sumSquares;
				if (!checkedAdd(sourceSamplesMeasured, segmentMeasurements[i].samples)
					|| !checkedAdd(expectedSourceSamples, quint64(count) * quint64(budget.channels)))
					return {QStringLiteral("resource_limit"), {}, {}, budget.jobBytes};
			}
			renderInputDomain = {0, qint64(budget.renderInputFrames)};
			for (quint64 firstFrame = 0; firstFrame < budget.renderInputFrames;)
			{
				if (cancelled.load(std::memory_order_acquire))
					return {QStringLiteral("cancelled"), {}, {}, budget.jobBytes};
				const auto count = size_t(std::min<quint64>(VocalRenderBlockFrames,
					budget.renderInputFrames - firstFrame));
				auto output = std::span<SampleFrame>(compBlock.data(), count);
				const auto assembled = AiBridgeVocalComp::process(comp, sourceViews, qint64(firstFrame), output);
				if (assembled != QStringLiteral("ok")) return {assembled, {}, {}, budget.jobBytes};
				std::copy(output.begin(), output.end(), renderInputBuffer.begin() + std::ptrdiff_t(firstFrame));
				firstFrame += quint64(count);
			}
		}

		std::vector<VocalEditOperation> gainFadeOperations;
		try
		{
			gainFadeOperations.reserve(input.ticket.request.operations.size());
			for (const auto& operation : input.ticket.request.operations)
				if (!std::holds_alternative<VocalTimingMap>(operation)) gainFadeOperations.push_back(operation);
		}
		catch (const std::bad_alloc&)
		{
			return {QStringLiteral("resource_limit"), {}, {}, budget.jobBytes};
		}
		for (size_t offset = 0; offset < renderInputBuffer.size();)
		{
			if (cancelled.load(std::memory_order_acquire))
				return {QStringLiteral("cancelled"), {}, {}, budget.jobBytes};
			const auto count = std::min(VocalRenderBlockFrames, renderInputBuffer.size() - offset);
			const auto inputFrames = std::span<const SampleFrame>(renderInputBuffer.data() + offset, count);
			const auto firstFrame = renderInputDomain.inFrame + qint64(offset);
			auto output = std::span<float>(outputBlock.data(), count * size_t(budget.channels));
			const auto processed = AiBridgeVocalGainFade::process(renderInputDomain, gainFadeOperations,
				firstFrame, inputFrames, budget.channels, output);
			if (processed != QStringLiteral("ok")) return {processed, {}, {}, budget.jobBytes};
			for (size_t i = 0; i < count; ++i)
			{
				const auto left = output[i * size_t(budget.channels)];
				const auto right = budget.channels == 2 ? output[i * 2 + 1] : left;
				if (!std::isfinite(left) || !std::isfinite(right))
					return {QStringLiteral("invalid_audio"), {}, {}, budget.jobBytes};
				renderInputBuffer[offset + i] = SampleFrame(left, right);
			}
			offset += count;
		}
		std::vector<VocalDspTimingAnchor> dspAnchors;
		try
		{
			dspAnchors.reserve(input.ticket.plan.resolvedAnchors.size());
			for (const auto& anchor : input.ticket.plan.resolvedAnchors)
				dspAnchors.push_back({anchor.renderInputFrame, anchor.outputFrame,
					anchor.preAnchorCrossfadeFrames, anchor.crossfadeLaw, anchor.copyProtectedInterval});
		}
		catch (const std::bad_alloc&)
		{
			return {QStringLiteral("resource_limit"), {}, {}, budget.jobBytes};
		}
		if (phase) phase(VocalJobPhase::Rendering);
		const auto stretched = AiBridgeVocalDspBackend::renderTimingMap(renderInputBuffer,
			renderInputDomain.inFrame, dspAnchors, timingMap->protectedRenderInputFrames,
			budget.sampleRate, budget.channels, input.ticket.limits.minimumLocalStretchRatio,
			input.ticket.limits.maximumLocalStretchRatio, input.ticket.limits.maxCrossfadeFrames,
			cancelled, [&](quint64, std::span<const SampleFrame> block) -> PcmWaveVisitDecision {
				if (cancelled.load(std::memory_order_acquire)) return PcmWaveVisitDecision::Cancelled;
				for (size_t i = 0; i < block.size(); ++i)
				{
					outputBlock[i * size_t(budget.channels)] = block[i].left();
					if (budget.channels == 2) outputBlock[i * 2 + 1] = block[i].right();
				}
				const auto code = writeRendered(std::span<const float>(outputBlock.data(),
					block.size() * size_t(budget.channels)));
				if (code == QStringLiteral("cancelled")) return PcmWaveVisitDecision::Cancelled;
				if (code != QStringLiteral("ok"))
				{
					renderFailure = code;
					return PcmWaveVisitDecision::ResourceLimit;
				}
				return PcmWaveVisitDecision::Continue;
			});
		if (cancelled.load(std::memory_order_acquire) || stretched.code == QStringLiteral("cancelled"))
			return {QStringLiteral("cancelled"), {}, {}, budget.jobBytes};
		if (renderFailure != QStringLiteral("ok")) return {renderFailure, {}, {}, budget.jobBytes};
		if (stretched.code != QStringLiteral("ok")) return {stretched.code, {}, {}, budget.jobBytes};
		dspMetrics = stretched.metrics;
	}
	else if (const auto* direct = std::get_if<VocalDirectInput>(&input.ticket.request.input))
	{
		const auto& selection = input.ticket.request.selections[direct->selectionIndex];
		const auto& sourceTicket = input.ticket.sources.sources[direct->selectionIndex];
		expectedSourceSamples = budget.outputFrames * quint64(budget.channels);
		const auto scan = scanVocalSource(sourceTicket,
			[&](quint64 firstFrame, std::span<const SampleFrame> decoded) -> PcmWaveVisitDecision {
				if (cancelled.load(std::memory_order_acquire)) return PcmWaveVisitDecision::Cancelled;
				if (firstFrame > quint64(std::numeric_limits<qint64>::max())
					|| decoded.size() > size_t(std::numeric_limits<qint64>::max()))
				{
					renderFailure = QStringLiteral("resource_limit");
					return PcmWaveVisitDecision::ResourceLimit;
				}
				const auto decodedBegin = qint64(firstFrame);
				const auto decodedEnd = decodedBegin + qint64(decoded.size());
				const auto begin = std::max(decodedBegin, selection.sourceFrames.inFrame);
				const auto end = std::min(decodedEnd, selection.sourceFrames.outFrame);
				if (end <= begin) return PcmWaveVisitDecision::Continue;
				auto current = begin;
				while (current < end)
				{
					if (cancelled.load(std::memory_order_acquire)) return PcmWaveVisitDecision::Cancelled;
					const auto count = size_t(std::min<qint64>(qint64(VocalRenderBlockFrames), end - current));
					const auto sourceOffset = size_t(current - decodedBegin);
					const auto inputBlock = decoded.subspan(sourceOffset, count);
					measureSource(inputBlock);
					const auto appended = appendProcessed(current, inputBlock, selection.sourceFrames);
					if (appended != QStringLiteral("ok"))
					{
						renderFailure = appended;
						return appended == QStringLiteral("cancelled")
							? PcmWaveVisitDecision::Cancelled : PcmWaveVisitDecision::ResourceLimit;
					}
					current += qint64(count);
				}
				return PcmWaveVisitDecision::Continue;
			}, cancelled);
		if (cancelled.load(std::memory_order_acquire) || scan.code == QStringLiteral("cancelled"))
			return {QStringLiteral("cancelled"), {}, {}, budget.jobBytes};
		if (renderFailure != QStringLiteral("ok")) return {renderFailure, {}, {}, budget.jobBytes};
		if (scan.code != QStringLiteral("ok")) return {scan.code, {}, {}, budget.jobBytes};
	}
	else
	{
		const auto& comp = std::get<VocalCompRecipe>(input.ticket.request.input);
		std::vector<std::vector<SampleFrame>> selectedBuffers;
		std::vector<SampleFrame> compBlock;
		std::vector<VocalCompSourceView> sourceViews;
		try
		{
			selectedBuffers.resize(input.ticket.request.selections.size());
			for (size_t i = 0; i < selectedBuffers.size(); ++i)
			{
				const auto& range = input.ticket.request.selections[i].sourceFrames;
				selectedBuffers[i].resize(size_t(range.outFrame - range.inFrame));
			}
			compBlock.resize(VocalRenderBlockFrames);
			sourceViews.reserve(selectedBuffers.size());
		}
		catch (const std::bad_alloc&)
		{
			return {QStringLiteral("resource_limit"), {}, {}, budget.jobBytes};
		}
		for (size_t i = 0; i < selectedBuffers.size(); ++i)
		{
			if (cancelled.load(std::memory_order_acquire))
				return {QStringLiteral("cancelled"), {}, {}, budget.jobBytes};
			const auto& selection = input.ticket.request.selections[i];
			const auto& sourceTicket = input.ticket.sources.sources[i];
			quint64 copiedFrames = 0;
			const auto scan = scanVocalSource(sourceTicket,
				[&](quint64 firstFrame, std::span<const SampleFrame> decoded) -> PcmWaveVisitDecision {
					if (cancelled.load(std::memory_order_acquire)) return PcmWaveVisitDecision::Cancelled;
					if (firstFrame > quint64(std::numeric_limits<qint64>::max())
						|| decoded.size() > size_t(std::numeric_limits<qint64>::max()))
					{
						return PcmWaveVisitDecision::ResourceLimit;
					}
					const auto decodedBegin = qint64(firstFrame);
					const auto decodedEnd = decodedBegin + qint64(decoded.size());
					const auto begin = std::max(decodedBegin, selection.sourceFrames.inFrame);
					const auto end = std::min(decodedEnd, selection.sourceFrames.outFrame);
					if (end <= begin) return PcmWaveVisitDecision::Continue;
					const auto sourceOffset = size_t(begin - decodedBegin);
					const auto targetOffset = size_t(begin - selection.sourceFrames.inFrame);
					const auto count = size_t(end - begin);
					std::copy_n(decoded.begin() + std::ptrdiff_t(sourceOffset), count,
						selectedBuffers[i].begin() + std::ptrdiff_t(targetOffset));
					copiedFrames += quint64(count);
					return PcmWaveVisitDecision::Continue;
				}, cancelled);
			if (cancelled.load(std::memory_order_acquire) || scan.code == QStringLiteral("cancelled"))
				return {QStringLiteral("cancelled"), {}, {}, budget.jobBytes};
			if (scan.code != QStringLiteral("ok"))
				return {scan.code, {}, {}, budget.jobBytes};
			if (copiedFrames != quint64(selectedBuffers[i].size()))
				return {QStringLiteral("asset_integrity_failed"), {}, {}, budget.jobBytes};
			sourceViews.push_back({selection.sourceFrames.inFrame,
				std::span<const SampleFrame>(selectedBuffers[i])});
		}
		try { segmentMeasurements.resize(comp.segments.size()); }
		catch (const std::bad_alloc&) { return {QStringLiteral("resource_limit"), {}, {}, budget.jobBytes}; }
		for (size_t i = 0; i < comp.segments.size(); ++i)
		{
			const auto& segment = comp.segments[i];
			const auto& selection = input.ticket.request.selections[segment.selectionIndex];
			const auto offset = size_t(segment.sourceFrames.inFrame - selection.sourceFrames.inFrame);
			const auto count = size_t(segment.sourceFrames.outFrame - segment.sourceFrames.inFrame);
			const auto frames = std::span<const SampleFrame>(selectedBuffers[segment.selectionIndex]).subspan(offset, count);
			for (const auto& frame : frames)
			{
				const auto left = double(frame.left());
				segmentMeasurements[i].peak = std::max(segmentMeasurements[i].peak, std::abs(left));
				segmentMeasurements[i].sumSquares += static_cast<long double>(left) * static_cast<long double>(left);
				++segmentMeasurements[i].samples;
				if (budget.channels == 2)
				{
					const auto right = double(frame.right());
					segmentMeasurements[i].peak = std::max(segmentMeasurements[i].peak, std::abs(right));
					segmentMeasurements[i].sumSquares += static_cast<long double>(right) * static_cast<long double>(right);
					++segmentMeasurements[i].samples;
				}
			}
			sourcePeak = std::max(sourcePeak, segmentMeasurements[i].peak);
			sourceSumSquares += segmentMeasurements[i].sumSquares;
			sourceSamplesMeasured += segmentMeasurements[i].samples;
		}
		const VocalEditFrameRange renderInputFrames{0, qint64(budget.outputFrames)};
		for (quint64 firstFrame = 0; firstFrame < budget.outputFrames;)
		{
			if (cancelled.load(std::memory_order_acquire))
				return {QStringLiteral("cancelled"), {}, {}, budget.jobBytes};
			const auto count = size_t(std::min<quint64>(VocalRenderBlockFrames, budget.outputFrames - firstFrame));
			auto compOutput = std::span<SampleFrame>(compBlock.data(), count);
			const auto assembled = AiBridgeVocalComp::process(comp, sourceViews, qint64(firstFrame), compOutput);
			if (assembled != QStringLiteral("ok")) return {assembled, {}, {}, budget.jobBytes};
			const auto appended = appendProcessed(qint64(firstFrame), compOutput, renderInputFrames);
			if (appended != QStringLiteral("ok")) return {appended, {}, {}, budget.jobBytes};
			firstFrame += quint64(count);
		}
		for (const auto& segment : comp.segments)
			expectedSourceSamples += quint64(segment.sourceFrames.outFrame - segment.sourceFrames.inFrame)
				* quint64(budget.channels);
	}
	if (cancelled.load(std::memory_order_acquire)) return {QStringLiteral("cancelled"), {}, {}, budget.jobBytes};
	if (framesWritten != budget.outputFrames || created.writer->framesWritten() != budget.outputFrames
		|| samplesMeasured != budget.outputFrames * quint64(budget.channels)
		|| sourceSamplesMeasured != expectedSourceSamples)
	{
		return {QStringLiteral("asset_integrity_failed"), {}, {}, budget.jobBytes};
	}
	if (cancelled.load(std::memory_order_acquire)) return {QStringLiteral("cancelled"), {}, {}, budget.jobBytes};

	ManagedVocalManifest manifest;
	manifest.schemaVersion = 2;
	manifest.projectId = input.projectId;
	manifest.media = {budget.outputFrames, budget.sampleRate, budget.channels, {}, peak, peak > 1.0};
	const auto outputRms = samplesMeasured ? std::sqrt(double(sumSquares / samplesMeasured)) : 0.0;
	const auto sourceRms = sourceSamplesMeasured
		? std::sqrt(double(sourceSumSquares / sourceSamplesMeasured)) : 0.0;
	manifest.media.rms = outputRms;
	manifest.media.rmsKnown = true;
	manifest.recipeHash = input.ticket.plan.recipeHash;
	manifest.rendererId = input.ticket.plan.rendererId;
	manifest.rendererVersion = input.ticket.plan.rendererVersion;
	manifest.rendererOptions = QJsonObject{
		{QStringLiteral("gain_interpolation"), QStringLiteral("linear_db")},
		{QStringLiteral("interior_boundary_policy"), QStringLiteral("unity_gain")},
		{QStringLiteral("linear_fade"), QStringLiteral("linear_amplitude")},
		{QStringLiteral("equal_power_fade"), QStringLiteral("sine_cosine")},
		{QStringLiteral("crossfade_weights"), QStringLiteral("sample_center_linear_or_sine_cosine")},
		{QStringLiteral("peak_policy"), QStringLiteral("report_no_clipping")}};
	if (timingMap)
	{
		manifest.rendererOptions.insert(QStringLiteral("stretch_backend"),
			QString::fromLatin1(AiBridgeVocalDspBackend::RendererId));
		manifest.rendererOptions.insert(QStringLiteral("stretch_backend_version"),
			QString::fromLatin1(AiBridgeVocalDspBackend::StretchVersion));
		manifest.rendererOptions.insert(QStringLiteral("stretch_source_commit"),
			QString::fromLatin1(AiBridgeVocalDspBackend::StretchSourceCommit));
		manifest.rendererOptions.insert(QStringLiteral("stretch_dependency_commit"),
			QStringLiteral("Signalsmith Linear ") + QString::fromLatin1(AiBridgeVocalDspBackend::LinearVersion)
				+ QStringLiteral(" / ") + QString::fromLatin1(AiBridgeVocalDspBackend::LinearDependencyCommit));
		manifest.rendererOptions.insert(QStringLiteral("stretch_processing"), QStringLiteral("offline_blockwise"));
		manifest.rendererOptions.insert(QStringLiteral("stretch_map_policy"),
			QStringLiteral("per_anchor_segments_with_declared_pre_anchor_crossfades"));
		manifest.rendererOptions.insert(QStringLiteral("maximum_process_block_frames"),
			QString::number(AiBridgeVocalDspBackend::MaximumProcessBlockFrames));
		manifest.rendererOptions.insert(QStringLiteral("maximum_process_call_microseconds"),
			QString::number(dspMetrics.maximumProcessCallMicroseconds));
		manifest.rendererOptions.insert(QStringLiteral("maximum_uninterruptible_preroll_frames"),
			QString::number(dspMetrics.maximumUninterruptibleInputFrames));
		manifest.rendererOptions.insert(QStringLiteral("reserved_backend_workspace_bytes"),
			QString::number(dspMetrics.reservedWorkspaceBytes));
		manifest.rendererOptions.insert(QStringLiteral("pitch_scale"), 1.0);
		manifest.rendererOptions.insert(QStringLiteral("maximum_output_anchor_error_frames"),
			input.ticket.limits.maximumAnchorErrorFrames);
	}
	manifest.recipe = recipeJson(input.ticket.request);
	if (const auto* direct = std::get_if<VocalDirectInput>(&input.ticket.request.input))
	{
		const auto& selection = input.ticket.request.selections[direct->selectionIndex];
		const auto& sourceTicket = input.ticket.sources.sources[direct->selectionIndex];
		manifest.realizedFrameMaps.append(QJsonObject{
			{QStringLiteral("selection_index"), int(direct->selectionIndex)},
			{QStringLiteral("source_clip_id"), selection.clipId},
			{QStringLiteral("source_frames"), frameRangeJson(selection.sourceFrames)},
			{QStringLiteral("output_frames"), frameRangeJson({0, qint64(budget.renderInputFrames)})},
			{QStringLiteral("source_measurements"), QJsonObject{
				{QStringLiteral("peak"), sourcePeak}, {QStringLiteral("rms"), sourceRms},
				{QStringLiteral("overrange"), sourcePeak > 1.0}}}});
		const auto sourceIdentity = QString::fromLatin1(
			QCryptographicHash::hash(sourceTicket.expectedIdentity, QCryptographicHash::Sha256).toHex());
		const auto sourceDigest = QString::fromLatin1(sourceTicket.expectedInfo.sourceSha256.toHex());
		manifest.sources.push_back({selection.clipId, sourceDigest, sourceIdentity, sourceTicket.reference,
			{selection.sourceFrames.inFrame, selection.sourceFrames.outFrame},
			qint64(sourceTicket.expectedInfo.frames), budget.sampleRate, budget.channels,
			ManagedVocalSourceBasis::AuthorizedPcm, sourceTicket.recordedTakeProjectId,
			sourceTicket.recordedTakeId});
	}
	else
	{
		const auto& comp = std::get<VocalCompRecipe>(input.ticket.request.input);
		for (size_t i = 0; i < input.ticket.request.selections.size(); ++i)
		{
			const auto& selection = input.ticket.request.selections[i];
			const auto& sourceTicket = input.ticket.sources.sources[i];
			const auto sourceIdentity = QString::fromLatin1(
				QCryptographicHash::hash(sourceTicket.expectedIdentity, QCryptographicHash::Sha256).toHex());
			const auto sourceDigest = QString::fromLatin1(sourceTicket.expectedInfo.sourceSha256.toHex());
			manifest.sources.push_back({selection.clipId, sourceDigest, sourceIdentity, sourceTicket.reference,
				{selection.sourceFrames.inFrame, selection.sourceFrames.outFrame},
				qint64(sourceTicket.expectedInfo.frames), budget.sampleRate, budget.channels,
				ManagedVocalSourceBasis::AuthorizedPcm, sourceTicket.recordedTakeProjectId,
				sourceTicket.recordedTakeId});
		}
		for (size_t i = 0; i < comp.segments.size(); ++i)
		{
			const auto& segment = comp.segments[i];
			const auto& selection = input.ticket.request.selections[segment.selectionIndex];
			const auto& measured = segmentMeasurements[i];
			const auto segmentRms = measured.samples
				? std::sqrt(double(measured.sumSquares / measured.samples)) : 0.0;
			QJsonObject map{{QStringLiteral("segment_index"), int(i)},
				{QStringLiteral("selection_index"), int(segment.selectionIndex)},
				{QStringLiteral("source_clip_id"), selection.clipId},
				{QStringLiteral("source_frames"), frameRangeJson(segment.sourceFrames)},
				{QStringLiteral("output_frames"), frameRangeJson(segment.outputFrames)},
				{QStringLiteral("source_measurements"), QJsonObject{
					{QStringLiteral("peak"), measured.peak}, {QStringLiteral("rms"), segmentRms},
					{QStringLiteral("overrange"), measured.peak > 1.0}}}};
			if (i)
			{
				if (const auto* boundary = boundaryFor(comp, i - 1, i))
					map.insert(QStringLiteral("crossfade_in"), QJsonObject{
						{QStringLiteral("overlap_frames"), QString::number(boundary->overlapFrames)},
						{QStringLiteral("law"), boundary->law == VocalCrossfadeLaw::LinearAmplitude
							? QStringLiteral("linear_amplitude") : QStringLiteral("equal_power")}});
			}
			if (i + 1 < comp.segments.size())
			{
				if (const auto* boundary = boundaryFor(comp, i, i + 1))
					map.insert(QStringLiteral("crossfade_out"), QJsonObject{
						{QStringLiteral("overlap_frames"), QString::number(boundary->overlapFrames)},
						{QStringLiteral("law"), boundary->law == VocalCrossfadeLaw::LinearAmplitude
							? QStringLiteral("linear_amplitude") : QStringLiteral("equal_power")}});
			}
			manifest.realizedFrameMaps.append(map);
		}
	}
	if (timingMap)
	{
		for (size_t i = 0; i < input.ticket.plan.resolvedAnchors.size(); ++i)
		{
			const auto& anchor = input.ticket.plan.resolvedAnchors[i];
			QJsonObject realized{{QStringLiteral("map_kind"), QStringLiteral("timing_anchor")},
				{QStringLiteral("anchor_index"), int(i)},
				{QStringLiteral("render_input_frame"), QString::number(anchor.renderInputFrame)},
				{QStringLiteral("requested_output_frame"), QString::number(anchor.requestedOutputFrame)},
				{QStringLiteral("realized_output_frame"), QString::number(anchor.outputFrame)},
				{QStringLiteral("tick_rounding_error_frames"), anchor.roundingErrorFrames},
				{QStringLiteral("tick_rounding_error_numerator"), QString::number(anchor.roundingErrorNumerator)},
				{QStringLiteral("tick_rounding_error_denominator"), QString::number(anchor.roundingErrorDenominator)},
				{QStringLiteral("strength_rounding_error_frames"), anchor.strengthRoundingErrorFrames},
				{QStringLiteral("pre_anchor_crossfade_frames"), QString::number(anchor.preAnchorCrossfadeFrames)},
				{QStringLiteral("crossfade_law"), anchor.crossfadeLaw == VocalCrossfadeLaw::LinearAmplitude
					? QStringLiteral("linear_amplitude") : QStringLiteral("equal_power")},
				{QStringLiteral("copy_protected_interval"), anchor.copyProtectedInterval},
				{QStringLiteral("synthetic_protected_boundary"), anchor.syntheticProtectedBoundary}};
			if (anchor.targetTick)
				realized.insert(QStringLiteral("target_tick"), QString::number(*anchor.targetTick));
			manifest.realizedFrameMaps.append(realized);
		}
	}
	if (phase) phase(VocalJobPhase::Publishing);
	if (cancelled.load(std::memory_order_acquire)) return {QStringLiteral("cancelled"), {}, {}, budget.jobBytes};
	for (const auto& sourceTicket : input.ticket.sources.sources)
	{
		const auto revalidated = scanVocalSource(sourceTicket,
			[](quint64, std::span<const SampleFrame>) { return PcmWaveVisitDecision::Continue; }, cancelled);
		if (cancelled.load(std::memory_order_acquire) || revalidated.code == QStringLiteral("cancelled"))
			return {QStringLiteral("cancelled"), {}, {}, budget.jobBytes};
		if (revalidated.code != QStringLiteral("ok"))
		{
			const auto code = revalidated.code == QStringLiteral("audio_source_changed")
				|| revalidated.code == QStringLiteral("audio_source_missing")
				? QStringLiteral("source_changed") : revalidated.code;
			return {code, {}, {}, budget.jobBytes};
		}
	}
	if (cancelled.load(std::memory_order_acquire)) return {QStringLiteral("cancelled"), {}, {}, budget.jobBytes};
	if (input.managedProjectHold && (!input.managedProjectHold->isAuthorized()
		|| input.managedProjectHold->projectOwner().projectId != input.projectId
		|| input.managedProjectHold->projectOwner().rootId != input.assetRoot->rootId()))
		return {QStringLiteral("project_identity_unverified"), {}, {}, budget.jobBytes};
	auto published = store.publish(std::move(created.writer), std::move(manifest));
	if (published.code != QStringLiteral("ok") || !published.value)
	{
		return {published.code, {}, {}, budget.jobBytes};
	}
	VocalRenderMeasurements measurements;
	measurements.frames = budget.outputFrames;
	measurements.sampleRate = budget.sampleRate;
	measurements.channels = budget.channels;
	measurements.sourcePeak = sourcePeak;
	measurements.sourceRms = sourceRms;
	measurements.sourceOverrange = sourcePeak > 1.0;
	measurements.peak = peak;
	measurements.rms = outputRms;
	measurements.overrange = peak > 1.0;
	return {QStringLiteral("ok"), std::move(published.value), measurements, budget.jobBytes};
}

} // namespace lmms
