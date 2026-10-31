#include "AiBridgeProtocol.h"
#include "AiBridgeMutationTypes.h"
#include "AiBridgeMidiTypes.h"
#include "AiBridgeVocalRequest.h"
#include "AiBridgeMidiExportRequest.h"
#include "AiBridgeProjectMutationGuard.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QSet>

#include <cmath>
#include <limits>

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
	for (const auto& key : {QStringLiteral("offset"), QStringLiteral("limit")})
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

struct MidiValidationFailure
{
	QString code;
	QString message;

	bool valid() const { return code.isEmpty(); }
};

bool validEnum(const QJsonValue &value, const QSet<QString> &allowed)
{
	return value.isString() && allowed.contains(value.toString());
}

MidiValidationFailure validateMidiSelection(const QJsonValue &value, int *noteRefCount = nullptr)
{
	if (!value.isObject()) return {QStringLiteral("invalid_selection"), QStringLiteral("selection must be an object")};
	const auto selection = value.toObject();
	if (!selection.value(QStringLiteral("mode")).isString()) return {QStringLiteral("invalid_selection"), QStringLiteral("selection mode is required")};
	const auto mode = selection.value(QStringLiteral("mode")).toString();
	if (mode == QStringLiteral("note_refs"))
	{
		if (!hasOnlyKeys(selection, {QStringLiteral("mode"), QStringLiteral("note_refs")})
			|| !selection.value(QStringLiteral("note_refs")).isArray())
			return {QStringLiteral("invalid_selection"), QStringLiteral("note_refs selection contains predicate filters or invalid fields")};
		const auto refs = selection.value(QStringLiteral("note_refs")).toArray();
		if (refs.isEmpty() || refs.size() > AiBridgeMidiLimits::maxSelectedNotesPerMutation)
			return {QStringLiteral("note_limit_exceeded"), QStringLiteral("note_refs exceeds the selected-note limit")};
		for (const auto &ref : refs)
			if (!ref.isString() || ref.toString().isEmpty()) return {QStringLiteral("invalid_selection"), QStringLiteral("note_refs must contain non-empty strings")};
		if (noteRefCount) *noteRefCount += refs.size();
		return {};
	}
	if (mode != QStringLiteral("predicate")) return {QStringLiteral("invalid_selection"), QStringLiteral("selection mode is invalid")};
	if (!hasOnlyKeys(selection, {QStringLiteral("mode"), QStringLiteral("start_tick"), QStringLiteral("end_tick"),
		QStringLiteral("keys"), QStringLiteral("key_min"), QStringLiteral("key_max"),
		QStringLiteral("volume_min"), QStringLiteral("volume_max"), QStringLiteral("length_min"), QStringLiteral("length_max")}))
		return {QStringLiteral("invalid_selection"), QStringLiteral("predicate selection contains an unknown field")};
	const bool hasStart = selection.contains(QStringLiteral("start_tick"));
	const bool hasEnd = selection.contains(QStringLiteral("end_tick"));
	if (hasStart != hasEnd || (hasStart && (!isInteger(selection.value(QStringLiteral("start_tick")))
		|| !isInteger(selection.value(QStringLiteral("end_tick")))
		|| selection.value(QStringLiteral("start_tick")).toVariant().toLongLong() >= selection.value(QStringLiteral("end_tick")).toVariant().toLongLong())))
		return {QStringLiteral("invalid_selection"), QStringLiteral("predicate tick range must be a non-empty half-open interval")};
	for (const auto &range : {qMakePair(QStringLiteral("key_min"), QStringLiteral("key_max")),
		qMakePair(QStringLiteral("volume_min"), QStringLiteral("volume_max")),
		qMakePair(QStringLiteral("length_min"), QStringLiteral("length_max"))})
	{
		const bool hasMinimum = selection.contains(range.first);
		const bool hasMaximum = selection.contains(range.second);
		if (!hasMinimum && !hasMaximum) continue;
		for (const auto &key : {range.first, range.second})
			if (selection.contains(key) && !isInteger(selection.value(key))) return {QStringLiteral("invalid_selection"), QStringLiteral("predicate range values must be integers")};
		const qint64 minimum = hasMinimum ? selection.value(range.first).toVariant().toLongLong() : 0;
		const qint64 maximum = hasMaximum ? selection.value(range.second).toVariant().toLongLong() : (range.first == QStringLiteral("length_min") ? std::numeric_limits<qint64>::max() : (range.first == QStringLiteral("key_min") ? 127 : 200));
		const qint64 upperBound = range.first == QStringLiteral("key_min") ? 127 : range.first == QStringLiteral("volume_min") ? 200 : std::numeric_limits<qint32>::max();
		const qint64 lowerBound = range.first == QStringLiteral("length_min") ? 1 : 0;
		if (minimum < lowerBound || maximum > upperBound || minimum > maximum)
			return {QStringLiteral("invalid_selection"), QStringLiteral("predicate range is out of bounds")};
	}
	if (selection.contains(QStringLiteral("keys")))
	{
		const auto keys = selection.value(QStringLiteral("keys")).toArray();
		if (!selection.value(QStringLiteral("keys")).isArray() || keys.isEmpty() || keys.size() > 128)
			return {QStringLiteral("invalid_selection"), QStringLiteral("keys must be a non-empty bounded array")};
		for (const auto &key : keys)
			if (!isInteger(key) || key.toInt() < 0 || key.toInt() > 127) return {QStringLiteral("invalid_selection"), QStringLiteral("keys contains an out-of-range key")};
	}
	return {};
}

