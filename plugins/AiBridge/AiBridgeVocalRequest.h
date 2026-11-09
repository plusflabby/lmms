#ifndef LMMS_AI_BRIDGE_VOCAL_REQUEST_H
#define LMMS_AI_BRIDGE_VOCAL_REQUEST_H
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <cmath>
#include <limits>
namespace lmms::VocalRequest {
inline bool handles(const QString& action)
{
	return action=="get_vocal_capabilities" || action=="analyze_vocal_audio" || action=="get_vocal_job"
		|| action=="cancel_vocal_job" || action=="get_vocal_analysis" || action=="plan_vocal_edit"
		|| action=="render_vocal_variant" || action=="get_vocal_variant" || action=="apply_vocal_variant";
}
inline bool only(const QJsonObject& object,const QSet<QString>& keys)
{for(auto it=object.begin();it!=object.end();++it)if(!keys.contains(it.key()))return false;return true;}
inline bool id(const QJsonValue& value)
{return value.isString() && !value.toString().isEmpty() && value.toString().size()<=256 && value.toString().toUtf8().size()<=256;}
inline bool integer(const QJsonValue& value,double low,double high)
{const auto n=value.toDouble();constexpr double maxSafeInteger=9007199254740991.0;
	return value.isDouble() && std::isfinite(n) && n==std::floor(n) && n>=low && n<=high
		&& n>=-maxSafeInteger && n<=maxSafeInteger;}
inline bool frame(const QJsonValue& value)
{
	if(!value.isString())return false;
	const auto text=value.toString();bool ok=false;const auto number=text.toLongLong(&ok);
	return ok && number>=0 && QString::number(number)==text;
}
inline bool hash(const QJsonValue& value)
{
	if(!value.isString() || value.toString().size()!=64)return false;
	for(const auto ch:value.toString())if(!((ch>='0' && ch<='9') || (ch>='a' && ch<='f')))return false;
	return true;
}
inline bool uuid(const QJsonValue& value)
{
	if(!value.isString() || value.toString().size()!=36)return false;
	const auto text=value.toString();
	for(int i=0;i<text.size();++i)
	{
		if(i==8 || i==13 || i==18 || i==23){if(text[i]!='-')return false;continue;}
		if(!((text[i]>='0' && text[i]<='9') || (text[i]>='a' && text[i]<='f')))return false;
	}
	return true;
}
inline bool decimal64(const QJsonValue& value,bool nonzero=false)
{
	if(!value.isString())return false;
	const auto text=value.toString();
	if(text.isEmpty() || text.size()>20 || (text.size()>1 && text[0]=='0'))return false;
	for(const auto ch:text)if(ch<'0' || ch>'9')return false;
	bool ok=false;const auto number=text.toULongLong(&ok);
	return ok && (!nonzero || number!=0) && QString::number(number)==text;
}
inline bool digestRange(const QJsonValue& value)
{
	if(!value.isObject())return false;
	const auto range=value.toObject();
	return only(range,{"in_frame","out_frame"}) && frame(range["in_frame"]) && frame(range["out_frame"])
		&& range["out_frame"].toString().toLongLong()>range["in_frame"].toString().toLongLong();
}
inline bool finiteRange(const QJsonValue& value,double low,double high)
{
	if(!value.isDouble() || !std::isfinite(value.toDouble()))return false;
	return value.toDouble()>=low && value.toDouble()<=high;
}
inline bool operation(const QJsonValue& value,int selections,int& gainFadePoints,int& timingAnchors,bool& hasTiming)
{
	if(!value.isObject())return false;
	const auto op=value.toObject();
	const auto kind=op["operation"].toString();
	if(kind=="gain_envelope")
	{
		if(!only(op,{"operation","points"}) || !op["points"].isArray())return false;
		const auto points=op["points"].toArray();
		if(points.size()<2 || points.size()>1024 || gainFadePoints>1024-points.size())return false;
		for(const auto& item:points)
		{
			if(!item.isObject())return false;
			const auto point=item.toObject();
			if(!only(point,{"render_input_frame","gain_db"}) || !frame(point["render_input_frame"])
				|| !finiteRange(point["gain_db"],-48.0,24.0))return false;
		}
		gainFadePoints+=points.size();return true;
	}
	if(kind=="fade")
	{
		if(!only(op,{"operation","render_input_frames","start_gain_db","end_gain_db","curve"})
			|| !digestRange(op["render_input_frames"]) || !finiteRange(op["start_gain_db"],-48.0,24.0)
			|| !finiteRange(op["end_gain_db"],-48.0,24.0) || !op["curve"].isString()
			|| (op["curve"]!="linear_amplitude" && op["curve"]!="equal_power"))return false;
		gainFadePoints+=2;if(gainFadePoints>1024)return false;
		return true;
	}
	if(kind!="timing_map" || hasTiming
		|| !only(op,{"operation","anchors","strength","minimum_local_stretch_ratio","maximum_local_stretch_ratio",
			"protected_render_input_frames"}) || !op["anchors"].isArray()
		|| !op["protected_render_input_frames"].isArray()
		|| !finiteRange(op["strength"],0.0,1.0)
		|| !finiteRange(op["minimum_local_stretch_ratio"],0.80,1.25)
		|| !finiteRange(op["maximum_local_stretch_ratio"],0.80,1.25)
		|| op["minimum_local_stretch_ratio"].toDouble()>op["maximum_local_stretch_ratio"].toDouble())return false;
	const auto anchors=op["anchors"].toArray();
	if(anchors.size()<2 || anchors.size()>256 || timingAnchors>256-anchors.size())return false;
	for(const auto& item:anchors)
	{
		if(!item.isObject())return false;
		const auto anchor=item.toObject();
		if(!only(anchor,{"render_input_frame","target_output_frame","target_tick","pre_anchor_crossfade_frames","crossfade_law"})
			|| !frame(anchor["render_input_frame"]) || anchor.contains("target_output_frame")==anchor.contains("target_tick")
			|| (anchor.contains("target_output_frame") && !frame(anchor["target_output_frame"]))
			|| (anchor.contains("target_tick") && !integer(anchor["target_tick"],0,std::numeric_limits<qint64>::max()))
			|| !frame(anchor["pre_anchor_crossfade_frames"]) || !anchor["crossfade_law"].isString()
			|| (anchor["crossfade_law"]!="linear_amplitude" && anchor["crossfade_law"]!="equal_power"))return false;
	}
	const auto protectedRanges=op["protected_render_input_frames"].toArray();
	if(protectedRanges.size()>256)return false;
	for(const auto& range:protectedRanges)if(!digestRange(range))return false;
	timingAnchors+=anchors.size();hasTiming=true;(void)selections;return true;
}
inline bool editPlan(const QJsonObject& p)
{
	if(!decimal64(p["expected_project_generation"],true) || !hash(p["expected_world_hash"])
		|| !p["recipe_version"].isDouble() || p["recipe_version"].toInt(-1)!=1
		|| !p["selections"].isArray() || !p["operations"].isArray() || !p["input_kind"].isString())return false;
	const auto selections=p["selections"].toArray();
	if(selections.isEmpty() || selections.size()>8)return false;
	QSet<QString> seen;
	for(const auto& item:selections)
	{
		if(!item.isObject())return false;
		const auto selection=item.toObject();
		if(!only(selection,{"clip_id","expected_audio_state_hash","source_basis","source_frames"})
			|| !id(selection["clip_id"]) || !hash(selection["expected_audio_state_hash"])
			|| !selection["source_basis"].isString()
			|| (selection["source_basis"]!="source_pcm" && selection["source_basis"]!="qualified_v2_reference")
			|| !digestRange(selection["source_frames"]) || seen.contains(selection["clip_id"].toString()))return false;
		seen.insert(selection["clip_id"].toString());
	}
	const auto inputKind=p["input_kind"].toString();
	if(inputKind=="direct_source")
	{
		if(!only(p,{"recipe_version","expected_project_generation","expected_world_hash","selections","input_kind",
			"input_selection_index","operations"}) || selections.size()!=1
			|| !integer(p["input_selection_index"],0,selections.size()-1))return false;
	}
	else if(inputKind=="comp")
	{
		if(!only(p,{"recipe_version","expected_project_generation","expected_world_hash","selections","input_kind","comp","operations"})
			|| selections.size()<2 || !p["comp"].isObject())return false;
		const auto comp=p["comp"].toObject();
		if(!only(comp,{"segments","boundaries"}) || !comp["segments"].isArray() || !comp["boundaries"].isArray())return false;
		const auto segments=comp["segments"].toArray(),boundaries=comp["boundaries"].toArray();
		if(segments.size()<2 || segments.size()>8 || boundaries.size()>7)return false;
		for(const auto& item:segments)
		{
			if(!item.isObject())return false;
			const auto segment=item.toObject();
			if(!only(segment,{"selection_index","source_frames","output_frames"})
				|| !integer(segment["selection_index"],0,selections.size()-1)
				|| !digestRange(segment["source_frames"]) || !digestRange(segment["output_frames"]))return false;
		}
		for(const auto& item:boundaries)
		{
			if(!item.isObject())return false;
			const auto boundary=item.toObject();
			if(!only(boundary,{"left_segment_index","right_segment_index","overlap_frames","law"})
				|| !integer(boundary["left_segment_index"],0,6) || !integer(boundary["right_segment_index"],1,7)
				|| !frame(boundary["overlap_frames"]) || !boundary["law"].isString()
				|| (boundary["law"]!="linear_amplitude" && boundary["law"]!="equal_power"))return false;
		}
	}
	else return false;
	const auto operations=p["operations"].toArray();
	if(operations.isEmpty() || operations.size()>32)return false;
	int pointCount=0,anchorCount=0;bool timing=false;
	for(const auto& item:operations)
	{
		if(timing && item.toObject().value(QStringLiteral("operation"))!=QStringLiteral("timing_map"))return false;
		if(!operation(item,selections.size(),pointCount,anchorCount,timing))return false;
	}
	return true;
}
inline bool valid(const QString& action,const QJsonObject& p)
{
	if(action=="get_vocal_capabilities")return p.isEmpty();
	if(action=="plan_vocal_edit")return editPlan(p);
	if(action=="render_vocal_variant")return only(p,{"plan_id","plan_hash","dry_run"})
		&& uuid(p["plan_id"]) && hash(p["plan_hash"]) && p["dry_run"].isBool();
	if(action=="get_vocal_variant")return only(p,{"variant_id"}) && uuid(p["variant_id"]);
	if(action=="apply_vocal_variant")
	{
		const auto mode=p["mode"].toString();
		const QSet<QString> common{"variant_id","clip_id","mode","expected_project_generation","expected_world_hash",
			"expected_audio_state_hash","expected_track_state_hash","dry_run"};
		if(!uuid(p["variant_id"]) || !id(p["clip_id"]) || !decimal64(p["expected_project_generation"],true)
			|| !hash(p["expected_world_hash"]) || !hash(p["expected_audio_state_hash"])
			|| !hash(p["expected_track_state_hash"]) || !p["dry_run"].isBool())return false;
		if(mode=="compare")return only(p,common+QSet<QString>{"destination_track_id","expected_destination_track_state_hash"})
			&& id(p["destination_track_id"]) && hash(p["expected_destination_track_state_hash"]);
		return (mode=="replace" || mode=="revert") && only(p,common);
	}
	if(action=="cancel_vocal_job")return only(p,{"job_id"}) && id(p["job_id"]);
	if(action=="get_vocal_job")return only(p,{"job_id","mode"}) && id(p["job_id"])
		&& p["mode"].isString() && (p["mode"]=="status" || p["mode"]=="release");
	if(action=="get_vocal_analysis")return only(p,{"analysis_id","offset","limit"}) && id(p["analysis_id"])
		&& (!p.contains("offset") || integer(p["offset"],0,std::numeric_limits<int>::max()))
		&& (!p.contains("limit") || integer(p["limit"],1,256));
	if(action!="analyze_vocal_audio" || !only(p,{"kind","selections","settings"})
		|| (p.contains("kind") && (!p["kind"].isString() || p["kind"]!="levels")) || !p["selections"].isArray())return false;
	const auto selections=p["selections"].toArray();if(selections.isEmpty() || selections.size()>8)return false;
	QSet<QString> seen;
	for(const auto& value:selections)
	{
		if(!value.isObject())return false;
		const auto s=value.toObject();
		if(!only(s,{"clip_id","expected_audio_state_hash","source_frames"}) || !id(s["clip_id"])
			|| !hash(s["expected_audio_state_hash"]) || !s["source_frames"].isObject() || seen.contains(s["clip_id"].toString()))return false;
		seen.insert(s["clip_id"].toString());const auto range=s["source_frames"].toObject();
		if(!only(range,{"in_frame","out_frame"}) || !frame(range["in_frame"]) || !frame(range["out_frame"])
			|| range["out_frame"].toString().toLongLong()<=range["in_frame"].toString().toLongLong())return false;
	}
	if(p.contains("settings"))
	{
		if(!p["settings"].isObject())return false;
		const auto s=p["settings"].toObject();if(!only(s,{"low_level_dbfs","minimum_low_level_ms"}))return false;
		if(s.contains("low_level_dbfs") && (!s["low_level_dbfs"].isDouble() || !std::isfinite(s["low_level_dbfs"].toDouble())
			|| s["low_level_dbfs"].toDouble()< -96 || s["low_level_dbfs"].toDouble()> -20))return false;
		if(s.contains("minimum_low_level_ms") && !integer(s["minimum_low_level_ms"],50,2000))return false;
	}
	return true;
}
}
#endif
