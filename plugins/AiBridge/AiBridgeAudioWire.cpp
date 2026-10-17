#include "AiBridgeAudioWire.h"
#include "AiBridgeAudioActions.h"
#include "AiBridgeAudioRangeActions.h"
#include "AudioWindowPlayback.h"
#include "AudioEngine.h"
#include "AudioImportPolicy.h"
#include "SampleDecoder.h"
#include "Engine.h"
#include "Song.h"
#include <QJsonArray>
#include <QSet>
namespace lmms {
namespace {
AiBridgeAudioEdit edit(const QString& kind,const QJsonObject& p)
{
	AiBridgeAudioEdit r;r.kind=kind=="move"?AiBridgeAudioEditKind::Move:kind=="duplicate"?AiBridgeAudioEditKind::Duplicate:AiBridgeAudioEditKind::Delete;
	r.clipId=p.value("clip_id").toString();r.expectedAudioStateHash=p.value("expected_audio_state_hash").toString();
	r.destinationTrackId=p.value("destination_track_id").toString();if(p.contains("position_ticks"))r.position=qint64(p.value("position_ticks").toDouble());return r;
}
QJsonObject actionJson(const AiBridgeAudioActionResult& r)
{
	return {{"track_id",r.trackId},{"clip_id",r.clipId},{"preview",r.preview},{"observed",r.observed},
		{"applied",QJsonArray::fromStringList(r.applied)},{"failed",QJsonArray::fromStringList(r.failed)},
		{"unapplied",QJsonArray::fromStringList(r.unapplied)},{"failure_code",r.failureCode}};
}
}
bool AiBridgeAudioWire::handles(const QString& action)
{
	return QSet<QString>{"get_audio_capabilities","get_audio_clip","create_audio_track","import_audio_file","move_audio_clip","duplicate_audio_clip","delete_audio_clip","batch_edit_audio_clips","trim_audio_clip","split_audio_clip"}.contains(action);
}
AiBridgeProtocolResponse AiBridgeAudioWire::execute(const AiBridgeProtocolRequest& request)
{
	const auto& p=request.parameters;const bool dry=p.value("dry_run").toBool(false);QString code="ok";QJsonObject data;
	if(request.action=="get_audio_capabilities")
	{
		const auto status=AudioImportPolicy::configured().refresh();const auto limits=SampleDecoder::configuredImportLimits();
		data={{"import_policy_status",status},{"import_roots_configured",status=="ok"},{"max_file_bytes",QString::number(limits.maxFileBytes)},
			{"max_decoded_bytes",QString::number(limits.maxDecodedBytes)},{"import_format","pcm16_fixed_header_wave_mono_stereo"},
			{"placement_unit","integer_ticks"},{"ticks_per_bar",TimePos::ticksPerBar()},
			{"ticks_per_beat",TimePos::ticksPerBar()/Engine::getSong()->getTimeSigModel().getNumerator()},
			{"max_affected_clips",32},{"max_batch_steps",256},{"max_preview_rows",128},{"max_path_utf16_units",1024},
			{"max_frame_bytes",AiBridgeProtocol::MaxFrameBytes},{"trim_supported",verifyAudioWindowReferenceProfile()},{"split_supported",verifyAudioWindowReferenceProfile()},
			{"range_editing",QJsonObject{{"xml_version",2},{"phase","linear_equal_ref256_v2"},{"reference_tempo_bpm",125},
				{"source_sample_rates",QJsonArray{44100,48000}},{"equal_rates_required",true},{"ticks_per_beat",48},
				{"forward_only",true},{"authorized_source_required",true},{"reference_refill_frames",256},
				{"excluded_reference_remainder",1},{"unsupported_state_behavior","quarantine"},
				{"current_output_sample_rate",int(Engine::audioEngine()->outputSampleRate())}}}};
	}
	else if(request.action=="get_audio_clip")
	{
		const auto r=AiBridgeAudioContext{}.clip(p.value("clip_id").toString());code=r.code;if(r.value)data=AiBridgeAudioContext::toJson(*r.value);
	}
	else if(request.action=="trim_audio_clip" || request.action=="split_audio_clip")
	{
		const auto kind=request.action=="split_audio_clip"?AiBridgeAudioRangeKind::Split:
			p.value("edge")=="left"?AiBridgeAudioRangeKind::TrimLeft:AiBridgeAudioRangeKind::TrimRight;
		const auto boundary=p.value(request.action=="split_audio_clip"?"split_tick":"new_edge_tick").toDouble();
		const auto plan=prepareAudioRange({kind,p.value("clip_id").toString(),p.value("expected_audio_state_hash").toString(),qint64(boundary),dry});
		const auto result=executeAudioRange(plan,dry);code=result.code;data=result.toJson();data.insert("dry_run",dry);
	}
	else if(request.action=="create_audio_track" || request.action=="import_audio_file")
	{
		AiBridgeAudioActionResult r;
		if(request.action=="create_audio_track")r=AiBridgeAudioActions{}.createTrack({p.value("name").toString(),p.value("volume").toDouble(100),p.value("panning").toDouble(0),p.value("muted").toBool(false),dry});
		else r=AiBridgeAudioActions{}.importFile({p.value("track_id").toString(),p.value("path").toString(),qint64(p.value("position_ticks").toDouble()),dry});
		code=r.code;data=actionJson(r);data.insert("dry_run",dry);
	}
	else
	{
		std::vector<AiBridgeAudioEdit> edits;
		if(request.action=="batch_edit_audio_clips")for(const auto& value:p.value("operations").toArray()){const auto op=value.toObject();edits.push_back(edit(op.value("operation").toString(),op));}
		else edits.push_back(edit(request.action=="move_audio_clip"?"move":request.action=="duplicate_audio_clip"?"duplicate":"delete",p));
		const auto r=AiBridgeAudioActions{}.batchEdit(edits,dry);code=r.code;data=r.toJson();data.insert("dry_run",dry);
	}
	return {2,request.requestId,code=="ok" || code=="no_effect",code,QStringLiteral("Audio request completed"),data};
}
}
