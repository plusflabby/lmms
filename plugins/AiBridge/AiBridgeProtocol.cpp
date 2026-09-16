#include "AiBridgeProtocol.h"
#include "AiBridgeMutationTypes.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QSet>

#include <cmath>

namespace lmms
{

namespace
{

AiBridgeProtocolParseResult failure(const QString &code, const QString &message)
{
	return {false, code, message, {}};
}

bool isInteger(const QJsonValue &value)
{
	if (!value.isDouble()) return false;
	const auto number = value.toDouble();
	return std::isfinite(number) && std::floor(number) == number;
}

bool isFiniteNumber(const QJsonValue &value)
{
	return value.isDouble() && std::isfinite(value.toDouble());
}

bool hasOnlyKeys(const QJsonObject &object, const QSet<QString> &allowed)
{
	for (auto it = object.constBegin(); it != object.constEnd(); ++it)
	{
		if (!allowed.contains(it.key())) return false;
	}
	return true;
}

bool validPaging(const QJsonObject &parameters)
{
	if (!hasOnlyKeys(parameters, {QStringLiteral("offset"), QStringLiteral("limit")})) return false;
	for (const auto key : {QStringLiteral("offset"), QStringLiteral("limit")})
		if (parameters.contains(key) && (!isInteger(parameters.value(key)) || parameters.value(key).toInt() < 0 || parameters.value(key).toInt() > 256)) return false;
	return true;
}

bool parseNote(const QJsonValue &value)
{
	if (!value.isObject()) return false;
	const auto object = value.toObject();
	if (!hasOnlyKeys(object, {QStringLiteral("position_ticks"), QStringLiteral("length_ticks"), QStringLiteral("key"), QStringLiteral("volume"), QStringLiteral("panning"), QStringLiteral("note_type")})) return false;
	for (const auto &key : {QStringLiteral("position_ticks"), QStringLiteral("length_ticks"), QStringLiteral("key"), QStringLiteral("volume"), QStringLiteral("panning")})
		if (!object.contains(key) || !isInteger(object.value(key))) return false;
	AiBridgeCopiedNote note;
	note.position_ticks = object.value(QStringLiteral("position_ticks")).toVariant().toLongLong();
	note.length_ticks = object.value(QStringLiteral("length_ticks")).toVariant().toLongLong();
	note.key = object.value(QStringLiteral("key")).toInt();
	note.volume = object.value(QStringLiteral("volume")).toInt();
	note.panning = object.value(QStringLiteral("panning")).toInt();
	note.note_type = object.value(QStringLiteral("note_type")).toString();
	return AiBridgeMutationTypes::validateNoteInput(note).success();
}

bool validSelection(const QJsonValue &value)
{
	if (!value.isObject()) return false;
	const auto object = value.toObject();
	AiBridgeNoteSelection selection;
	for (const auto &key : {QStringLiteral("position_start"), QStringLiteral("position_end")})
		if (object.contains(key) && !isInteger(object.value(key))) return false;
	if (object.contains(QStringLiteral("position_start"))) selection.position_start=object.value(QStringLiteral("position_start")).toVariant().toLongLong();
	if (object.contains(QStringLiteral("position_end"))) selection.position_end=object.value(QStringLiteral("position_end")).toVariant().toLongLong();
	for (const auto &key : {QStringLiteral("key_min"), QStringLiteral("key_max")})
		if (object.contains(key) && (!isInteger(object.value(key)) || object.value(key).toInt()<0 || object.value(key).toInt()>127)) return false;
	if (object.contains(QStringLiteral("key_min"))) selection.key_min=object.value(QStringLiteral("key_min")).toInt();
	if (object.contains(QStringLiteral("key_max"))) selection.key_max=object.value(QStringLiteral("key_max")).toInt();
	if (object.contains(QStringLiteral("exact")))
	{
		if (!object.value(QStringLiteral("exact")).isObject()) return false;
		const auto exactObject=object.value(QStringLiteral("exact")).toObject();
		AiBridgeExactNote exact;
		for (const auto &key : {QStringLiteral("position"), QStringLiteral("length"), QStringLiteral("key"), QStringLiteral("volume"), QStringLiteral("panning")})
			if (exactObject.contains(key) && !isInteger(exactObject.value(key))) return false;
		if (exactObject.contains(QStringLiteral("position"))) exact.position=exactObject.value(QStringLiteral("position")).toVariant().toLongLong();
		if (exactObject.contains(QStringLiteral("length"))) exact.length=exactObject.value(QStringLiteral("length")).toVariant().toLongLong();
		if (exactObject.contains(QStringLiteral("key"))) exact.key=exactObject.value(QStringLiteral("key")).toInt();
		if (exactObject.contains(QStringLiteral("volume"))) exact.volume=exactObject.value(QStringLiteral("volume")).toInt();
		if (exactObject.contains(QStringLiteral("panning"))) exact.panning=exactObject.value(QStringLiteral("panning")).toInt();
		if (exactObject.contains(QStringLiteral("note_type"))) { if (!exactObject.value(QStringLiteral("note_type")).isString()) return false; exact.note_type=exactObject.value(QStringLiteral("note_type")).toString(); }
		selection.exact=exact;
	}
	return AiBridgeMutationTypes::validateSelection(selection,true).success();
}

} // namespace

AiBridgeProtocolParseResult AiBridgeProtocol::parseRequest(const QByteArray &frame)
{
	if (frame.size() == 0 || frame.size() > MaxFrameBytes || !frame.endsWith('\n'))
		return failure(QStringLiteral("frame_too_large_or_incomplete"), QStringLiteral("Request frame must be newline terminated and at most 64 KiB"));

	const auto jsonBytes = frame.left(frame.size() - 1);
	if (QString::fromUtf8(jsonBytes).toUtf8() != jsonBytes)
		return failure(QStringLiteral("invalid_utf8"), QStringLiteral("Request is not valid UTF-8"));

	QJsonParseError parseError;
	const auto document = QJsonDocument::fromJson(jsonBytes, &parseError);
	if (parseError.error != QJsonParseError::NoError || !document.isObject())
		return failure(QStringLiteral("malformed_json"), QStringLiteral("Request must be a JSON object"));

	const auto object = document.object();
	if (!hasOnlyKeys(object, {QStringLiteral("version"), QStringLiteral("request_id"), QStringLiteral("action"), QStringLiteral("parameters")}))
		return failure(QStringLiteral("unknown_field"), QStringLiteral("Request contains an unknown field"));
	if (!object.contains(QStringLiteral("version")) || !isInteger(object.value(QStringLiteral("version"))) || object.value(QStringLiteral("version")).toInt() != 2)
		return failure(QStringLiteral("unsupported_version"), QStringLiteral("Only protocol version 2 is supported"));
	if (!object.contains(QStringLiteral("request_id")) || !object.value(QStringLiteral("request_id")).isString())
		return failure(QStringLiteral("invalid_request_id"), QStringLiteral("request_id must be a non-empty string"));
	const auto requestId = object.value(QStringLiteral("request_id")).toString();
	if (requestId.isEmpty() || requestId.toUtf8().size() > MaxRequestIdBytes)
		return failure(QStringLiteral("invalid_request_id"), QStringLiteral("request_id must be between 1 and 128 UTF-8 bytes"));
	if (!object.contains(QStringLiteral("action")) || !object.value(QStringLiteral("action")).isString())
		return failure(QStringLiteral("invalid_action"), QStringLiteral("action must be a string"));
	const auto action = object.value(QStringLiteral("action")).toString();
	if (!QSet<QString>{QStringLiteral("get_context"), QStringLiteral("set_tempo"), QStringLiteral("adjust_current_track_volume"), QStringLiteral("add_note_to_current_clip"), QStringLiteral("get_windows"), QStringLiteral("set_window_geometry"), QStringLiteral("focus_window"), QStringLiteral("arrange_windows"), QStringLiteral("get_project_structure"), QStringLiteral("list_tracks"), QStringLiteral("list_clips"), QStringLiteral("get_clip"), QStringLiteral("open_clip_in_piano_roll"), QStringLiteral("add_notes"), QStringLiteral("delete_notes"), QStringLiteral("transpose_notes"), QStringLiteral("move_notes"), QStringLiteral("duplicate_region"), QStringLiteral("create_instrument_track"), QStringLiteral("rename_track"), QStringLiteral("create_midi_clip"), QStringLiteral("set_clip_properties"), QStringLiteral("list_instruments"), QStringLiteral("set_track_instrument"), QStringLiteral("set_track_properties"), QStringLiteral("list_bundled_samples"), QStringLiteral("load_bundled_sample"), QStringLiteral("list_bundled_presets"), QStringLiteral("load_bundled_preset"), QStringLiteral("get_mixer_structure"), QStringLiteral("create_mixer_channel"), QStringLiteral("delete_mixer_channel"), QStringLiteral("set_mixer_channel_properties"), QStringLiteral("route_track_to_mixer_channel"), QStringLiteral("create_mixer_route"), QStringLiteral("delete_mixer_route"), QStringLiteral("set_mixer_route_properties"), QStringLiteral("list_native_effects"), QStringLiteral("add_effect"), QStringLiteral("remove_effect"), QStringLiteral("move_effect"), QStringLiteral("set_effect_state"), QStringLiteral("get_effect_parameters"), QStringLiteral("set_effect_parameters"), QStringLiteral("get_mixer_meters"), QStringLiteral("get_arrangement"), QStringLiteral("get_timeline_region"), QStringLiteral("set_timeline_clip"), QStringLiteral("duplicate_timeline_clip"), QStringLiteral("delete_timeline_clip"), QStringLiteral("batch_edit_timeline_clips"), QStringLiteral("list_automation_targets"), QStringLiteral("get_automation_capabilities"), QStringLiteral("list_automation"), QStringLiteral("get_automation_clip"), QStringLiteral("create_automation_track"), QStringLiteral("delete_automation_track"), QStringLiteral("create_automation_clip"), QStringLiteral("delete_automation_clip"), QStringLiteral("set_automation_clip_properties"), QStringLiteral("set_automation_clip_target"), QStringLiteral("edit_automation_points"), QStringLiteral("duplicate_arrangement_region"), QStringLiteral("move_arrangement_region"), QStringLiteral("delete_arrangement_region"), QStringLiteral("insert_arrangement_space"), QStringLiteral("close_arrangement_gap"), QStringLiteral("replace_arrangement_region")}.contains(action))
		return failure(QStringLiteral("unknown_action"), QStringLiteral("Action is not allowlisted"));
	if (!object.contains(QStringLiteral("parameters")) || !object.value(QStringLiteral("parameters")).isObject())
		return failure(QStringLiteral("invalid_parameters"), QStringLiteral("parameters must be an object"));

	const auto parameters = object.value(QStringLiteral("parameters")).toObject();
	const auto requestFailure = [&](const QString &code, const QString &message)
	{
		return AiBridgeProtocolParseResult{false, code, message, {2, requestId, action, parameters}};
	};
	if (action == QStringLiteral("get_context") && !parameters.isEmpty())
		return requestFailure(QStringLiteral("unknown_parameter"), QStringLiteral("get_context accepts no parameters"));
	if (action == QStringLiteral("set_tempo") && (!hasOnlyKeys(parameters, {QStringLiteral("bpm")}) || !isInteger(parameters.value(QStringLiteral("bpm")))))
		return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("set_tempo requires integer bpm"));
	if (action == QStringLiteral("adjust_current_track_volume") && (!hasOnlyKeys(parameters, {QStringLiteral("delta")}) || !parameters.value(QStringLiteral("delta")).isDouble()))
		return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("adjust_current_track_volume requires numeric delta"));
	if (action == QStringLiteral("add_note_to_current_clip") && (!hasOnlyKeys(parameters, {QStringLiteral("key"), QStringLiteral("position"), QStringLiteral("length"), QStringLiteral("velocity")}) || !isInteger(parameters.value(QStringLiteral("key"))) || !isInteger(parameters.value(QStringLiteral("position"))) || !isInteger(parameters.value(QStringLiteral("length"))) || !isInteger(parameters.value(QStringLiteral("velocity")))))
		return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("add_note_to_current_clip requires integer note parameters"));
	if ((action == QStringLiteral("focus_window") || action == QStringLiteral("set_window_geometry")) && (!parameters.value(QStringLiteral("id")).isString()))
		return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("window action requires string id"));
	if (action == QStringLiteral("focus_window") && !hasOnlyKeys(parameters, {QStringLiteral("id")}))
		return requestFailure(QStringLiteral("unknown_parameter"), QStringLiteral("focus_window accepts only id"));
	if (action == QStringLiteral("set_window_geometry") && (!hasOnlyKeys(parameters, {QStringLiteral("id"), QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("width"), QStringLiteral("height")}) || !isInteger(parameters.value(QStringLiteral("x"))) || !isInteger(parameters.value(QStringLiteral("y"))) || !isInteger(parameters.value(QStringLiteral("width"))) || !isInteger(parameters.value(QStringLiteral("height")))))
		return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("set_window_geometry requires integer geometry"));
	if (action == QStringLiteral("get_windows") && !parameters.isEmpty())
		return requestFailure(QStringLiteral("unknown_parameter"), QStringLiteral("get_windows accepts no parameters"));
	if (action == QStringLiteral("arrange_windows") && (!hasOnlyKeys(parameters, {QStringLiteral("strategy")}) || !parameters.value(QStringLiteral("strategy")).isString()))
		return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("arrange_windows requires strategy"));
	if (action == QStringLiteral("get_project_structure") && !parameters.isEmpty()) return requestFailure(QStringLiteral("unknown_parameter"), QStringLiteral("get_project_structure accepts no parameters"));
	if ((action == QStringLiteral("list_bundled_samples") || action == QStringLiteral("list_bundled_presets")) && !hasOnlyKeys(parameters, {QStringLiteral("instrument_id"), QStringLiteral("query"), QStringLiteral("category"), QStringLiteral("format"), QStringLiteral("offset"), QStringLiteral("limit")})) return requestFailure(QStringLiteral("unknown_parameter"), QStringLiteral("asset listing contains an unknown parameter"));
	if ((action == QStringLiteral("load_bundled_sample") || action == QStringLiteral("load_bundled_preset")) && (!hasOnlyKeys(parameters, {QStringLiteral("track_id"), QStringLiteral("asset_id")}) || !parameters.value(QStringLiteral("track_id")).isString() || !parameters.value(QStringLiteral("asset_id")).isString())) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("asset loading requires track_id and asset_id"));
	if (action == QStringLiteral("list_bundled_samples") || action == QStringLiteral("list_bundled_presets")) {
		for (const auto &key : {QStringLiteral("instrument_id"), QStringLiteral("query"), QStringLiteral("category"), QStringLiteral("format")}) if (parameters.contains(key) && (!parameters.value(key).isString() || parameters.value(key).toString().toUtf8().size() > 64)) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("asset filters are limited to 64 UTF-8 bytes"));
		QJsonObject paging; if (parameters.contains(QStringLiteral("offset"))) paging.insert(QStringLiteral("offset"), parameters.value(QStringLiteral("offset"))); if (parameters.contains(QStringLiteral("limit"))) paging.insert(QStringLiteral("limit"), parameters.value(QStringLiteral("limit"))); if (!validPaging(paging)) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("asset paging is invalid"));
	}
	if (action == QStringLiteral("list_tracks") && !validPaging(parameters)) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("list_tracks paging is invalid"));
	if (action == QStringLiteral("list_instruments") && !validPaging(parameters)) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("list_instruments paging is invalid"));
	if (action == QStringLiteral("list_clips") && (!parameters.contains(QStringLiteral("track_id")) || !parameters.value(QStringLiteral("track_id")).isString())) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("list_clips requires track_id"));
	if (action == QStringLiteral("get_clip") && (!parameters.contains(QStringLiteral("clip_id")) || !parameters.value(QStringLiteral("clip_id")).isString())) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("get_clip requires clip_id"));
	if ((action == QStringLiteral("list_clips") || action == QStringLiteral("get_clip")) && (parameters.contains(QStringLiteral("offset")) || parameters.contains(QStringLiteral("limit"))))
	{
		QJsonObject paging = parameters;
		paging.remove(action == QStringLiteral("list_clips") ? QStringLiteral("track_id") : QStringLiteral("clip_id"));
		if (!validPaging(paging)) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("paging is invalid"));
	}
	if (action == QStringLiteral("list_clips") && !hasOnlyKeys(parameters, {QStringLiteral("track_id"), QStringLiteral("offset"), QStringLiteral("limit")})) return requestFailure(QStringLiteral("unknown_parameter"), QStringLiteral("list_clips contains an unknown parameter"));
	if (action == QStringLiteral("get_clip") && !hasOnlyKeys(parameters, {QStringLiteral("clip_id"), QStringLiteral("offset"), QStringLiteral("limit")})) return requestFailure(QStringLiteral("unknown_parameter"), QStringLiteral("get_clip contains an unknown parameter"));
	if (action == QStringLiteral("open_clip_in_piano_roll") && (!hasOnlyKeys(parameters, {QStringLiteral("clip_id")}) || !parameters.value(QStringLiteral("clip_id")).isString())) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("open_clip_in_piano_roll requires clip_id"));
	if (action == QStringLiteral("create_instrument_track") && (!hasOnlyKeys(parameters, {QStringLiteral("name")}) || (parameters.contains(QStringLiteral("name")) && !parameters.value(QStringLiteral("name")).isString()))) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("create_instrument_track accepts an optional string name"));
	if (action == QStringLiteral("rename_track") && (!hasOnlyKeys(parameters, {QStringLiteral("track_id"), QStringLiteral("name")}) || !parameters.value(QStringLiteral("track_id")).isString() || !parameters.value(QStringLiteral("name")).isString())) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("rename_track requires track_id and name"));
	if (action == QStringLiteral("create_midi_clip") && (!hasOnlyKeys(parameters, {QStringLiteral("track_id"), QStringLiteral("start_ticks"), QStringLiteral("length_ticks"), QStringLiteral("auto_resize")}) || !parameters.value(QStringLiteral("track_id")).isString() || !parameters.contains(QStringLiteral("start_ticks")) || !isInteger(parameters.value(QStringLiteral("start_ticks"))) || (parameters.contains(QStringLiteral("length_ticks")) && !isInteger(parameters.value(QStringLiteral("length_ticks")))) || (parameters.contains(QStringLiteral("auto_resize")) && !parameters.value(QStringLiteral("auto_resize")).isBool()))) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("create_midi_clip parameters are invalid"));
	if (action == QStringLiteral("set_clip_properties")) {
		if (!hasOnlyKeys(parameters, {QStringLiteral("clip_id"), QStringLiteral("start_ticks"), QStringLiteral("length_ticks"), QStringLiteral("auto_resize"), QStringLiteral("expected_start_ticks"), QStringLiteral("expected_content_hash")}) || !parameters.value(QStringLiteral("clip_id")).isString()) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("set_clip_properties parameters are invalid"));
		if (!parameters.contains(QStringLiteral("start_ticks")) && !parameters.contains(QStringLiteral("length_ticks")) && !parameters.contains(QStringLiteral("auto_resize"))) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("set_clip_properties requires a property"));
		for (const auto &key : {QStringLiteral("start_ticks"), QStringLiteral("length_ticks"), QStringLiteral("expected_start_ticks")}) if (parameters.contains(key) && !isInteger(parameters.value(key))) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("clip tick values must be integers"));
		if (parameters.contains(QStringLiteral("auto_resize")) && !parameters.value(QStringLiteral("auto_resize")).isBool()) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("auto_resize must be boolean"));
		if (parameters.contains(QStringLiteral("expected_content_hash")) && !parameters.value(QStringLiteral("expected_content_hash")).isString()) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("expected_content_hash must be a string"));
	}
	if (action == QStringLiteral("set_track_instrument") && (!hasOnlyKeys(parameters, {QStringLiteral("track_id"), QStringLiteral("instrument_id")}) || !parameters.value(QStringLiteral("track_id")).isString() || !parameters.value(QStringLiteral("instrument_id")).isString())) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("set_track_instrument requires track_id and instrument_id"));
	if (action == QStringLiteral("set_track_properties")) {
		if (!hasOnlyKeys(parameters, {QStringLiteral("track_id"), QStringLiteral("volume"), QStringLiteral("panning")}) || !parameters.value(QStringLiteral("track_id")).isString() || (!parameters.contains(QStringLiteral("volume")) && !parameters.contains(QStringLiteral("panning")))) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("set_track_properties requires volume or panning"));
		for (const auto &key : {QStringLiteral("volume"), QStringLiteral("panning")}) if (parameters.contains(key) && !isInteger(parameters.value(key))) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("track properties must be integers"));
	}
	if (action == QStringLiteral("add_notes"))
	{
		if (!hasOnlyKeys(parameters, {QStringLiteral("clip_id"), QStringLiteral("notes"), QStringLiteral("expected_content_hash")}) || !parameters.value(QStringLiteral("clip_id")).isString() || !parameters.value(QStringLiteral("notes")).isArray())
			return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("add_notes requires clip_id and notes"));
		const auto notes = parameters.value(QStringLiteral("notes")).toArray();
		if (notes.isEmpty() || notes.size() > 256)
			return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("add_notes accepts 1 to 256 notes"));
		for (const auto &note : notes)
			if (!parseNote(note)) return requestFailure(QStringLiteral("invalid_note"), QStringLiteral("add_notes contains an invalid note"));
	}
	if (action == QStringLiteral("delete_notes") || action == QStringLiteral("transpose_notes") || action == QStringLiteral("move_notes"))
	{
		QSet<QString> allowed{QStringLiteral("clip_id"), QStringLiteral("selection"), QStringLiteral("expected_content_hash")};
		const auto valueKey = action == QStringLiteral("transpose_notes") ? QStringLiteral("semitones") : QStringLiteral("delta_ticks");
		if (action != QStringLiteral("delete_notes")) allowed.insert(valueKey);
		if (!hasOnlyKeys(parameters, allowed) || !parameters.value(QStringLiteral("clip_id")).isString() || !validSelection(parameters.value(QStringLiteral("selection"))))
			return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("mutation selection is invalid"));
		if (action != QStringLiteral("delete_notes") && (!parameters.contains(valueKey) || !isInteger(parameters.value(valueKey))))
			return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("mutation delta must be an integer"));
	}
	if (action == QStringLiteral("duplicate_region"))
	{
		if (!hasOnlyKeys(parameters, {QStringLiteral("clip_id"), QStringLiteral("source_start"), QStringLiteral("source_end"), QStringLiteral("destination_start"), QStringLiteral("expected_content_hash")}) || !parameters.value(QStringLiteral("clip_id")).isString())
			return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("duplicate_region requires clip_id and bounds"));
		for (const auto &key : {QStringLiteral("source_start"), QStringLiteral("source_end"), QStringLiteral("destination_start")})
			if (!parameters.contains(key) || !isInteger(parameters.value(key)))
				return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("duplicate_region bounds must be integers"));
	}
	if (action == QStringLiteral("get_mixer_structure") && !validPaging(parameters)) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("mixer structure paging is invalid"));
	if (action == QStringLiteral("create_mixer_channel") && (!hasOnlyKeys(parameters, {QStringLiteral("name")}) || (parameters.contains(QStringLiteral("name")) && (!parameters.value(QStringLiteral("name")).isString() || parameters.value(QStringLiteral("name")).toString().isEmpty())))) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("create_mixer_channel accepts an optional name"));
	if (action == QStringLiteral("delete_mixer_channel") && (!hasOnlyKeys(parameters, {QStringLiteral("mixer_channel_id")}) || !parameters.value(QStringLiteral("mixer_channel_id")).isString())) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("delete_mixer_channel requires mixer_channel_id"));
	if (action == QStringLiteral("set_mixer_channel_properties"))
	{
		if (!parameters.contains(QStringLiteral("mixer_channel_id")) || !parameters.value(QStringLiteral("mixer_channel_id")).isString() || !hasOnlyKeys(parameters, {QStringLiteral("mixer_channel_id"), QStringLiteral("name"), QStringLiteral("volume"), QStringLiteral("mute"), QStringLiteral("solo")}) || parameters.size() < 2) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("channel properties are invalid"));
		if (parameters.contains(QStringLiteral("name")) && !parameters.value(QStringLiteral("name")).isString()) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("name must be a string"));
		if (parameters.contains(QStringLiteral("volume")) && !isFiniteNumber(parameters.value(QStringLiteral("volume")))) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("volume must be numeric"));
		if (parameters.contains(QStringLiteral("mute")) && !parameters.value(QStringLiteral("mute")).isBool()) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("mute must be boolean"));
		if (parameters.contains(QStringLiteral("solo")) && !parameters.value(QStringLiteral("solo")).isBool()) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("solo must be boolean"));
	}
	if (action == QStringLiteral("route_track_to_mixer_channel") && (!hasOnlyKeys(parameters, {QStringLiteral("track_id"), QStringLiteral("mixer_channel_id")}) || !parameters.value(QStringLiteral("track_id")).isString() || !parameters.value(QStringLiteral("mixer_channel_id")).isString())) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("route_track_to_mixer_channel requires track_id and mixer_channel_id"));
	if (action == QStringLiteral("create_mixer_route") && (!hasOnlyKeys(parameters, {QStringLiteral("source_channel_id"), QStringLiteral("destination_channel_id"), QStringLiteral("amount")}) || !parameters.value(QStringLiteral("source_channel_id")).isString() || !parameters.value(QStringLiteral("destination_channel_id")).isString() || !isFiniteNumber(parameters.value(QStringLiteral("amount"))))) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("create_mixer_route parameters are invalid"));
	if (action == QStringLiteral("delete_mixer_route") && (!hasOnlyKeys(parameters, {QStringLiteral("mixer_route_id")}) || !parameters.value(QStringLiteral("mixer_route_id")).isString())) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("delete_mixer_route requires mixer_route_id"));
	if (action == QStringLiteral("set_mixer_route_properties") && (!hasOnlyKeys(parameters, {QStringLiteral("mixer_route_id"), QStringLiteral("amount")}) || !parameters.value(QStringLiteral("mixer_route_id")).isString() || !isFiniteNumber(parameters.value(QStringLiteral("amount"))))) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("set_mixer_route_properties parameters are invalid"));
	if (action == QStringLiteral("list_native_effects"))
	{
		if (!hasOnlyKeys(parameters, {QStringLiteral("offset"), QStringLiteral("limit"), QStringLiteral("filter")})) return requestFailure(QStringLiteral("unknown_parameter"), QStringLiteral("effect catalog contains an unknown parameter"));
		QJsonObject paging; for (const auto& key : {QStringLiteral("offset"), QStringLiteral("limit")}) if (parameters.contains(key)) paging.insert(key, parameters.value(key));
		if (!validPaging(paging) || (parameters.contains(QStringLiteral("filter")) && !parameters.value(QStringLiteral("filter")).isString())) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("effect catalog parameters are invalid"));
	}
	if ((action == QStringLiteral("remove_effect") || action == QStringLiteral("move_effect") || action == QStringLiteral("set_effect_state") || action == QStringLiteral("get_effect_parameters") || action == QStringLiteral("set_effect_parameters")) && (!parameters.contains(QStringLiteral("effect_instance_id")) || !parameters.value(QStringLiteral("effect_instance_id")).isString())) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("effect_instance_id is required"));
	if (action == QStringLiteral("add_effect") && (!hasOnlyKeys(parameters, {QStringLiteral("mixer_channel_id"), QStringLiteral("effect_id"), QStringLiteral("position")}) || !parameters.value(QStringLiteral("mixer_channel_id")).isString() || !parameters.value(QStringLiteral("effect_id")).isString() || !isInteger(parameters.value(QStringLiteral("position"))))) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("add_effect parameters are invalid"));
	if (action == QStringLiteral("remove_effect") && !hasOnlyKeys(parameters, {QStringLiteral("effect_instance_id")})) return requestFailure(QStringLiteral("unknown_parameter"), QStringLiteral("remove_effect contains an unknown parameter"));
	if (action == QStringLiteral("move_effect") && (!hasOnlyKeys(parameters, {QStringLiteral("effect_instance_id"), QStringLiteral("position")}) || !isInteger(parameters.value(QStringLiteral("position"))))) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("move_effect requires position"));
	if (action == QStringLiteral("set_effect_state") && (!hasOnlyKeys(parameters, {QStringLiteral("effect_instance_id"), QStringLiteral("enabled"), QStringLiteral("wet_dry")}) || !parameters.value(QStringLiteral("enabled")).isBool() || !isFiniteNumber(parameters.value(QStringLiteral("wet_dry"))))) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("set_effect_state parameters are invalid"));
	if (action == QStringLiteral("get_effect_parameters"))
	{
		QJsonObject paging; for (const auto& key : {QStringLiteral("offset"), QStringLiteral("limit")}) if (parameters.contains(key)) paging.insert(key, parameters.value(key));
		if (!hasOnlyKeys(parameters, {QStringLiteral("effect_instance_id"), QStringLiteral("offset"), QStringLiteral("limit")}) || !validPaging(paging)) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("effect parameter paging is invalid"));
	}
	if (action == QStringLiteral("set_effect_parameters"))
	{
		if (!hasOnlyKeys(parameters, {QStringLiteral("effect_instance_id"), QStringLiteral("changes")}) || !parameters.value(QStringLiteral("changes")).isArray() || parameters.value(QStringLiteral("changes")).toArray().isEmpty() || parameters.value(QStringLiteral("changes")).toArray().size() > 64) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("parameter changes are invalid"));
		for (const auto& value : parameters.value(QStringLiteral("changes")).toArray()) { const auto change = value.toObject(); if (!value.isObject() || !hasOnlyKeys(change, {QStringLiteral("id"), QStringLiteral("type"), QStringLiteral("value")}) || !change.value(QStringLiteral("id")).isString() || !change.value(QStringLiteral("type")).isString() || !isFiniteNumber(change.value(QStringLiteral("value")))) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("parameter change is invalid")); }
	}
	if (action == QStringLiteral("get_mixer_meters"))
	{
		QJsonObject paging; for (const auto& key : {QStringLiteral("offset"), QStringLiteral("limit")}) if (parameters.contains(key)) paging.insert(key, parameters.value(key));
		if (!hasOnlyKeys(parameters, {QStringLiteral("mixer_channel_ids"), QStringLiteral("offset"), QStringLiteral("limit")}) || !validPaging(paging)) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("meter paging is invalid"));
		if (parameters.contains(QStringLiteral("mixer_channel_ids")) && (!parameters.value(QStringLiteral("mixer_channel_ids")).isArray() || parameters.value(QStringLiteral("mixer_channel_ids")).toArray().size() > 64)) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("meter channel list is invalid"));
	}
	const QSet<QString> phase3g{QStringLiteral("get_arrangement"), QStringLiteral("get_timeline_region"), QStringLiteral("set_timeline_clip"), QStringLiteral("duplicate_timeline_clip"), QStringLiteral("delete_timeline_clip"), QStringLiteral("batch_edit_timeline_clips"), QStringLiteral("list_automation_targets"), QStringLiteral("get_automation_capabilities"), QStringLiteral("list_automation"), QStringLiteral("get_automation_clip"), QStringLiteral("create_automation_track"), QStringLiteral("delete_automation_track"), QStringLiteral("create_automation_clip"), QStringLiteral("delete_automation_clip"), QStringLiteral("set_automation_clip_properties"), QStringLiteral("set_automation_clip_target"), QStringLiteral("edit_automation_points"), QStringLiteral("duplicate_arrangement_region"), QStringLiteral("move_arrangement_region"), QStringLiteral("delete_arrangement_region"), QStringLiteral("insert_arrangement_space"), QStringLiteral("close_arrangement_gap"), QStringLiteral("replace_arrangement_region")};
	if (phase3g.contains(action))
	{
		if (action == QStringLiteral("get_arrangement") || action == QStringLiteral("list_automation_targets") || action == QStringLiteral("get_automation_capabilities") || action == QStringLiteral("list_automation"))
		{
			QJsonObject paging; for (const auto &key : {QStringLiteral("offset"), QStringLiteral("limit")}) if (parameters.contains(key)) paging.insert(key, parameters.value(key));
			if (!hasOnlyKeys(parameters, {QStringLiteral("start_tick"), QStringLiteral("end_tick"), QStringLiteral("track_ids"), QStringLiteral("include_automation"), QStringLiteral("offset"), QStringLiteral("limit")}) || !validPaging(paging)) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("Phase 3G paging or fields are invalid"));
		}
		else if (action == QStringLiteral("get_timeline_region"))
		{
			if (!hasOnlyKeys(parameters, {QStringLiteral("start_tick"), QStringLiteral("end_tick"), QStringLiteral("track_ids"), QStringLiteral("include_automation"), QStringLiteral("offset"), QStringLiteral("limit")}) || !parameters.contains(QStringLiteral("start_tick")) || !parameters.contains(QStringLiteral("end_tick")) || !isInteger(parameters.value(QStringLiteral("start_tick"))) || !isInteger(parameters.value(QStringLiteral("end_tick")))) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("timeline region requires integer bounds"));
		}
		else if (action == QStringLiteral("get_automation_clip"))
		{
			QJsonObject paging; for (const auto &key : {QStringLiteral("offset"), QStringLiteral("limit")}) if (parameters.contains(key)) paging.insert(key, parameters.value(key));
			if (!hasOnlyKeys(parameters, {QStringLiteral("automation_clip_id"), QStringLiteral("offset"), QStringLiteral("limit")}) || !parameters.value(QStringLiteral("automation_clip_id")).isString() || !validPaging(paging)) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("automation clip request is invalid"));
		}
		else if (action == QStringLiteral("set_timeline_clip") || action == QStringLiteral("duplicate_timeline_clip") || action == QStringLiteral("delete_timeline_clip"))
		{
			QSet<QString> allowed{QStringLiteral("clip_id")}; if (action == QStringLiteral("set_timeline_clip")) allowed.unite({QStringLiteral("start_tick"), QStringLiteral("length_ticks"), QStringLiteral("auto_resize")}); if (action == QStringLiteral("duplicate_timeline_clip")) allowed.insert(QStringLiteral("destination_tick"));
			if (!hasOnlyKeys(parameters, allowed) || !parameters.value(QStringLiteral("clip_id")).isString()) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("timeline clip request is invalid"));
		}
		else if (action == QStringLiteral("create_automation_track"))
		{
			if (!hasOnlyKeys(parameters, {QStringLiteral("name")}) || (parameters.contains(QStringLiteral("name")) && !parameters.value(QStringLiteral("name")).isString())) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("automation track name is invalid"));
		}
		else if (action == QStringLiteral("delete_automation_track") || action == QStringLiteral("delete_automation_clip"))
		{
			const auto key = action == QStringLiteral("delete_automation_track") ? QStringLiteral("automation_track_id") : QStringLiteral("automation_clip_id"); if (!hasOnlyKeys(parameters, {key}) || !parameters.value(key).isString()) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("automation identity is invalid"));
		}
		else if (action == QStringLiteral("create_automation_clip"))
		{
			if (!hasOnlyKeys(parameters, {QStringLiteral("automation_track_id"), QStringLiteral("start_tick"), QStringLiteral("length_ticks"), QStringLiteral("automation_target_id")}) || !parameters.value(QStringLiteral("automation_track_id")).isString() || !isInteger(parameters.value(QStringLiteral("start_tick"))) || !isInteger(parameters.value(QStringLiteral("length_ticks")))) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("automation clip creation is invalid"));
		}
		else if (action == QStringLiteral("set_automation_clip_target"))
		{
			if (!hasOnlyKeys(parameters, {QStringLiteral("automation_clip_id"), QStringLiteral("automation_target_id")}) || !parameters.value(QStringLiteral("automation_clip_id")).isString() || !parameters.value(QStringLiteral("automation_target_id")).isString()) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("automation target binding is invalid"));
		}
		else if (action == QStringLiteral("edit_automation_points"))
		{
			if (!hasOnlyKeys(parameters, {QStringLiteral("automation_clip_id"), QStringLiteral("mutations")}) || !parameters.value(QStringLiteral("automation_clip_id")).isString() || !parameters.value(QStringLiteral("mutations")).isArray() || parameters.value(QStringLiteral("mutations")).toArray().isEmpty() || parameters.value(QStringLiteral("mutations")).toArray().size() > 512) return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("automation point mutations are invalid"));
		}
	}

	return {true, QStringLiteral("ok"), QStringLiteral("Request accepted"), {2, requestId, action, parameters}};
}

QByteArray AiBridgeProtocol::serializeResponse(const AiBridgeProtocolResponse &response)
{
	QJsonObject object{
		{QStringLiteral("version"), response.version},
		{QStringLiteral("request_id"), response.requestId},
		{QStringLiteral("success"), response.success},
		{QStringLiteral("code"), response.code},
		{QStringLiteral("message"), response.message}};
	if (!response.data.isUndefined()) object.insert(QStringLiteral("data"), response.data);
	const auto frame = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
	if (frame.size() <= MaxFrameBytes) return frame;
	QJsonObject error{{QStringLiteral("version"), response.version}, {QStringLiteral("request_id"), response.requestId}, {QStringLiteral("success"), false}, {QStringLiteral("code"), QStringLiteral("result_too_large")}, {QStringLiteral("message"), QStringLiteral("response exceeds 64 KiB frame limit")}};
	return QJsonDocument(error).toJson(QJsonDocument::Compact) + '\n';
}

} // namespace lmms
