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
		|| action=="cancel_vocal_job" || action=="get_vocal_analysis";
}
inline bool only(const QJsonObject& object,const QSet<QString>& keys)
{for(auto it=object.begin();it!=object.end();++it)if(!keys.contains(it.key()))return false;return true;}
inline bool id(const QJsonValue& value)
{return value.isString() && !value.toString().isEmpty() && value.toString().size()<=256 && value.toString().toUtf8().size()<=256;}
inline bool integer(const QJsonValue& value,double low,double high)
{const auto n=value.toDouble();return value.isDouble() && std::isfinite(n) && n==std::floor(n) && n>=low && n<=high;}
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
inline bool valid(const QString& action,const QJsonObject& p)
{
	if(action=="get_vocal_capabilities")return p.isEmpty();
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