MidiValidationFailure validateMidiClips(const QJsonValue &value)
{
	if (!value.isArray()) return {QStringLiteral("invalid_parameter"), QStringLiteral("clips must be an array")};
	const auto clips = value.toArray();
	if (clips.isEmpty() || clips.size() > AiBridgeMidiLimits::maxClipsPerMutation)
		return {QStringLiteral("parameter_out_of_range"), QStringLiteral("clips exceeds the mutation limit")};
	int noteRefCount = 0;
	QSet<QString> clipIds;
	for (const auto &value : clips)
	{
		if (!value.isObject()) return {QStringLiteral("invalid_parameter"), QStringLiteral("clip selection must be an object")};
		const auto clip = value.toObject();
		if (!hasOnlyKeys(clip, {QStringLiteral("clip_id"), QStringLiteral("expected_midi_state_hash"), QStringLiteral("selection")})
			|| !clip.value(QStringLiteral("clip_id")).isString() || clip.value(QStringLiteral("clip_id")).toString().isEmpty()
			|| !clip.value(QStringLiteral("expected_midi_state_hash")).isString() || clip.value(QStringLiteral("expected_midi_state_hash")).toString().isEmpty())
			return {QStringLiteral("invalid_parameter"), QStringLiteral("clip selection requires clip_id and expected_midi_state_hash")};
		if (clipIds.contains(clip.value(QStringLiteral("clip_id")).toString())) return {QStringLiteral("invalid_parameter"), QStringLiteral("clip selections must be unique")};
		clipIds.insert(clip.value(QStringLiteral("clip_id")).toString());
		const auto selection = validateMidiSelection(clip.value(QStringLiteral("selection")), &noteRefCount);
		if (!selection.valid()) return selection;
		if (noteRefCount > AiBridgeMidiLimits::maxSelectedNotesPerMutation)
			return {QStringLiteral("note_limit_exceeded"), QStringLiteral("note_refs exceeds the mutation limit")};
	}
	return {};
}

