#include "AiBridgeMidiExportWire.h"

#include "AiBridgeMidiExportRequest.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>

namespace lmms
{
namespace
{
MidiExportSelection selectionFrom(const QJsonObject& parameters)
{
	MidiExportSelection selection;
	const auto input=parameters.value(QStringLiteral("selection")).toObject();
	const auto mode=input.value(QStringLiteral("mode")).toString(QStringLiteral("song"));
	if(mode==QStringLiteral("tracks"))
	{
		selection.mode=MidiExportSelectionMode::Tracks;
		for(const auto& id:input.value(QStringLiteral("track_ids")).toArray())selection.trackIds.append(id.toString());
	}
	else if(mode==QStringLiteral("clips"))
	{
		selection.mode=MidiExportSelectionMode::Clips;
		for(const auto& id:input.value(QStringLiteral("clip_ids")).toArray())selection.clipIds.append(id.toString());
	}
	if(parameters.contains(QStringLiteral("range")))
	{
		const auto range=parameters.value(QStringLiteral("range")).toObject();selection.hasRange=true;
		selection.rangeStart=range.value(QStringLiteral("start_tick")).toVariant().toLongLong();
		selection.rangeEnd=range.value(QStringLiteral("end_tick")).toVariant().toLongLong();
	}
	selection.normalizeToZero=parameters.value(QStringLiteral("normalize_to_zero")).toBool(false);
	selection.includeMuted=parameters.value(QStringLiteral("include_muted")).toBool(false);
	selection.fidelity=parameters.value(QStringLiteral("fidelity")).toString()==QStringLiteral("best_effort")
		?MidiExportFidelity::BestEffort:MidiExportFidelity::Strict;
	return selection;
}

AiBridgeProtocolResponse reply(const AiBridgeProtocolRequest& request,const QString& code,const QJsonObject& data={})
{
	const auto success=code==QStringLiteral("ok");
	AiBridgeProtocolResponse result{2,request.requestId,success,code,success?QStringLiteral("MIDI export operation accepted"):QStringLiteral("MIDI export operation rejected"),
		data.isEmpty()?QJsonValue(QJsonValue::Undefined):QJsonValue(data)};
	if(QJsonDocument::fromJson(AiBridgeProtocol::serializeResponse(result)).toJson().size()>AiBridgeProtocol::MaxFrameBytes)
		return {2,request.requestId,false,QStringLiteral("result_too_large"),QStringLiteral("MIDI export result exceeds the IPC response limit")};
	return result;
}
}

bool AiBridgeMidiExportWire::handles(const QString& action){return MidiExportRequest::handles(action);}

AiBridgeProtocolResponse AiBridgeMidiExportWire::execute(const AiBridgeProtocolRequest& request,QPointer<AiBridgeMidiExportJobs> jobs)
{
	if(!QCoreApplication::instance()||QThread::currentThread()!=QCoreApplication::instance()->thread())return reply(request,"wrong_thread");
	if(!handles(request.action)||!MidiExportRequest::valid(request.action,request.parameters))return reply(request,"invalid_parameter");
	if(!jobs)return reply(request,"job_invalidated");
	const auto& p=request.parameters;
	if(request.action==QStringLiteral("inspect_midi_export")||request.action==QStringLiteral("export_midi"))
	{
		const bool exportFile=request.action==QStringLiteral("export_midi");
		auto normalized=p;
		if(!normalized.contains("selection"))normalized.insert("selection",QJsonObject{{"mode","song"}});
		if(!normalized.contains("normalize_to_zero"))normalized.insert("normalize_to_zero",false);
		if(!normalized.contains("fidelity"))normalized.insert("fidelity","strict");
		if(!normalized.contains("include_muted"))normalized.insert("include_muted",false);
		if(exportFile&&!normalized.contains("overwrite"))normalized.insert("overwrite",false);
		if(exportFile&&!normalized.contains("filename"))return reply(request,"invalid_parameter");
		const auto start=jobs->start(p.value("operation_id").toString(),selectionFrom(p),exportFile,
			p.value("filename").toString(),p.value("overwrite").toBool(false),normalized);
		if(!jobs)return reply(request,"job_invalidated");
		return start.code=="ok"?reply(request,"ok",start.data):reply(request,start.code);
	}
	if(request.action==QStringLiteral("cancel_midi_export_job"))
	{
		const auto result=jobs->cancel(p.value("operation_id").toString());
		return result.code=="ok"?reply(request,"ok",result.data):reply(request,result.code);
	}
	if(request.action==QStringLiteral("get_midi_export_job"))
	{
		MidiExportQueryResult result;
		if(p.value("mode")==QStringLiteral("list"))result=jobs->list();
		else if(p.value("mode")==QStringLiteral("release"))result=jobs->release(p.value("operation_id").toString());
		else result=jobs->status(p.value("operation_id").toString(),p.value("section").toString(QStringLiteral("summary")),
			p.value("offset").toInt(0),p.value("limit").toInt(32));
		return result.code=="ok"?reply(request,"ok",result.data):reply(request,result.code);
	}
	return reply(request,"unknown_action");
}

} // namespace lmms
