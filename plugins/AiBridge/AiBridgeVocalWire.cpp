#include "AiBridgeVocalWire.h"
#include "AiBridgeVocalRequest.h"
#include "AiBridgeVocalContext.h"
#include "AiBridgeVocalAssetActions.h"
#include "AiBridgeVocalVariantAttachment.h"
#include "AiBridgeVocalTrackActions.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "SampleTrack.h"
#include "Song.h"
#include "src/core/vocal/ManagedVocalPlayback.h"
#include "src/core/vocal/VocalRuntimeQualification.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QThread>
namespace lmms {
namespace {
QString stateName(VocalJobState state)
{
	switch(state){case VocalJobState::Queued:return "queued";case VocalJobState::Running:return "running";
	case VocalJobState::CancelRequested:return "cancel_requested";case VocalJobState::Cancelled:return "cancelled";
	case VocalJobState::Succeeded:return "succeeded";case VocalJobState::Failed:return "failed";case VocalJobState::Invalidated:return "invalidated";}
	return "invalidated";
}
QString phaseName(VocalJobPhase phase)
{
	switch(phase){case VocalJobPhase::Authorizing:return "authorizing";case VocalJobPhase::Scanning:return "scanning";
	case VocalJobPhase::Analyzing:return "analyzing";case VocalJobPhase::Rendering:return "rendering";
	case VocalJobPhase::Publishing:return "publishing";}return "authorizing";
}
QJsonObject jobJson(const VocalJobSnapshot& job)
{
	QJsonObject result{{"job_id",job.jobId},{"input_hash",job.inputHash},{"project_generation",QString::number(job.generation)},
		{"state",stateName(job.state)},{"phase",phaseName(job.phase)}};
	if(job.kind==VocalJobKind::Render)result.insert(QStringLiteral("kind"),QStringLiteral("render"));
	if(job.progress)result["progress"]=*job.progress;
	if(!job.analysisId.isEmpty())result["analysis_id"]=job.analysisId;
	if(!job.variantId.isEmpty())result["variant_id"]=job.variantId;
	if(!job.failureCode.isEmpty())result["failure_code"]=job.failureCode;
	return result;
}
QJsonObject rangeJson(const VocalFrameRange& range)
{return {{"in_frame",QString::number(range.inFrame)},{"out_frame",QString::number(range.outFrame)}};}
QJsonObject metricsJson(const VocalChannelMetrics& m)
{
	return {{"count",QString::number(m.count)},{"peak",m.peak},{"rms",m.rms},{"dc",m.dc},{"silence",m.silence},
		{"peak_dbfs",m.peakDbfs?QJsonValue(*m.peakDbfs):QJsonValue(QJsonValue::Null)},
		{"rms_dbfs",m.rmsDbfs?QJsonValue(*m.rmsDbfs):QJsonValue(QJsonValue::Null)},
		{"full_scale_candidates",QString::number(m.fullScaleCandidates)},{"overrange",QString::number(m.overrange)}};
}
QJsonArray channelsJson(const std::array<VocalChannelMetrics,2>& metrics,int count)
{QJsonArray result;for(int i=0;i<count;++i)result.append(metricsJson(metrics[i]));return result;}
QJsonObject analysisJson(const VocalAnalysisPage& page)
{
	QJsonArray summaries,records;
	for(const auto& summary:page.summaries)
	{
		const auto& p=summary.provenance;
		summaries.append(QJsonObject{{"clip_id",p.clipId},{"source_revision",p.sourceRevision},{"project_generation",QString::number(p.generation)},
			{"source_frames",rangeJson(p.frames)},{"sample_rate",p.sampleRate},{"channels",p.channels},{"window_frames",QString::number(p.windowFrames)},
			{"basis",p.basis},{"algorithm",p.algorithm},{"authorization",p.authorization},{"source_validation",p.sourceValidation},
			{"settings",QJsonObject{{"low_level_dbfs",p.settings.lowLevelDbfs},{"minimum_low_level_ms",p.settings.minimumLowLevelMs}}},
			{"metrics",channelsJson(summary.channels,p.channels)}});
	}
	for(const auto& record:page.records)
	{
		const auto count=page.summaries.at(size_t(record.selectionIndex)).provenance.channels;
		records.append(QJsonObject{{"kind",record.kind==VocalAnalysisRecord::Kind::Window?"window":"low_level_span"},
			{"selection_index",record.selectionIndex},{"source_frames",rangeJson(record.frames)},{"metrics",channelsJson(record.channels,count)}});
	}
	return {{"analysis_id",page.analysisId},{"applicability",page.applicability},{"summaries",summaries},{"records",records},
		{"total",page.page.total},{"offset",page.page.offset},{"limit",page.page.limit},{"has_more",page.page.hasMore}};
}
QJsonObject variantJson(const ManagedVocalAssetInspectResult& asset)
{
	const auto world=captureVocalEditWorld();
	const auto worldHash=vocalEditWorldHash(world);
	QString eligibility=QStringLiteral("eligible_for_target_preflight");
	if(!qualifiedVocalSamplerateRuntime())
		eligibility=QStringLiteral("vocal_runtime_unqualified");
	else if(world.tempoAutomationPresent || world.tempoBpm<=0 || world.ticksPerQuarterNote!=48
		|| world.timeSignatureNumerator<1 || world.timeSignatureNumerator>32
		|| world.timeSignatureDenominator<1 || world.timeSignatureDenominator>32)
		eligibility=QStringLiteral("unsupported_playback_world");
	else if((asset.sampleRate!=44100 && asset.sampleRate!=48000) || asset.sampleRate!=world.outputRate)
		eligibility=QStringLiteral("unsupported_sample_rate");
	const auto extent=managedVocalVisualExtent(asset.frames,asset.sampleRate,world.tempoBpm);
	if(eligibility==QStringLiteral("eligible_for_target_preflight") && extent.code!=QStringLiteral("ok"))
		eligibility=extent.code;
	QJsonArray sources;
	for(const auto& source:asset.sources)
	{
		sources.append(QJsonObject{{"clip_id",source.clipId},{"source_sha256",source.sourceSha256},
			{"source_frames",rangeJson({source.sourceFrames.inFrame,source.sourceFrames.outFrame})},
			{"source_frame_count",QString::number(source.sourceFrameCount)},
			{"sample_rate",source.sampleRate},{"channels",source.channels},
			{"source_basis",source.basis==ManagedVocalSourceBasis::AuthorizedPcm?QStringLiteral("authorized_pcm")
				:QStringLiteral("playback_reference")}});
	}
	AiBridgeProjectContext ids;
	return {{"variant_id",asset.assetId},{"status",QStringLiteral("available")},{"integrity",QStringLiteral("verified")},
		{"media_sha256",asset.mediaSha256},{"manifest_sha256",asset.manifestSha256},{"recipe_hash",asset.recipeHash},
		{"renderer_id",asset.rendererId},{"renderer_version",asset.rendererVersion},
		{"frames",QString::number(asset.frames)},{"sample_rate",asset.sampleRate},{"channels",asset.channels},
		{"peak",asset.media.peak},{"rms",asset.media.rms},{"overrange",asset.media.overrange},
		{"source_count",int(asset.sources.size())},{"sources",sources},
		{"project_generation",QString::number(ids.projectGeneration())},{"world_hash",worldHash},
		{"playback_eligibility",eligibility},{"attachment_eligibility",eligibility}};
}
VocalEditFrameRange editRange(const QJsonObject& object)
{
	return {object.value(QStringLiteral("in_frame")).toString().toLongLong(),
		object.value(QStringLiteral("out_frame")).toString().toLongLong()};
}
VocalEditRequest parseEditRequest(const QJsonObject& parameters)
{
	VocalEditRequest request;
	request.recipeVersion=quint32(parameters.value(QStringLiteral("recipe_version")).toInt());
	request.expectedProjectGeneration=parameters.value(QStringLiteral("expected_project_generation")).toString().toULongLong();
	request.expectedWorldHash=parameters.value(QStringLiteral("expected_world_hash")).toString();
	for(const auto& item:parameters.value(QStringLiteral("selections")).toArray())
	{
		const auto selection=item.toObject();
		const auto basis=selection.value(QStringLiteral("source_basis")).toString();
		request.selections.push_back({selection.value(QStringLiteral("clip_id")).toString(),
			selection.value(QStringLiteral("expected_audio_state_hash")).toString(),
			basis==QStringLiteral("source_pcm")?VocalEditSourceBasis::SourcePcm:VocalEditSourceBasis::PlaybackReference,
			editRange(selection.value(QStringLiteral("source_frames")).toObject())});
	}
	if(parameters.value(QStringLiteral("input_kind"))==QStringLiteral("direct_source"))
	{
		request.input=VocalDirectInput{size_t(parameters.value(QStringLiteral("input_selection_index")).toInt())};
	}
	else
	{
		const auto compObject=parameters.value(QStringLiteral("comp")).toObject();
		VocalCompRecipe comp;
		for(const auto& item:compObject.value(QStringLiteral("segments")).toArray())
		{
			const auto segment=item.toObject();
			comp.segments.push_back({size_t(segment.value(QStringLiteral("selection_index")).toInt()),
				editRange(segment.value(QStringLiteral("source_frames")).toObject()),
				editRange(segment.value(QStringLiteral("output_frames")).toObject())});
		}
		for(const auto& item:compObject.value(QStringLiteral("boundaries")).toArray())
		{
			const auto boundary=item.toObject();
			comp.boundaries.push_back({size_t(boundary.value(QStringLiteral("left_segment_index")).toInt()),
				size_t(boundary.value(QStringLiteral("right_segment_index")).toInt()),
				boundary.value(QStringLiteral("overlap_frames")).toString().toLongLong(),
				boundary.value(QStringLiteral("law"))==QStringLiteral("linear_amplitude")
					?VocalCrossfadeLaw::LinearAmplitude:VocalCrossfadeLaw::EqualPower});
		}
		request.input=std::move(comp);
	}
	for(const auto& item:parameters.value(QStringLiteral("operations")).toArray())
	{
		const auto operation=item.toObject();
		const auto kind=operation.value(QStringLiteral("operation")).toString();
		if(kind==QStringLiteral("gain_envelope"))
		{
			VocalGainEnvelope envelope;
			for(const auto& pointValue:operation.value(QStringLiteral("points")).toArray())
			{
				const auto point=pointValue.toObject();
				envelope.points.push_back({point.value(QStringLiteral("render_input_frame")).toString().toLongLong(),
					point.value(QStringLiteral("gain_db")).toDouble()});
			}
			request.operations.push_back(std::move(envelope));
		}
		else if(kind==QStringLiteral("fade"))
		{
			request.operations.push_back(VocalFade{editRange(operation.value(QStringLiteral("render_input_frames")).toObject()),
				operation.value(QStringLiteral("start_gain_db")).toDouble(),operation.value(QStringLiteral("end_gain_db")).toDouble(),
				operation.value(QStringLiteral("curve"))==QStringLiteral("linear_amplitude")
					?VocalFadeCurve::LinearAmplitude:VocalFadeCurve::EqualPower});
		}
		else
		{
			VocalTimingMap map;
			map.strength=operation.value(QStringLiteral("strength")).toDouble();
			map.minimumLocalStretchRatio=operation.value(QStringLiteral("minimum_local_stretch_ratio")).toDouble();
			map.maximumLocalStretchRatio=operation.value(QStringLiteral("maximum_local_stretch_ratio")).toDouble();
			for(const auto& anchorValue:operation.value(QStringLiteral("anchors")).toArray())
			{
				const auto anchorObject=anchorValue.toObject();VocalTimingAnchor anchor;
				anchor.renderInputFrame=anchorObject.value(QStringLiteral("render_input_frame")).toString().toLongLong();
				if(anchorObject.contains(QStringLiteral("target_output_frame")))
					anchor.outputFrame=anchorObject.value(QStringLiteral("target_output_frame")).toString().toLongLong();
				else anchor.targetTick=anchorObject.value(QStringLiteral("target_tick")).toVariant().toLongLong();
				anchor.preAnchorCrossfadeFrames=anchorObject.value(QStringLiteral("pre_anchor_crossfade_frames")).toString().toLongLong();
				anchor.crossfadeLaw=anchorObject.value(QStringLiteral("crossfade_law"))==QStringLiteral("linear_amplitude")
					?VocalCrossfadeLaw::LinearAmplitude:VocalCrossfadeLaw::EqualPower;
				map.anchors.push_back(anchor);
			}
			for(const auto& range:operation.value(QStringLiteral("protected_render_input_frames")).toArray())
				map.protectedRenderInputFrames.push_back(editRange(range.toObject()));
			request.operations.push_back(std::move(map));
		}
	}
	return request;
}
QString crossfadeLawName(VocalCrossfadeLaw law)
{
	return law==VocalCrossfadeLaw::LinearAmplitude?QStringLiteral("linear_amplitude"):QStringLiteral("equal_power");
}
QJsonObject planJson(const VocalEditPlanHandle& handle)
{
	QJsonArray sources,anchors,frameMaps;
	for(size_t i=0;i<handle.planning.sources.size();++i)
	{
		const auto& source=handle.planning.sources[i];const auto& selection=handle.request.selections[i];
		sources.append(QJsonObject{{"selection_index",int(i)},{"clip_id",source.clipId},
			{"audio_state_hash",source.audioStateHash},{"source_frames",rangeJson({selection.sourceFrames.inFrame,selection.sourceFrames.outFrame})},
			{"source_frame_count",QString::number(source.sourceFrames)},{"sample_rate",source.sampleRate},
			{"channels",source.channels},{"source_verified",source.sourceVerified}});
	}
	if(const auto* direct=std::get_if<VocalDirectInput>(&handle.request.input))
	{
		const auto& selection=handle.request.selections[direct->selectionIndex];
		frameMaps.append(QJsonObject{{"map_kind",QStringLiteral("direct_source")},
			{"selection_index",int(direct->selectionIndex)},{"source_clip_id",selection.clipId},
			{"source_frames",rangeJson({selection.sourceFrames.inFrame,selection.sourceFrames.outFrame})},
			{"render_input_frames",rangeJson({selection.sourceFrames.inFrame,selection.sourceFrames.outFrame})}});
	}
	else
	{
		const auto& comp=std::get<VocalCompRecipe>(handle.request.input);
		for(size_t i=0;i<comp.segments.size();++i)
		{
			const auto& segment=comp.segments[i];const auto& selection=handle.request.selections[segment.selectionIndex];
			QJsonObject row{{"map_kind",QStringLiteral("comp_segment")},{"segment_index",int(i)},
				{"selection_index",int(segment.selectionIndex)},{"source_clip_id",selection.clipId},
				{"source_frames",rangeJson({segment.sourceFrames.inFrame,segment.sourceFrames.outFrame})},
				{"render_input_frames",rangeJson({segment.outputFrames.inFrame,segment.outputFrames.outFrame})}};
			for(const auto& boundary:comp.boundaries)
			{
				if(boundary.leftSegmentIndex==i)
					row.insert(QStringLiteral("crossfade_out"),QJsonObject{{"overlap_frames",QString::number(boundary.overlapFrames)},
						{"law",crossfadeLawName(boundary.law)}});
				if(boundary.rightSegmentIndex==i)
					row.insert(QStringLiteral("crossfade_in"),QJsonObject{{"overlap_frames",QString::number(boundary.overlapFrames)},
						{"law",crossfadeLawName(boundary.law)}});
			}
			frameMaps.append(row);
		}
	}
	for(const auto& anchor:handle.plan.anchorPreviews)
	{
		QJsonObject row{{"render_input_frame",QString::number(anchor.renderInputFrame)},
			{"requested_output_frame",QString::number(anchor.requestedOutputFrame)},
			{"realized_output_frame",QString::number(anchor.outputFrame)},
			{"tick_rounding_error_frames",anchor.roundingErrorFrames},
			{"tick_rounding_error_numerator",QString::number(anchor.roundingErrorNumerator)},
			{"tick_rounding_error_denominator",QString::number(anchor.roundingErrorDenominator)},
			{"strength_rounding_error_frames",anchor.strengthRoundingErrorFrames},
			{"pre_anchor_crossfade_frames",QString::number(anchor.preAnchorCrossfadeFrames)},
			{"crossfade_law",crossfadeLawName(anchor.crossfadeLaw)},
			{"copy_protected_interval",anchor.copyProtectedInterval},
			{"synthetic_protected_boundary",anchor.syntheticProtectedBoundary}};
		if(anchor.targetTick)row.insert(QStringLiteral("target_tick"),QString::number(*anchor.targetTick));
		anchors.append(row);
	}
	VocalEditFrameRange inputRange;
	if(const auto* direct=std::get_if<VocalDirectInput>(&handle.request.input))
		inputRange=handle.request.selections[direct->selectionIndex].sourceFrames;
	else inputRange={0,std::get<VocalCompRecipe>(handle.request.input).segments.back().outputFrames.outFrame};
	qint64 outputFrames=inputRange.outFrame-inputRange.inFrame;
	const VocalTimingMap* requestedTiming=nullptr;
	for(const auto& operation:handle.request.operations)if(const auto* timing=std::get_if<VocalTimingMap>(&operation))requestedTiming=timing;
	if(requestedTiming && !handle.plan.resolvedAnchors.empty())
		outputFrames=handle.plan.resolvedAnchors.back().outputFrame-handle.plan.resolvedAnchors.front().outputFrame;
	QJsonValue timingRequest(QJsonValue::Null);
	if(requestedTiming)
	{
		QJsonArray protectedRanges;
		for(const auto& range:requestedTiming->protectedRenderInputFrames)
			protectedRanges.append(rangeJson({range.inFrame,range.outFrame}));
		timingRequest=QJsonObject{{"strength",requestedTiming->strength},
			{"minimum_local_stretch_ratio",requestedTiming->minimumLocalStretchRatio},
			{"maximum_local_stretch_ratio",requestedTiming->maximumLocalStretchRatio},
			{"protected_render_input_frames",protectedRanges}};
	}
	const auto& world=handle.planning.world;
	return {{"plan_id",handle.planId},{"plan_hash",handle.plan.planHash},{"recipe_hash",handle.plan.recipeHash},
		{"project_generation",QString::number(handle.plan.projectGeneration)},{"world_hash",handle.plan.worldHash},
		{"world",QJsonObject{{"tempo_bpm",world.tempoBpm},{"output_sample_rate",world.outputRate},
			{"ticks_per_quarter_note",world.ticksPerQuarterNote},{"time_signature_numerator",world.timeSignatureNumerator},
			{"time_signature_denominator",world.timeSignatureDenominator},{"tempo_automation_present",world.tempoAutomationPresent}}},
		{"renderer_id",handle.plan.rendererId},{"renderer_version",handle.plan.rendererVersion},
		{"render_input_frames",QString::number(inputRange.outFrame-inputRange.inFrame)},
		{"output_frames",QString::number(outputFrames)},{"output_frame_extent",rangeJson({0,outputFrames})},
		{"frame_maps",frameMaps},{"sources",sources},{"anchor_preview",anchors},
		{"timing_request",timingRequest},
		{"anchor_preview_truncated",handle.plan.anchorPreviewTruncated},
		{"omitted_anchor_count",int(handle.plan.omittedAnchorCount)},
		{"rounding_policy",QStringLiteral("nearest_ties_up")},
		{"limits",QJsonObject{{"minimum_gain_db",handle.limits.minimumGainDb},{"maximum_gain_db",handle.limits.maximumGainDb},
			{"maximum_gain_fade_points",int(handle.limits.maxGainFadePoints)},
			{"maximum_comp_segments",int(handle.limits.maxCompSegments)},
			{"maximum_crossfade_frames",QString::number(handle.limits.maxCrossfadeFrames)},
			{"maximum_crossfade_milliseconds",250},{"synthetic_pitch_error_target_cents",5.0},
			{"minimum_local_stretch_ratio",handle.limits.minimumLocalStretchRatio},
			{"maximum_local_stretch_ratio",handle.limits.maximumLocalStretchRatio},
			{"maximum_anchor_error_frames",handle.limits.maximumAnchorErrorFrames}}}};
}
QJsonObject renderAdmissionJson(const VocalRenderAdmissionResult& result)
{
	QJsonObject data{{"plan_id",result.planId},{"plan_hash",result.planHash},{"dry_run",result.dryRun},
		{"render_input_frames",QString::number(result.budget.renderInputFrames)},
		{"output_frames",QString::number(result.budget.outputFrames)},
		{"sample_rate",result.budget.sampleRate},{"channels",result.budget.channels},
		{"session_bytes",QString::number(result.budget.sessionBytes)},
		{"job_bytes",QString::number(result.budget.jobBytes)},
		{"staged_bytes",QString::number(result.budget.stagedBytes)}};
	if(!result.dryRun && !result.jobId.isEmpty())data.insert(QStringLiteral("job_id"),result.jobId);
	return data;
}
VocalVariantApplyRequest applyRequest(const QJsonObject& parameters)
{
	return {parameters.value(QStringLiteral("variant_id")).toString(),
		parameters.value(QStringLiteral("clip_id")).toString(),parameters.value(QStringLiteral("mode")).toString(),
		parameters.value(QStringLiteral("destination_track_id")).toString(),
		parameters.value(QStringLiteral("expected_world_hash")).toString(),
		parameters.value(QStringLiteral("expected_audio_state_hash")).toString(),
		parameters.value(QStringLiteral("expected_track_state_hash")).toString(),
		parameters.value(QStringLiteral("expected_destination_track_state_hash")).toString(),
		parameters.value(QStringLiteral("expected_project_generation")).toString().toULongLong(),
		parameters.value(QStringLiteral("dry_run")).toBool()};
}
}
bool AiBridgeVocalWire::handles(const QString& action){return VocalRequest::handles(action);}
bool AiBridgeVocalWire::validRequest(const QString& action,const QJsonObject& p){return VocalRequest::valid(action,p);}
AiBridgeProtocolResponse AiBridgeVocalWire::execute(const AiBridgeProtocolRequest& r,QPointer<AiBridgeVocalJobs> jobs)
{
	const auto reply=[&](const QString& code,QJsonValue data=QJsonValue(QJsonValue::Undefined)){
		AiBridgeProtocolResponse result{2,r.requestId,code=="ok",code,code=="ok"?"Vocal operation complete":"Vocal operation rejected",data};
		if(!QJsonDocument::fromJson(AiBridgeProtocol::serializeResponse(result)).object()["success"].toBool() && result.success)
			return AiBridgeProtocolResponse{2,r.requestId,false,"result_too_large","Vocal response exceeds 64 KiB"};
		return result;
	};
	if(!QCoreApplication::instance() || QThread::currentThread()!=QCoreApplication::instance()->thread())return reply("wrong_thread");
	if(!handles(r.action) || !validRequest(r.action,r.parameters))return reply("invalid_parameter");
	if(!jobs || !jobs->isAvailable())return reply("job_invalidated");
	const auto& p=r.parameters;
	if(r.action=="analyze_vocal_audio")
	{
		VocalAnalyzeRequest request;
		for(const auto& value:p["selections"].toArray())
		{
			const auto selection=value.toObject();const auto frames=selection["source_frames"].toObject();
			request.selections.push_back({selection["clip_id"].toString(),selection["expected_audio_state_hash"].toString(),
				{frames["in_frame"].toString().toLongLong(),frames["out_frame"].toString().toLongLong()}});
		}
		const auto settings=p["settings"].toObject();
		if(settings.contains("low_level_dbfs"))request.settings.lowLevelDbfs=settings["low_level_dbfs"].toDouble();
		if(settings.contains("minimum_low_level_ms"))request.settings.minimumLowLevelMs=settings["minimum_low_level_ms"].toInt();
		const auto result=jobs->start(request);if(!jobs)return reply("job_invalidated");
		return result.code=="ok"?reply("ok",QJsonObject{{"job_id",result.jobId},{"input_hash",result.inputHash}}):reply(result.code);
	}
	if(r.action=="get_vocal_job" || r.action=="cancel_vocal_job")
	{
		const auto id=p["job_id"].toString();const auto result=r.action=="cancel_vocal_job"?jobs->cancel(id)
			:p["mode"]=="release"?jobs->release(id):jobs->job(id);
		if(!jobs)return reply("job_invalidated");
		if(!result.value)return reply(result.code);
		auto data=jobJson(*result.value);data["cancel_applied"]=result.cancelApplied;
		if(r.action=="get_vocal_job" && p["mode"]=="release")data["released"]=true;
		return reply(result.code,data);
	}
	if(r.action=="get_vocal_analysis")
	{
		const auto result=jobs->analysis(p["analysis_id"].toString(),p["offset"].toInt(0),p["limit"].toInt(256));
		if(!jobs)return reply("job_invalidated");
		return result.code=="ok"?reply("ok",analysisJson(result)):reply(result.code);
	}
	if(r.action=="plan_vocal_edit")
	{
		const auto planned=jobs->planEdit(parseEditRequest(p));
		if(!jobs)return reply("job_invalidated");
		return planned.value?reply("ok",planJson(*planned.value)):reply(planned.code);
	}
	if(r.action=="render_vocal_variant")
	{
		const auto admitted=jobs->renderPlan(p["plan_id"].toString(),p["plan_hash"].toString(),p["dry_run"].toBool());
		if(!jobs)return reply("job_invalidated");
		return admitted.code==QStringLiteral("ok")?reply("ok",renderAdmissionJson(admitted)):reply(admitted.code);
	}
	if(r.action=="get_vocal_variant")
	{
		const auto inspected=AiBridgeVocalAssetActions::inspectVariant(p["variant_id"].toString());
		if(!jobs)return reply("job_invalidated");
		return inspected.code==QStringLiteral("ok")?reply("ok",variantJson(inspected)):reply(inspected.code);
	}
	if(r.action=="apply_vocal_variant")
	{
		const auto applied=AiBridgeVocalVariantAttachment::apply(applyRequest(p));
		if(!jobs)return reply("job_invalidated");
		return applied.data.isEmpty()?reply(applied.code):reply(applied.code,applied.data);
	}
	const auto bounded=validateVocalProjectBounds();if(bounded!="ok")return reply(bounded);
	const auto snapshots=jobs->snapshots();if(!jobs || !jobs->isAvailable())return reply("job_invalidated");
	const auto rechecked=validateVocalProjectBounds();if(rechecked!="ok")return reply(rechecked);
	const auto rootsText=ConfigManager::inst()->value("aibridge","audio_import_roots");
	if(rootsText.size()>VocalLimits::rootChars)return reply("resource_limit");
	const auto roots=QJsonDocument::fromJson(rootsText.toUtf8());
	if(roots.isArray() && roots.array().size()>VocalLimits::rootCount)return reply("resource_limit");
	QJsonArray states,tracks;for(const auto& job:snapshots)states.append(jobJson(job));
	int total=0;AiBridgeProjectContext ids;
	for(auto* native:Engine::getSong()->tracks())if(auto* track=dynamic_cast<SampleTrack*>(native))
	{
		++total;if(tracks.size()>=VocalLimits::tracks)continue;
		const auto captured=captureVocalTrack(ids.idForTrack(track));if(!captured.value)return reply(captured.code);
		const auto& s=*captured.value;tracks.append(QJsonObject{{"track_id",s.trackId},{"name",s.name},{"track_state_hash",s.hash},
			{"project_generation",QString::number(s.generation)},{"volume",s.volume},{"panning",s.panning},{"muted",s.muted},{"solo",s.solo},{"mixer_channel_id",s.mixerId}});
	}
	const auto importLimits=SampleDecoder::configuredImportLimits();
	const auto editWorldHash=vocalEditWorldHash(captureVocalEditWorld());
	return reply("ok",QJsonObject{{"basis","decoded_source_pre_mixer"},{"algorithm","levels_v1"},{"source_validation","job_snapshot"},
		{"vocal_edit_world_hash",editWorldHash.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(editWorldHash)},
		{"analysis_supported",true},{"import_roots_configured",roots.isArray() && !roots.array().isEmpty()},
		{"eq_supported",false},{"eq_reason","deferred_native_model_lifetime"},{"track_property_writes_supported",false},{"track_property_writes_reason","deferred_native_model_lifetime"},
		{"cancellation","cooperative_no_os_read_deadline"},{"jobs",states},{"tracks",tracks},{"track_page",QJsonObject{{"total",total},{"offset",0},{"limit",128},{"has_more",total>128}}},
		{"native_track_ranges",QJsonObject{{"volume_min",0},{"volume_max",200},{"panning_min",-100},{"panning_max",100},{"step",0.1},{"writable",false}}},
		{"limits",QJsonObject{{"selections",8},{"seconds",120},{"active_workers",1},{"retained_jobs",8},{"selection_records",4096},{"job_records",8192},{"page_records",256},{"track_snapshot_clips",128},
			{"job_bytes",QString::number(VocalLimits::jobBytes)},{"session_bytes",QString::number(VocalLimits::sessionBytes)},
			{"project_tracks",4096},{"project_clips",4096},{"automation_bindings",65536},{"response_bytes",65536},
			{"import_file_bytes",QString::number(importLimits.maxFileBytes)},{"import_decoded_bytes",QString::number(importLimits.maxDecodedBytes)}}}});
}
}