MidiValidationFailure validateMidiRequest(const QString &action, const QJsonObject &parameters)
{
	const QSet<QString> midiActions{QStringLiteral("get_midi_capabilities"), QStringLiteral("get_midi_notes"), QStringLiteral("analyze_midi"), QStringLiteral("quantize_notes"), QStringLiteral("humanize_notes"), QStringLiteral("apply_groove"), QStringLiteral("shape_note_velocities"), QStringLiteral("shape_note_lengths"), QStringLiteral("cleanup_notes"), QStringLiteral("constrain_notes_to_scale")};
	if (!midiActions.contains(action)) return {};
	if (action == QStringLiteral("get_midi_capabilities"))
		return parameters.isEmpty() ? MidiValidationFailure{} : MidiValidationFailure{QStringLiteral("unknown_parameter"), QStringLiteral("get_midi_capabilities accepts no parameters")};
	if (action == QStringLiteral("get_midi_notes"))
	{
		if (!hasOnlyKeys(parameters, {QStringLiteral("clip_id"), QStringLiteral("offset"), QStringLiteral("limit")})
			|| !parameters.value(QStringLiteral("clip_id")).isString() || parameters.value(QStringLiteral("clip_id")).toString().isEmpty()) return {QStringLiteral("invalid_parameter"), QStringLiteral("get_midi_notes requires clip_id")};
		for (const auto &key : {QStringLiteral("offset"), QStringLiteral("limit")}) if (parameters.contains(key) && !isInteger(parameters.value(key))) return {QStringLiteral("invalid_parameter"), QStringLiteral("note page values must be integers")};
		if (parameters.value(QStringLiteral("offset")).toInt() < 0 || (parameters.contains(QStringLiteral("limit")) && (parameters.value(QStringLiteral("limit")).toInt() < 1 || parameters.value(QStringLiteral("limit")).toInt() > AiBridgeMidiLimits::maxNotesPerPage))) return {QStringLiteral("parameter_out_of_range"), QStringLiteral("note page is out of bounds")};
		return {};
	}
	if (action == QStringLiteral("analyze_midi"))
	{
		if (!hasOnlyKeys(parameters, {QStringLiteral("clip_ids"), QStringLiteral("grid_ticks"), QStringLiteral("short_note_threshold_ticks"), QStringLiteral("max_notes")}) || !parameters.value(QStringLiteral("clip_ids")).isArray()) return {QStringLiteral("invalid_parameter"), QStringLiteral("analyze_midi requires clip_ids")};
		const auto clips = parameters.value(QStringLiteral("clip_ids")).toArray();
		if (clips.isEmpty() || clips.size() > AiBridgeMidiLimits::maxClipsPerMutation) return {QStringLiteral("parameter_out_of_range"), QStringLiteral("analysis clip count is out of bounds")};
		for (const auto &clip : clips) if (!clip.isString() || clip.toString().isEmpty()) return {QStringLiteral("invalid_parameter"), QStringLiteral("analysis clip_ids must be strings")};
		for (const auto &key : {QStringLiteral("grid_ticks"), QStringLiteral("short_note_threshold_ticks"), QStringLiteral("max_notes")}) if (parameters.contains(key) && (!isInteger(parameters.value(key)) || parameters.value(key).toVariant().toLongLong() < 1)) return {QStringLiteral("parameter_out_of_range"), QStringLiteral("analysis bounds must be positive integers")};
		if (parameters.contains(QStringLiteral("max_notes")) && parameters.value(QStringLiteral("max_notes")).toInt() > AiBridgeMidiLimits::maxAnalysisNotes) return {QStringLiteral("parameter_out_of_range"), QStringLiteral("analysis note limit is out of bounds")};
		return {};
	}
	QSet<QString> allowed{QStringLiteral("clips"), QStringLiteral("dry_run"), QStringLiteral("boundary_policy")};
	if (action == QStringLiteral("quantize_notes")) allowed.unite({QStringLiteral("grid_ticks"), QStringLiteral("strength"), QStringLiteral("direction"), QStringLiteral("max_shift_ticks")});
	if (action == QStringLiteral("humanize_notes")) allowed.unite({QStringLiteral("seed"), QStringLiteral("timing_range_ticks"), QStringLiteral("volume_range"), QStringLiteral("length_range_ticks"), QStringLiteral("distribution")});
	if (action == QStringLiteral("apply_groove")) allowed.unite({QStringLiteral("mode"), QStringLiteral("strength"), QStringLiteral("subdivision_ticks"), QStringLiteral("swing_percent"), QStringLiteral("step_ticks"), QStringLiteral("timing_offsets"), QStringLiteral("volume_offsets")});
	if (action == QStringLiteral("shape_note_velocities")) allowed.unite({QStringLiteral("mode"), QStringLiteral("value"), QStringLiteral("start_value"), QStringLiteral("end_value")});
	if (action == QStringLiteral("shape_note_lengths")) allowed.unite({QStringLiteral("mode"), QStringLiteral("value"), QStringLiteral("gate_percent"), QStringLiteral("fallback_length_ticks")});
	if (action == QStringLiteral("cleanup_notes")) allowed.unite({QStringLiteral("remove_exact_duplicates"), QStringLiteral("same_pitch_overlap_mode"), QStringLiteral("minimum_length_ticks"), QStringLiteral("short_note_action")});
	if (action == QStringLiteral("constrain_notes_to_scale")) allowed.unite({QStringLiteral("allowed_pitch_classes"), QStringLiteral("direction"), QStringLiteral("nearest_tie_break")});
	if (!hasOnlyKeys(parameters, allowed) || (parameters.contains(QStringLiteral("dry_run")) && !parameters.value(QStringLiteral("dry_run")).isBool())) return {QStringLiteral("invalid_parameter"), QStringLiteral("MIDI mutation parameters are invalid")};
	if (const auto clips = validateMidiClips(parameters.value(QStringLiteral("clips"))); !clips.valid()) return clips;
	if (parameters.contains(QStringLiteral("boundary_policy")) && !validEnum(parameters.value(QStringLiteral("boundary_policy")), {QStringLiteral("clamp"), QStringLiteral("reject")})) return {QStringLiteral("invalid_parameter"), QStringLiteral("boundary policy is invalid")};
	if (action == QStringLiteral("quantize_notes"))
	{
		if (!isInteger(parameters.value(QStringLiteral("grid_ticks"))) || parameters.value(QStringLiteral("grid_ticks")).toVariant().toLongLong() < 1) return {QStringLiteral("invalid_grid"), QStringLiteral("grid_ticks must be positive")};
		if (parameters.contains(QStringLiteral("strength")) && (!isFiniteNumber(parameters.value(QStringLiteral("strength"))) || parameters.value(QStringLiteral("strength")).toDouble() < 0.0 || parameters.value(QStringLiteral("strength")).toDouble() > 1.0)) return {QStringLiteral("parameter_out_of_range"), QStringLiteral("strength must be between zero and one")};
		if (parameters.contains(QStringLiteral("direction")) && !validEnum(parameters.value(QStringLiteral("direction")), {QStringLiteral("nearest"), QStringLiteral("earlier"), QStringLiteral("later")})) return {QStringLiteral("invalid_parameter"), QStringLiteral("quantize direction is invalid")};
		if (parameters.contains(QStringLiteral("max_shift_ticks")) && (!isInteger(parameters.value(QStringLiteral("max_shift_ticks"))) || parameters.value(QStringLiteral("max_shift_ticks")).toVariant().toLongLong() < 0)) return {QStringLiteral("parameter_out_of_range"), QStringLiteral("max_shift_ticks is invalid")};
	}
	if (action == QStringLiteral("humanize_notes"))
	{
		if (!isInteger(parameters.value(QStringLiteral("seed"))) || parameters.value(QStringLiteral("seed")).toDouble() < 0 || parameters.value(QStringLiteral("seed")).toDouble() > 4294967295.0) return {QStringLiteral("parameter_out_of_range"), QStringLiteral("seed must be an unsigned 32-bit integer")};
		for (const auto &key : {QStringLiteral("timing_range_ticks"), QStringLiteral("volume_range"), QStringLiteral("length_range_ticks")}) if (parameters.contains(key) && (!isInteger(parameters.value(key)) || parameters.value(key).toVariant().toLongLong() < 0)) return {QStringLiteral("parameter_out_of_range"), QStringLiteral("humanize ranges must be non-negative integers")};
		if (parameters.contains(QStringLiteral("volume_range")) && parameters.value(QStringLiteral("volume_range")).toInt() > 200) return {QStringLiteral("parameter_out_of_range"), QStringLiteral("volume_range is out of bounds")};
		if (parameters.contains(QStringLiteral("distribution")) && !validEnum(parameters.value(QStringLiteral("distribution")), {QStringLiteral("triangular"), QStringLiteral("uniform")})) return {QStringLiteral("invalid_parameter"), QStringLiteral("humanize distribution is invalid")};
	}
	if (action == QStringLiteral("apply_groove"))
	{
		if (!validEnum(parameters.value(QStringLiteral("mode")), {QStringLiteral("swing"), QStringLiteral("template")})) return {QStringLiteral("invalid_parameter"), QStringLiteral("groove mode is invalid")};
		if (parameters.contains(QStringLiteral("strength")) && (!isFiniteNumber(parameters.value(QStringLiteral("strength"))) || parameters.value(QStringLiteral("strength")).toDouble() < 0.0 || parameters.value(QStringLiteral("strength")).toDouble() > 1.0)) return {QStringLiteral("parameter_out_of_range"), QStringLiteral("groove strength is out of bounds")};
		const bool swing = parameters.value(QStringLiteral("mode")).toString() == QStringLiteral("swing");
		if (swing && (!hasOnlyKeys(parameters, {QStringLiteral("clips"), QStringLiteral("dry_run"), QStringLiteral("boundary_policy"), QStringLiteral("mode"), QStringLiteral("strength"), QStringLiteral("subdivision_ticks"), QStringLiteral("swing_percent")}) || !isInteger(parameters.value(QStringLiteral("subdivision_ticks"))) || parameters.value(QStringLiteral("subdivision_ticks")).toVariant().toLongLong() < 1 || !isFiniteNumber(parameters.value(QStringLiteral("swing_percent"))) || parameters.value(QStringLiteral("swing_percent")).toDouble() < 50.0 || parameters.value(QStringLiteral("swing_percent")).toDouble() > 100.0)) return {QStringLiteral("invalid_parameter"), QStringLiteral("swing parameters are invalid")};
		if (!swing && (!hasOnlyKeys(parameters, {QStringLiteral("clips"), QStringLiteral("dry_run"), QStringLiteral("boundary_policy"), QStringLiteral("mode"), QStringLiteral("strength"), QStringLiteral("step_ticks"), QStringLiteral("timing_offsets"), QStringLiteral("volume_offsets")}) || !isInteger(parameters.value(QStringLiteral("step_ticks"))) || parameters.value(QStringLiteral("step_ticks")).toVariant().toLongLong() < 1 || !parameters.value(QStringLiteral("timing_offsets")).isArray() || parameters.value(QStringLiteral("timing_offsets")).toArray().isEmpty())) return {QStringLiteral("invalid_parameter"), QStringLiteral("groove template parameters are invalid")};
	}
	if (action == QStringLiteral("shape_note_velocities"))
	{
		if (!validEnum(parameters.value(QStringLiteral("mode")), {QStringLiteral("set"), QStringLiteral("offset"), QStringLiteral("scale"), QStringLiteral("ramp")})) return {QStringLiteral("invalid_parameter"), QStringLiteral("velocity mode is invalid")};
		const bool ramp = parameters.value(QStringLiteral("mode")).toString() == QStringLiteral("ramp");
		if (ramp ? (!hasOnlyKeys(parameters, {QStringLiteral("clips"), QStringLiteral("dry_run"), QStringLiteral("boundary_policy"), QStringLiteral("mode"), QStringLiteral("start_value"), QStringLiteral("end_value")}) || !isFiniteNumber(parameters.value(QStringLiteral("start_value"))) || !isFiniteNumber(parameters.value(QStringLiteral("end_value")))) : (!hasOnlyKeys(parameters, {QStringLiteral("clips"), QStringLiteral("dry_run"), QStringLiteral("boundary_policy"), QStringLiteral("mode"), QStringLiteral("value")}) || !isFiniteNumber(parameters.value(QStringLiteral("value"))))) return {QStringLiteral("invalid_parameter"), QStringLiteral("velocity mode parameters are invalid")};
	}
	if (action == QStringLiteral("shape_note_lengths"))
	{
		if (!validEnum(parameters.value(QStringLiteral("mode")), {QStringLiteral("set"), QStringLiteral("offset"), QStringLiteral("scale"), QStringLiteral("gate_to_next")})) return {QStringLiteral("invalid_parameter"), QStringLiteral("length mode is invalid")};
		const bool gate = parameters.value(QStringLiteral("mode")).toString() == QStringLiteral("gate_to_next");
		if (gate ? (!hasOnlyKeys(parameters, {QStringLiteral("clips"), QStringLiteral("dry_run"), QStringLiteral("boundary_policy"), QStringLiteral("mode"), QStringLiteral("gate_percent"), QStringLiteral("fallback_length_ticks")}) || !isFiniteNumber(parameters.value(QStringLiteral("gate_percent")))) : (!hasOnlyKeys(parameters, {QStringLiteral("clips"), QStringLiteral("dry_run"), QStringLiteral("boundary_policy"), QStringLiteral("mode"), QStringLiteral("value")}) || !isFiniteNumber(parameters.value(QStringLiteral("value"))))) return {QStringLiteral("invalid_parameter"), QStringLiteral("length mode parameters are invalid")};
		if (parameters.contains(QStringLiteral("fallback_length_ticks")) && (!isInteger(parameters.value(QStringLiteral("fallback_length_ticks"))) || parameters.value(QStringLiteral("fallback_length_ticks")).toVariant().toLongLong() < 1)) return {QStringLiteral("parameter_out_of_range"), QStringLiteral("fallback length is invalid")};
	}
	if (action == QStringLiteral("cleanup_notes"))
	{
		if (parameters.contains(QStringLiteral("remove_exact_duplicates")) && !parameters.value(QStringLiteral("remove_exact_duplicates")).isBool()) return {QStringLiteral("invalid_parameter"), QStringLiteral("duplicate cleanup flag is invalid")};
		if (parameters.contains(QStringLiteral("same_pitch_overlap_mode")) && !validEnum(parameters.value(QStringLiteral("same_pitch_overlap_mode")), {QStringLiteral("none"), QStringLiteral("trim_earlier"), QStringLiteral("reject")})) return {QStringLiteral("invalid_parameter"), QStringLiteral("overlap mode is invalid")};
		if (parameters.contains(QStringLiteral("minimum_length_ticks")) && (!isInteger(parameters.value(QStringLiteral("minimum_length_ticks"))) || parameters.value(QStringLiteral("minimum_length_ticks")).toVariant().toLongLong() < 1)) return {QStringLiteral("parameter_out_of_range"), QStringLiteral("minimum length is invalid")};
		if (parameters.contains(QStringLiteral("short_note_action")) != parameters.contains(QStringLiteral("minimum_length_ticks")) || (parameters.contains(QStringLiteral("short_note_action")) && !validEnum(parameters.value(QStringLiteral("short_note_action")), {QStringLiteral("remove"), QStringLiteral("extend")}))) return {QStringLiteral("invalid_parameter"), QStringLiteral("short-note cleanup requires both threshold and action")};
	}
	if (action == QStringLiteral("constrain_notes_to_scale"))
	{
		if (!parameters.value(QStringLiteral("allowed_pitch_classes")).isArray() || parameters.value(QStringLiteral("allowed_pitch_classes")).toArray().isEmpty()) return {QStringLiteral("invalid_pitch_class"), QStringLiteral("allowed_pitch_classes is required")};
		for (const auto &pitchClass : parameters.value(QStringLiteral("allowed_pitch_classes")).toArray()) if (!isInteger(pitchClass) || pitchClass.toInt() < 0 || pitchClass.toInt() > 11) return {QStringLiteral("invalid_pitch_class"), QStringLiteral("pitch class is out of bounds")};
		if (parameters.contains(QStringLiteral("direction")) && !validEnum(parameters.value(QStringLiteral("direction")), {QStringLiteral("nearest"), QStringLiteral("up"), QStringLiteral("down")})) return {QStringLiteral("invalid_parameter"), QStringLiteral("scale direction is invalid")};
		if (parameters.contains(QStringLiteral("nearest_tie_break")) && !validEnum(parameters.value(QStringLiteral("nearest_tie_break")), {QStringLiteral("down"), QStringLiteral("up")})) return {QStringLiteral("invalid_parameter"), QStringLiteral("scale tie break is invalid")};
	}
	return {};
}

