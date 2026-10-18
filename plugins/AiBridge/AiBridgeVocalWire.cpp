#include "AiBridgeVocalWire.h"
#include "AiBridgeVocalRequest.h"
#include "AiBridgeVocalContext.h"
#include "AiBridgeVocalTrackActions.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "SampleTrack.h"
#include "Song.h"
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
	case VocalJobPhase::Analyzing:return "analyzing";case VocalJobPhase::Publishing:return "publishing";}return "authorizing";
}
QJsonObject jobJson(const VocalJobSnapshot& job)
{
	QJsonObject result{{"job_id",job.jobId},{"input_hash",job.inputHash},{"project_generation",QString::number(job.generation)},
		{"state",stateName(job.state)},{"phase",phaseName(job.phase)}};
	if(job.progress)result["progress"]=*job.progress;
	if(!job.analysisId.isEmpty())result["analysis_id"]=job.analysisId;
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
	return reply("ok",QJsonObject{{"basis","decoded_source_pre_mixer"},{"algorithm","levels_v1"},{"source_validation","job_snapshot"},
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
