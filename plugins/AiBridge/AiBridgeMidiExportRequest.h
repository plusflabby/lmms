#ifndef LMMS_AIBRIDGE_MIDI_EXPORT_REQUEST_H
#define LMMS_AIBRIDGE_MIDI_EXPORT_REQUEST_H

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <cmath>

namespace lmms
{
namespace MidiExportRequest
{
inline bool handles(const QString& action)
{
	return action == QStringLiteral("inspect_midi_export") || action == QStringLiteral("export_midi")
		|| action == QStringLiteral("get_midi_export_job") || action == QStringLiteral("cancel_midi_export_job");
}

inline bool integer(const QJsonValue& value)
{
	return value.isDouble() && std::isfinite(value.toDouble()) && std::floor(value.toDouble()) == value.toDouble();
}

inline bool hasOnlyKeys(const QJsonObject& object,const QSet<QString>& allowed)
{
	for(auto it=object.begin();it!=object.end();++it)if(!allowed.contains(it.key()))return false;
	return true;
}

inline bool operationId(const QJsonValue& value)
{
	static const QRegularExpression uuid(QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-8][0-9a-fA-F]{3}-[89abAB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}$"));
	return value.isString() && uuid.match(value.toString()).hasMatch();
}

inline bool selectedIds(const QJsonValue& value, const QString& key)
{
	if (!value.isObject()) return false;
	const auto object=value.toObject();
	if (object.size()!=2 || object.value(QStringLiteral("mode")).toString()!=(key==QStringLiteral("track_ids")?QStringLiteral("tracks"):QStringLiteral("clips")) || !object.value(key).isArray()) return false;
	const auto ids=object.value(key).toArray();
	if(ids.isEmpty() || ids.size()>256) return false;
	QSet<QString> unique;
	for(const auto& id:ids){if(!id.isString() || id.toString().isEmpty() || id.toString().size()>512 || unique.contains(id.toString()))return false;unique.insert(id.toString());}
	return true;
}

inline bool commonOptions(const QJsonObject& parameters, const QSet<QString>& base)
{
	QSet<QString> allowed=base;
	for(const auto& key:{QStringLiteral("selection"),QStringLiteral("range"),QStringLiteral("normalize_to_zero"),QStringLiteral("fidelity"),QStringLiteral("include_muted")})allowed.insert(key);
	for(auto it=parameters.begin();it!=parameters.end();++it)if(!allowed.contains(it.key()))return false;
	if(parameters.contains(QStringLiteral("selection")))
	{
		const auto selection=parameters.value(QStringLiteral("selection"));
		if(!selection.isObject())return false;
		const auto object=selection.toObject();
		const auto mode=object.value(QStringLiteral("mode")).toString();
		if(mode==QStringLiteral("song")){if(object.size()!=1)return false;}
		else if(mode==QStringLiteral("tracks")){if(!selectedIds(selection,QStringLiteral("track_ids")))return false;}
		else if(mode==QStringLiteral("clips")){if(!selectedIds(selection,QStringLiteral("clip_ids")))return false;}
		else return false;
	}
	if(parameters.contains(QStringLiteral("range")))
	{
		const auto range=parameters.value(QStringLiteral("range"));
		if(!range.isObject())return false;
		const auto object=range.toObject();
		if(object.size()!=2 || !integer(object.value(QStringLiteral("start_tick"))) || !integer(object.value(QStringLiteral("end_tick"))))return false;
		const auto start=object.value(QStringLiteral("start_tick")).toDouble(),end=object.value(QStringLiteral("end_tick")).toDouble();
		if(start<0 || end<=start || end>8000000)return false;
	}
	if(parameters.contains(QStringLiteral("normalize_to_zero"))&&!parameters.value(QStringLiteral("normalize_to_zero")).isBool())return false;
	if(parameters.contains(QStringLiteral("include_muted"))&&!parameters.value(QStringLiteral("include_muted")).isBool())return false;
	if(parameters.contains(QStringLiteral("fidelity"))&&!QSet<QString>{QStringLiteral("strict"),QStringLiteral("best_effort")}.contains(parameters.value(QStringLiteral("fidelity")).toString()))return false;
	return true;
}

inline bool valid(const QString& action,const QJsonObject& parameters)
{
	if(action==QStringLiteral("inspect_midi_export"))
		return parameters.contains(QStringLiteral("operation_id"))&&operationId(parameters.value(QStringLiteral("operation_id")))
			&&commonOptions(parameters,{QStringLiteral("operation_id")});
	if(action==QStringLiteral("export_midi"))
	{
		if(!parameters.contains(QStringLiteral("operation_id"))||!operationId(parameters.value(QStringLiteral("operation_id")))
			||!parameters.value(QStringLiteral("filename")).isString())return false;
		const auto name=parameters.value(QStringLiteral("filename")).toString();
		static const QRegularExpression reserved(QStringLiteral("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\\.|$)"),QRegularExpression::CaseInsensitiveOption);
		if(name.size()<5 || name.size()>180 || !name.endsWith(QStringLiteral(".mid"),Qt::CaseInsensitive)
			|| name.contains(QRegularExpression(QStringLiteral("[\\x00-\\x1f<>:\"/\\\\|?*]")))
			|| name.endsWith(QLatin1Char(' ')) || name.endsWith(QLatin1Char('.')) || reserved.match(name).hasMatch())return false;
		QSet<QString> base{QStringLiteral("operation_id"),QStringLiteral("filename")};
		if(parameters.contains(QStringLiteral("overwrite"))&&!parameters.value(QStringLiteral("overwrite")).isBool())return false;
		if(parameters.contains(QStringLiteral("overwrite")))base.insert(QStringLiteral("overwrite"));
		return commonOptions(parameters,base);
	}
	if(action==QStringLiteral("cancel_midi_export_job"))
		return parameters.size()==1&&operationId(parameters.value(QStringLiteral("operation_id")));
	if(action==QStringLiteral("get_midi_export_job"))
	{
		const auto mode=parameters.value(QStringLiteral("mode")).toString();
		if(mode==QStringLiteral("list"))return parameters.size()==1;
		if(!operationId(parameters.value(QStringLiteral("operation_id"))))return false;
		if(mode==QStringLiteral("release"))return parameters.size()==2;
		if(mode!="status")return false;
		if(!hasOnlyKeys(parameters,{QStringLiteral("mode"),QStringLiteral("operation_id"),QStringLiteral("section"),QStringLiteral("offset"),QStringLiteral("limit")}))return false;
		if(parameters.contains(QStringLiteral("section"))&&!QSet<QString>{QStringLiteral("summary"),QStringLiteral("tracks"),QStringLiteral("issues")}.contains(parameters.value(QStringLiteral("section")).toString()))return false;
		if(parameters.contains(QStringLiteral("offset"))&&(!integer(parameters.value(QStringLiteral("offset")))||parameters.value(QStringLiteral("offset")).toDouble()<0||parameters.value(QStringLiteral("offset")).toDouble()>2147483647))return false;
		if(parameters.contains(QStringLiteral("limit"))&&(!integer(parameters.value(QStringLiteral("limit")))||parameters.value(QStringLiteral("limit")).toDouble()<1||parameters.value(QStringLiteral("limit")).toDouble()>64))return false;
		return true;
	}
	return false;
}
}
}

#endif