const QSet<QString> audioActions{"get_audio_capabilities","get_audio_clip","create_audio_track","import_audio_file","move_audio_clip","duplicate_audio_clip","delete_audio_clip","batch_edit_audio_clips","trim_audio_clip","split_audio_clip"};
bool audioString(const QJsonValue& v,int maximum)
{
	return v.isString() && !v.toString().trimmed().isEmpty() && v.toString().size()<=maximum && !v.toString().contains(QChar(0));
}
bool audioTick(const QJsonValue& v) {return isInteger(v) && v.toDouble()>=0 && v.toDouble()<=std::numeric_limits<qint32>::max();}
bool audioEdit(const QString& operation,const QJsonObject& p,bool inBatch)
{
	QSet<QString> keys{"clip_id","expected_audio_state_hash"};
	if(inBatch)keys.insert("operation");else keys.insert("dry_run");
	if(operation=="move" || operation=="duplicate")keys.insert("position_ticks");
	if(operation=="duplicate")keys.insert("destination_track_id");
	if(operation!="move" && operation!="duplicate" && operation!="delete")return false;
	if(!hasOnlyKeys(p,keys) || !audioString(p.value("clip_id"),256))return false;
	const auto hash=p.value("expected_audio_state_hash").toString();
	if(!p.value("expected_audio_state_hash").isString() || hash.size()!=64)return false;
	for(const auto c:hash)if(!QStringLiteral("0123456789abcdef").contains(c))return false;
	if(operation!="delete" && !audioTick(p.value("position_ticks")))return false;
	return operation!="duplicate" || audioString(p.value("destination_track_id"),256);
}
bool validAudioRequest(const QString& action,const QJsonObject& p)
{
	if(p.contains("dry_run") && !p.value("dry_run").isBool())return false;
	if(action=="get_audio_capabilities")return p.isEmpty();
	if(action=="get_audio_clip")return hasOnlyKeys(p,{"clip_id"}) && audioString(p.value("clip_id"),256);
	if(action=="trim_audio_clip" || action=="split_audio_clip")
	{
		QSet<QString> keys{"clip_id","expected_audio_state_hash","dry_run"};
		const auto tick=action=="trim_audio_clip"?QStringLiteral("new_edge_tick"):QStringLiteral("split_tick");keys.insert(tick);
		if(action=="trim_audio_clip")keys.insert("edge");
		if(!hasOnlyKeys(p,keys) || !audioString(p.value("clip_id"),256) || !audioTick(p.value(tick)))return false;
		const auto hash=p.value("expected_audio_state_hash").toString();if(!p.value("expected_audio_state_hash").isString() || hash.size()!=64)return false;
		for(const auto c:hash)if(!QStringLiteral("0123456789abcdef").contains(c))return false;
		return action!="trim_audio_clip" || (p.value("edge").isString() && (p.value("edge")=="left" || p.value("edge")=="right"));
	}
	if(action=="create_audio_track")
	{
		if(!hasOnlyKeys(p,{"name","volume","panning","muted","dry_run"}) || !audioString(p.value("name"),128))return false;
		for(const auto key:{"volume","panning"})if(p.contains(key) && !isFiniteNumber(p.value(key)))return false;
		return !p.contains("muted") || p.value("muted").isBool();
	}
	if(action=="import_audio_file")return hasOnlyKeys(p,{"track_id","path","position_ticks","dry_run"}) && audioString(p.value("track_id"),256) && audioString(p.value("path"),1024) && audioTick(p.value("position_ticks"));
	if(action=="batch_edit_audio_clips")
	{
		if(!hasOnlyKeys(p,{"operations","dry_run"}) || !p.value("operations").isArray())return false;
		const auto ops=p.value("operations").toArray();if(ops.isEmpty() || ops.size()>256)return false;
		QSet<QString> clips;int copies=0;
		for(const auto& value:ops){if(!value.isObject())return false;const auto op=value.toObject();const auto kind=op.value("operation").toString();if(!audioEdit(kind,op,true))return false;clips.insert(op.value("clip_id").toString());if(kind=="duplicate")++copies;}
		return clips.size()+copies<=32;
	}
	return audioEdit(action=="move_audio_clip"?"move":action=="duplicate_audio_clip"?"duplicate":"delete",p,false);
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
	if (action != QStringLiteral("guarded_mutate_project") && !MidiExportRequest::handles(action) && !QSet<QString>{QStringLiteral("get_context"), QStringLiteral("set_tempo"), QStringLiteral("adjust_current_track_volume"), QStringLiteral("add_note_to_current_clip"), QStringLiteral("get_windows"), QStringLiteral("set_window_geometry"), QStringLiteral("focus_window"), QStringLiteral("arrange_windows"), QStringLiteral("get_project_structure"), QStringLiteral("list_tracks"), QStringLiteral("list_clips"), QStringLiteral("get_clip"), QStringLiteral("open_clip_in_piano_roll"), QStringLiteral("add_notes"), QStringLiteral("delete_notes"), QStringLiteral("transpose_notes"), QStringLiteral("move_notes"), QStringLiteral("duplicate_region"), QStringLiteral("create_instrument_track"), QStringLiteral("rename_track"), QStringLiteral("create_midi_clip"), QStringLiteral("set_clip_properties"), QStringLiteral("list_instruments"), QStringLiteral("set_track_instrument"), QStringLiteral("set_track_properties"), QStringLiteral("list_bundled_samples"), QStringLiteral("load_bundled_sample"), QStringLiteral("list_bundled_presets"), QStringLiteral("load_bundled_preset"), QStringLiteral("get_mixer_structure"), QStringLiteral("create_mixer_channel"), QStringLiteral("delete_mixer_channel"), QStringLiteral("set_mixer_channel_properties"), QStringLiteral("route_track_to_mixer_channel"), QStringLiteral("create_mixer_route"), QStringLiteral("delete_mixer_route"), QStringLiteral("set_mixer_route_properties"), QStringLiteral("list_native_effects"), QStringLiteral("add_effect"), QStringLiteral("remove_effect"), QStringLiteral("move_effect"), QStringLiteral("set_effect_state"), QStringLiteral("get_effect_parameters"), QStringLiteral("set_effect_parameters"), QStringLiteral("get_mixer_meters"), QStringLiteral("get_arrangement"), QStringLiteral("get_timeline_region"), QStringLiteral("set_timeline_clip"), QStringLiteral("duplicate_timeline_clip"), QStringLiteral("delete_timeline_clip"), QStringLiteral("batch_edit_timeline_clips"), QStringLiteral("list_automation_targets"), QStringLiteral("get_automation_capabilities"), QStringLiteral("list_automation"), QStringLiteral("get_automation_clip"), QStringLiteral("create_automation_track"), QStringLiteral("delete_automation_track"), QStringLiteral("create_automation_clip"), QStringLiteral("delete_automation_clip"), QStringLiteral("set_automation_clip_properties"), QStringLiteral("set_automation_clip_target"), QStringLiteral("edit_automation_points"), QStringLiteral("duplicate_arrangement_region"), QStringLiteral("move_arrangement_region"), QStringLiteral("delete_arrangement_region"), QStringLiteral("insert_arrangement_space"), QStringLiteral("close_arrangement_gap"), QStringLiteral("replace_arrangement_region"), QStringLiteral("get_midi_capabilities"), QStringLiteral("get_midi_notes"), QStringLiteral("analyze_midi"), QStringLiteral("quantize_notes"), QStringLiteral("humanize_notes"), QStringLiteral("apply_groove"), QStringLiteral("shape_note_velocities"), QStringLiteral("shape_note_lengths"), QStringLiteral("cleanup_notes"), QStringLiteral("constrain_notes_to_scale")}.contains(action))
		if (!audioActions.contains(action) && !VocalRequest::handles(action) && !MidiExportRequest::handles(action)) return failure(QStringLiteral("unknown_action"), QStringLiteral("Action is not allowlisted"));
	if (!object.contains(QStringLiteral("parameters")) || !object.value(QStringLiteral("parameters")).isObject())
		return failure(QStringLiteral("invalid_parameters"), QStringLiteral("parameters must be an object"));

	const auto parameters = object.value(QStringLiteral("parameters")).toObject();
	if(VocalRequest::handles(action) && !VocalRequest::valid(action,parameters))
		return {false,"invalid_parameter","Vocal request fields or bounds are invalid",{2,requestId,action,parameters}};
	const auto requestFailure = [&](const QString &code, const QString &message)
	{
		return AiBridgeProtocolParseResult{false, code, message, {2, requestId, action, parameters}};
	};
	if (action == QStringLiteral("guarded_mutate_project"))
	{
		if (!hasOnlyKeys(parameters, {QStringLiteral("expected_project_identity"), QStringLiteral("operation"),
			QStringLiteral("parameters"), QStringLiteral("expected_target")})
			|| !parameters.value(QStringLiteral("expected_project_identity")).isObject()
			|| !parameters.value(QStringLiteral("operation")).isString()
			|| !parameters.value(QStringLiteral("parameters")).isObject())
			return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("guarded mutation envelope is invalid"));

		const auto expectedIdentity = parameters.value(QStringLiteral("expected_project_identity")).toObject();
		const auto identityCheck = AiBridgeProjectMutationGuard::validateIdentity(expectedIdentity);
		if (!identityCheck.success)
			return requestFailure(identityCheck.code, QStringLiteral("guarded mutation requires complete available project identity"));

		const auto operation = parameters.value(QStringLiteral("operation")).toString();
		const QSet<QString> guardedOperations{QStringLiteral("set_tempo"), QStringLiteral("create_instrument_track"),
			QStringLiteral("set_track_instrument"), QStringLiteral("create_midi_clip"),
			QStringLiteral("set_clip_properties"), QStringLiteral("add_notes"),
			QStringLiteral("delete_notes"), QStringLiteral("transpose_notes"),
			QStringLiteral("move_notes"), QStringLiteral("duplicate_region")};
		if (!guardedOperations.contains(operation))
			return requestFailure(QStringLiteral("unsupported_operation"), QStringLiteral("guarded operation is not supported"));

		const auto operationParameters = parameters.value(QStringLiteral("parameters")).toObject();
		const bool createsTrack = operation == QStringLiteral("create_instrument_track");
		const bool tempoOperation = operation == QStringLiteral("set_tempo");
		if (createsTrack)
		{
			if (parameters.contains(QStringLiteral("expected_target")))
				return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("track creation does not accept a target"));
		}
		else if (!parameters.value(QStringLiteral("expected_target")).isObject())
			return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("guarded operation requires a target precondition"));
		else
		{
			const auto target = parameters.value(QStringLiteral("expected_target")).toObject();
			if (tempoOperation)
			{
				if (!hasOnlyKeys(target, {QStringLiteral("kind"), QStringLiteral("expected_bpm")})
					|| target.size() != 2 || target.value(QStringLiteral("kind")).toString() != QStringLiteral("tempo")
					|| !isInteger(target.value(QStringLiteral("expected_bpm")))
					|| target.value(QStringLiteral("expected_bpm")).toInt() < 1
					|| target.value(QStringLiteral("expected_bpm")).toInt() > 999)
					return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("tempo target precondition is invalid"));
			}
			else
			{
				const bool trackTarget = operation == QStringLiteral("set_track_instrument")
					|| operation == QStringLiteral("create_midi_clip");
				const auto kind = trackTarget ? QStringLiteral("track") : QStringLiteral("clip");
				const auto idKey = trackTarget ? QStringLiteral("track_id") : QStringLiteral("clip_id");
				if (!hasOnlyKeys(target, {QStringLiteral("kind"), QStringLiteral("object_id"), QStringLiteral("expected_state_hash")})
					|| target.size() != 3 || target.value(QStringLiteral("kind")).toString() != kind
					|| !target.value(QStringLiteral("object_id")).isString()
					|| target.value(QStringLiteral("object_id")).toString().isEmpty()
					|| target.value(QStringLiteral("object_id")).toString() != operationParameters.value(idKey).toString()
					|| !AiBridgeProjectMutationGuard::isSha256(target.value(QStringLiteral("expected_state_hash")).toString()))
					return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("target precondition is invalid"));
			}
		}

		if (operation == QStringLiteral("set_clip_properties")
			&& (!AiBridgeProjectMutationGuard::isSha256(operationParameters.value(QStringLiteral("expected_content_hash")).toString())
				|| !isInteger(operationParameters.value(QStringLiteral("expected_start_ticks")))))
			return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("clip property mutation requires current content and start evidence"));
		if (operation == QStringLiteral("delete_notes") || operation == QStringLiteral("transpose_notes")
			|| operation == QStringLiteral("move_notes"))
		{
			const auto selectionValue = operationParameters.value(QStringLiteral("selection"));
			if (!selectionValue.isObject() || !hasOnlyKeys(selectionValue.toObject(),
				{QStringLiteral("position_start"), QStringLiteral("position_end"), QStringLiteral("key_min"),
				QStringLiteral("key_max"), QStringLiteral("exact")}))
				return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("guarded note selection contains an unknown field"));
			const auto selectionObject = selectionValue.toObject();
			if (selectionObject.contains(QStringLiteral("exact"))
				&& (!selectionObject.value(QStringLiteral("exact")).isObject()
					|| !hasOnlyKeys(selectionObject.value(QStringLiteral("exact")).toObject(),
						{QStringLiteral("position"), QStringLiteral("length"), QStringLiteral("key"),
						QStringLiteral("volume"), QStringLiteral("panning"), QStringLiteral("note_type")})))
				return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("guarded exact-note selector contains an unknown field"));
		}
		if ((operation == QStringLiteral("add_notes") || operation == QStringLiteral("delete_notes")
			|| operation == QStringLiteral("transpose_notes") || operation == QStringLiteral("move_notes")
			|| operation == QStringLiteral("duplicate_region"))
			&& !AiBridgeProjectMutationGuard::isSha256(operationParameters.value(QStringLiteral("expected_content_hash")).toString()))
			return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("MIDI mutation requires current content evidence"));

		const auto innerFrame = QJsonDocument(QJsonObject{{QStringLiteral("version"), 2},
			{QStringLiteral("request_id"), QStringLiteral("guarded-inner")},
			{QStringLiteral("action"), operation}, {QStringLiteral("parameters"), operationParameters}})
			.toJson(QJsonDocument::Compact) + '\n';
		if (!parseRequest(innerFrame).success)
			return requestFailure(QStringLiteral("invalid_parameter"), QStringLiteral("guarded operation parameters are invalid"));
		return {true, QStringLiteral("ok"), QStringLiteral("guarded mutation request is valid"),
			{2, requestId, action, parameters}};
	}
	if(MidiExportRequest::handles(action) && !MidiExportRequest::valid(action,parameters))
		return requestFailure(QStringLiteral("invalid_parameter"),QStringLiteral("MIDI export request fields or bounds are invalid"));
	if (const auto midiValidation = validateMidiRequest(action, parameters); !midiValidation.valid())
		return requestFailure(midiValidation.code, midiValidation.message);
	if(audioActions.contains(action) && !validAudioRequest(action,parameters))
		return requestFailure("invalid_parameter","Audio request fields or bounds are invalid");
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
