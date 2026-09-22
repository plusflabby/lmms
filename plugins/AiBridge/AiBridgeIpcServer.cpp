#include "AiBridgeIpcServer.h"

#include <QLocalSocket>

#include <QJsonObject>
#include <QJsonArray>
#include <QMetaObject>
#include <optional>

#include "AiBridgeActions.h"
#include "AiBridgeProjectActions.h"
#include "AiBridgeWindowActions.h"
#include "AiBridgeWindowContext.h"
#include "AiBridgeAssetTypes.h"
#include "AiBridgeMixerActions.h"
#include "AiBridgeMixerContext.h"
#include "AiBridgeMixerRegistry.h"
#include "AiBridgeEffectActions.h"
#include "AiBridgeEffectCatalog.h"
#include "AiBridgeEffectTypes.h"
#include "AiBridgeMeterActions.h"
#include "AiBridgeArrangementContext.h"
#include "AiBridgeArrangementActions.h"
#include "AiBridgeTimelineActions.h"
#include "AiBridgeAutomationContext.h"
#include "AiBridgeAutomationReconciler.h"
#include "AiBridgeAutomationActions.h"
#include "AiBridgeMidiContext.h"
#include "AiBridgeMidiAnalysis.h"
#include "AiBridgeMidiActions.h"
#include "AiBridgeMidiPlanner.h"
#include "MidiClip.h"
#include "Note.h"

namespace lmms
{

namespace
{
QJsonObject trackJson(const AiBridgeTrackValue& v)
{
	QJsonObject o{{"id",v.id},{"name",v.name},{"type",v.type},{"index",v.index},{"clip_count",v.clipCount}};
	if(v.hasVolume)o.insert("volume",v.volume);
	return o;
}
QJsonObject clipJson(const AiBridgeClipValue& v)
{
	return {{"id",v.id},{"track_id",v.trackId},{"name",v.name},{"type",v.type},{"start_ticks",v.startTicks},{"length_ticks",v.lengthTicks},{"note_count",v.noteCount},{"current_in_piano_roll",v.currentInPianoRoll}};
}
QJsonObject pageJson(const AiBridgePage& p)
{
	return {{"total",p.total},{"offset",p.offset},{"limit",p.limit},{"has_more",p.hasMore}};
}
QJsonObject constructionTrackJson(const AiBridgeTrackConstructionValue &v)
{
	return {{"track_id",v.trackId},{"name",v.name},{"type",v.type},{"index",v.index},{"volume",v.volume},{"panning",v.panning}};
}
QJsonObject constructionClipJson(const AiBridgeClipConstructionValue &v)
{
	return {{"clip_id",v.clipId},{"track_id",v.trackId},{"name",v.name},{"type",v.type},{"start_ticks",v.startTicks},{"length_ticks",v.lengthTicks},{"auto_resize",v.autoResize},{"content_hash",v.contentHash}};
}
QJsonObject trackPropertiesJson(const AiBridgeTrackPropertyValue &v)
{
	return {{"track_id",v.trackId},{"volume",v.volume},{"panning",v.panning}};
}
QJsonObject clipPropertiesJson(const AiBridgeClipPropertyValue &v)
{
	return {{"clip_id",v.clipId},{"track_id",v.trackId},{"start_ticks",v.startTicks},{"length_ticks",v.lengthTicks},{"auto_resize",v.autoResize},{"content_hash",v.contentHash}};
}
QJsonObject instrumentJson(const AiBridgeInstrumentValue &v)
{
	return {{"instrument_id",v.instrumentId},{"display_name",v.displayName},{"description",v.description},{"available",v.available}};
}

QJsonObject samplePageJson(const AiBridgeAssetPage<AiBridgeSampleValue> &page)
{
	QJsonArray items;
	for (const auto &item : page.items)
		items.append(QJsonObject{{"asset_id", item.assetId}, {"display_name", item.displayName}, {"instrument_id", item.instrumentId}, {"category", item.category}, {"format", item.format}});
	return QJsonObject{{"items", items}, {"total", page.total}, {"offset", page.offset}, {"limit", page.limit}, {"has_more", page.hasMore}};
}

QJsonObject presetPageJson(const AiBridgeAssetPage<AiBridgePresetValue> &page)
{
	QJsonArray items;
	for (const auto &item : page.items)
		items.append(QJsonObject{{"asset_id", item.assetId}, {"display_name", item.displayName}, {"instrument_id", item.instrumentId}, {"category", item.category}, {"format", item.format}});
	return QJsonObject{{"items", items}, {"total", page.total}, {"offset", page.offset}, {"limit", page.limit}, {"has_more", page.hasMore}};
}

AiBridgeCopiedNote copiedNote(const QJsonValue &value)
{
	const auto object = value.toObject();
	AiBridgeCopiedNote note;
	note.position_ticks = object.value(QStringLiteral("position_ticks")).toVariant().toLongLong();
	note.length_ticks = object.value(QStringLiteral("length_ticks")).toVariant().toLongLong();
	note.key = object.value(QStringLiteral("key")).toInt();
	note.volume = object.value(QStringLiteral("volume")).toInt();
	note.panning = object.value(QStringLiteral("panning")).toInt();
	note.note_type = object.value(QStringLiteral("note_type")).toString();
	return note;
}

AiBridgeNoteSelection selection(const QJsonObject &object)
{
	AiBridgeNoteSelection value;
	if (object.contains(QStringLiteral("position_start"))) value.position_start = object.value(QStringLiteral("position_start")).toVariant().toLongLong();
	if (object.contains(QStringLiteral("position_end"))) value.position_end = object.value(QStringLiteral("position_end")).toVariant().toLongLong();
	if (object.contains(QStringLiteral("key_min"))) value.key_min = object.value(QStringLiteral("key_min")).toInt();
	if (object.contains(QStringLiteral("key_max"))) value.key_max = object.value(QStringLiteral("key_max")).toInt();
	if (object.contains(QStringLiteral("exact")))
	{
		const auto exactObject = object.value(QStringLiteral("exact")).toObject();
		AiBridgeExactNote exact;
		if (exactObject.contains(QStringLiteral("position"))) exact.position = exactObject.value(QStringLiteral("position")).toVariant().toLongLong();
		if (exactObject.contains(QStringLiteral("length"))) exact.length = exactObject.value(QStringLiteral("length")).toVariant().toLongLong();
		if (exactObject.contains(QStringLiteral("key"))) exact.key = exactObject.value(QStringLiteral("key")).toInt();
		if (exactObject.contains(QStringLiteral("volume"))) exact.volume = exactObject.value(QStringLiteral("volume")).toInt();
		if (exactObject.contains(QStringLiteral("panning"))) exact.panning = exactObject.value(QStringLiteral("panning")).toInt();
		if (exactObject.contains(QStringLiteral("note_type"))) exact.note_type = exactObject.value(QStringLiteral("note_type")).toString();
		value.exact = exact;
	}
	return value;
}

QJsonObject mutationJson(const AiBridgeMutationResult &value)
{
	return {{"clip_id", value.clip_id}, {"track_id", value.track_id},
		{"notes_added", value.notes_added}, {"notes_deleted", value.notes_deleted},
		{"notes_changed", value.notes_changed},
		{"old_content_hash", value.old_content_hash},
		{"new_content_hash", value.new_content_hash},
		{"clip_length_ticks", value.clip_length_ticks},
		{"current_in_piano_roll", value.current_in_piano_roll}};
}

QJsonObject mixerPageJson(const AiBridgeMixerPage &page)
{
	return {{"total", page.total}, {"offset", page.offset}, {"limit", page.limit}, {"has_more", page.hasMore}};
}

QJsonObject effectSummaryJson(const AiBridgeMixerEffectSummary &value)
{
	return {{"effect_instance_id", value.effectInstanceId}, {"effect_id", value.effectId}, {"name", value.name}, {"position", value.position}};
}

QJsonObject channelJson(const AiBridgeMixerChannelSnapshot &value)
{
	QJsonArray tracks; for (const auto &id : value.trackIds) tracks.append(id);
	QJsonArray effects; for (const auto &effect : value.effects) effects.append(effectSummaryJson(effect));
	return {{"mixer_channel_id", value.mixerChannelId}, {"name", value.name}, {"master", value.master}, {"volume", value.volume}, {"mute", value.mute}, {"solo", value.solo}, {"track_ids", tracks}, {"effects", effects}};
}

QJsonObject routeJson(const AiBridgeMixerRouteSnapshot &value)
{
	return {{"mixer_route_id", value.mixerRouteId}, {"source_channel_id", value.sourceChannelId}, {"destination_channel_id", value.destinationChannelId}, {"amount", value.amount}};
}

QJsonObject mixerStructureJson(const AiBridgeMixerStructureSnapshot &value)
{
	QJsonArray channels; for (const auto &item : value.channelItems) channels.append(channelJson(item));
	QJsonArray routes; for (const auto &item : value.routeItems) routes.append(routeJson(item));
	return {{"channels", channels}, {"channel_page", mixerPageJson(value.channels)}, {"routes", routes}, {"route_page", mixerPageJson(value.routes)}};
}

QString parameterTypeName(AiBridgeEffectParameterType type)
{
	switch (type) { case AiBridgeEffectParameterType::Float: return "float"; case AiBridgeEffectParameterType::Integer: return "integer"; case AiBridgeEffectParameterType::Boolean: return "boolean"; case AiBridgeEffectParameterType::Enum: return "enum"; }
	return "float";
}

QJsonObject parameterJson(const AiBridgeEffectParameterMetadata &value)
{
	QJsonArray enums; for (const auto &item : value.enumValues) enums.append(item);

	return {{"id", value.id}, {"name", value.name}, {"type", parameterTypeName(value.type)}, {"value", value.value}, {"minimum", value.minimum}, {"maximum", value.maximum}, {"default", value.defaultValue}, {"step", value.step}, {"unit", value.unit}, {"enum_values", enums}, {"writable", value.writable}};
}

QJsonObject effectActionJson(const AiBridgeEffectActionResult &value)
{
	QJsonArray effects; for (const auto &item : value.effects) effects.append(effectSummaryJson(item));
	return {{"effect_instance_id", value.effectInstanceId}, {"effect_id", value.effectId}, {"mixer_channel_id", value.mixerChannelId}, {"position", value.position}, {"enabled", value.enabled}, {"wet_dry", value.wetDry}, {"effects", effects}};
}

QJsonObject parameterPageJson(const AiBridgeEffectParameterActionResult &value)
{
	QJsonArray items; for (const auto &item : value.page.items) items.append(parameterJson(item));
	QJsonObject result = mixerPageJson(value.page.page); result.insert("effect_instance_id", value.effectInstanceId); result.insert("parameters", items); return result;
}

QJsonObject meterPageJson(const AiBridgeMeterActionResult &value)
{
	QJsonArray items; for (const auto &item : value.page.items) items.append(QJsonObject{{"mixer_channel_id", item.mixerChannelId}, {"left_peak", item.leftPeak}, {"right_peak", item.rightPeak}});
	QJsonObject result = mixerPageJson(value.page.page); result.insert("meters", items); return result;
}

QJsonObject midiCapabilitiesJson()
{
	const auto capabilities = AiBridgeMidiContext::getCapabilities();
	QJsonArray distributions;
	for (const auto &distribution : capabilities.humanizeDistributions) distributions.append(distribution);
	return {{"key_min", capabilities.keyMinimum}, {"key_max", capabilities.keyMaximum},
		{"volume_min", capabilities.volumeMinimum}, {"volume_max", capabilities.volumeMaximum},
		{"panning_min", capabilities.panningMinimum}, {"panning_max", capabilities.panningMaximum},
		{"minimum_note_length_ticks", capabilities.minimumLengthTicks},
		{"max_clips_per_mutation", capabilities.maxClipsPerMutation},
		{"max_selected_notes_per_mutation", capabilities.maxSelectedNotesPerMutation},
		{"max_notes_per_page", capabilities.pageLimit},
		{"max_preview_entries", capabilities.maxPreviewEntries},
		{"max_analysis_notes", capabilities.maxAnalysisNotes},
		{"humanize_distributions", distributions}};
}

bool midiSnapshot(const QString &clipId, AiBridgeMidiClipSnapshot &snapshot, QString &code)
{
	AiBridgeProjectContext context;
	auto *clip = dynamic_cast<MidiClip *>(context.resolveClip(clipId));
	if (!clip) { code = QStringLiteral("stale_clip"); return false; }
	snapshot.clipId = clipId; snapshot.projectGeneration = context.projectGeneration();
	for (const auto *note : clip->notes()) snapshot.notes.append({note->pos().getTicks(), note->length().getTicks(), note->key(), note->getVolume(), note->getPanning()});
	snapshot.notes = canonicalizeMidiNotes(snapshot.notes);
	return true;
}

QJsonObject midiNoteJson(const AiBridgeMidiNoteSnapshot &note, const QString &noteRef)
{
	return {{"position_ticks", note.positionTicks}, {"length_ticks", note.lengthTicks}, {"key", note.key}, {"volume", note.volume}, {"panning", note.panning}, {"note_ref", noteRef}};
}

QJsonObject midiAnalysisJson(const AiBridgeMidiAnalysisResult &analysis)
{
	QJsonArray histogram; for (const int value : analysis.pitchClassHistogram) histogram.append(value);
	QJsonObject data{{"note_count", analysis.noteCount}, {"distinct_onset_count", analysis.distinctOnsetCount}, {"pitch_minimum", analysis.pitchMinimum}, {"pitch_maximum", analysis.pitchMaximum}, {"pitch_class_histogram", histogram}, {"volume_minimum", analysis.volumeMinimum}, {"volume_maximum", analysis.volumeMaximum}, {"volume_mean", analysis.volumeMean}, {"length_minimum_ticks", analysis.lengthMinimumTicks}, {"length_maximum_ticks", analysis.lengthMaximumTicks}, {"length_mean_ticks", analysis.lengthMeanTicks}, {"exact_duplicate_count", analysis.exactDuplicateCount}, {"same_pitch_overlap_count", analysis.samePitchOverlapCount}, {"short_note_count", analysis.shortNoteCount}};
	if (analysis.gridDeviation) data.insert("grid_deviation", QJsonObject{{"exact_on_grid_count", analysis.gridDeviation->exactOnGridCount}, {"mean_absolute_deviation_ticks", analysis.gridDeviation->meanAbsoluteDeviationTicks}, {"maximum_absolute_deviation_ticks", analysis.gridDeviation->maximumAbsoluteDeviationTicks}});
	return data;
}

AiBridgeMidiSelection midiSelectionFromJson(const QJsonObject &object)
{
	AiBridgeMidiSelection selection;
	selection.mode = object.value(QStringLiteral("mode")).toString() == QStringLiteral("predicate")
		? AiBridgeMidiSelectionMode::Predicate : AiBridgeMidiSelectionMode::NoteRefs;
	for (const auto &ref : object.value(QStringLiteral("note_refs")).toArray()) selection.noteRefs.append(ref.toString());
	for (const auto &key : object.value(QStringLiteral("keys")).toArray()) selection.keys.append(key.toInt());
	if (object.contains(QStringLiteral("start_tick"))) selection.startTick = object.value(QStringLiteral("start_tick")).toVariant().toLongLong();
	if (object.contains(QStringLiteral("end_tick"))) selection.endTick = object.value(QStringLiteral("end_tick")).toVariant().toLongLong();
	if (object.contains(QStringLiteral("key_min"))) selection.keyMin = object.value(QStringLiteral("key_min")).toInt();
	if (object.contains(QStringLiteral("key_max"))) selection.keyMax = object.value(QStringLiteral("key_max")).toInt();
	if (object.contains(QStringLiteral("volume_min"))) selection.volumeMin = object.value(QStringLiteral("volume_min")).toInt();
	if (object.contains(QStringLiteral("volume_max"))) selection.volumeMax = object.value(QStringLiteral("volume_max")).toInt();
	if (object.contains(QStringLiteral("length_min"))) selection.lengthMin = object.value(QStringLiteral("length_min")).toVariant().toLongLong();
	if (object.contains(QStringLiteral("length_max"))) selection.lengthMax = object.value(QStringLiteral("length_max")).toVariant().toLongLong();
	return selection;
}

AiBridgeMidiBoundaryPolicy midiBoundaryPolicy(const QJsonObject &parameters)
{
	return parameters.value(QStringLiteral("boundary_policy")).toString() == QStringLiteral("reject")
		? AiBridgeMidiBoundaryPolicy::Reject : AiBridgeMidiBoundaryPolicy::Clamp;
}

AiBridgeMidiValueMode midiValueMode(const QString &mode)
{
	if (mode == QStringLiteral("offset")) return AiBridgeMidiValueMode::Offset;
	if (mode == QStringLiteral("scale")) return AiBridgeMidiValueMode::Scale;
	if (mode == QStringLiteral("ramp")) return AiBridgeMidiValueMode::Ramp;
	if (mode == QStringLiteral("gate_to_next")) return AiBridgeMidiValueMode::GateToNext;
	return AiBridgeMidiValueMode::Set;
}

struct AiBridgeMidiPlannedMutation
{
	bool success = false;
	QString code;
	QVector<AiBridgeMidiNoteSnapshot> notes;
	int changedCount = 0;
	int clampCount = 0;
	int removedCount = 0;
};

AiBridgeMidiPlannedMutation planMidiMutation(const QString &action,
	const AiBridgeMidiClipSnapshot &snapshot, const QVector<int> &indices, const QJsonObject &parameters)
{
	if (action == QStringLiteral("quantize_notes"))
	{
		AiBridgeMidiQuantizeOptions options(parameters.value(QStringLiteral("grid_ticks")).toVariant().toLongLong());
		options.strength = parameters.value(QStringLiteral("strength")).toDouble(1.0);
		const auto direction = parameters.value(QStringLiteral("direction")).toString();
		options.direction = direction == QStringLiteral("earlier") ? AiBridgeMidiQuantizeDirection::Earlier : direction == QStringLiteral("later") ? AiBridgeMidiQuantizeDirection::Later : AiBridgeMidiQuantizeDirection::Nearest;
		if (parameters.contains(QStringLiteral("max_shift_ticks"))) options.maxShiftTicks = parameters.value(QStringLiteral("max_shift_ticks")).toVariant().toLongLong();
		options.boundaryPolicy = midiBoundaryPolicy(parameters);
		const auto plan = planQuantize(snapshot, indices, options);
		return {plan.success, plan.code, plan.notes, plan.changedCount, 0, 0};
	}
	if (action == QStringLiteral("humanize_notes"))
	{
		AiBridgeMidiHumanizeOptions options;
		options.seed = parameters.value(QStringLiteral("seed")).toVariant().toUInt();
		options.timingRangeTicks = parameters.value(QStringLiteral("timing_range_ticks")).toVariant().toLongLong();
		options.volumeRange = parameters.value(QStringLiteral("volume_range")).toInt();
		options.lengthRangeTicks = parameters.value(QStringLiteral("length_range_ticks")).toVariant().toLongLong();
		options.distribution = parameters.value(QStringLiteral("distribution")).toString() == QStringLiteral("uniform") ? AiBridgeMidiHumanizeDistribution::Uniform : AiBridgeMidiHumanizeDistribution::Triangular;
		options.boundaryPolicy = midiBoundaryPolicy(parameters);
		const auto plan = planHumanize(snapshot, indices, options);
		return {plan.success, plan.code, plan.notes, plan.changedCount, plan.clampCount, 0};
	}
	if (action == QStringLiteral("apply_groove"))
	{
		AiBridgeMidiGrooveOptions options;
		options.mode = parameters.value(QStringLiteral("mode")).toString() == QStringLiteral("template") ? AiBridgeMidiGrooveMode::Template : AiBridgeMidiGrooveMode::Swing;
		options.strength = parameters.value(QStringLiteral("strength")).toDouble(1.0);
		options.subdivisionTicks = parameters.value(QStringLiteral("subdivision_ticks")).toVariant().toLongLong();
		options.swingPercent = parameters.value(QStringLiteral("swing_percent")).toDouble(50.0);
		options.stepTicks = parameters.value(QStringLiteral("step_ticks")).toVariant().toLongLong();
		for (const auto &value : parameters.value(QStringLiteral("timing_offsets")).toArray()) options.timingOffsets.append(value.toVariant().toLongLong());
		for (const auto &value : parameters.value(QStringLiteral("volume_offsets")).toArray()) options.volumeOffsets.append(value.toInt());
		options.boundaryPolicy = midiBoundaryPolicy(parameters);
		const auto plan = planGroove(snapshot, indices, options);
		return {plan.success, plan.code, plan.notes, plan.changedCount, plan.clampCount, 0};
	}
	if (action == QStringLiteral("shape_note_velocities"))
	{
		AiBridgeMidiVelocityOptions options;
		options.mode = midiValueMode(parameters.value(QStringLiteral("mode")).toString());
		options.value = parameters.value(QStringLiteral("value")).toDouble();
		options.startValue = parameters.value(QStringLiteral("start_value")).toInt();
		options.endValue = parameters.value(QStringLiteral("end_value")).toInt();
		options.boundaryPolicy = midiBoundaryPolicy(parameters);
		const auto plan = planVelocityShape(snapshot, indices, options);
		return {plan.success, plan.code, plan.notes, plan.changedCount, plan.clampCount, 0};
	}
	if (action == QStringLiteral("shape_note_lengths"))
	{
		AiBridgeMidiLengthOptions options;
		options.mode = midiValueMode(parameters.value(QStringLiteral("mode")).toString());
		options.value = parameters.value(QStringLiteral("value")).toDouble();
		options.gatePercent = parameters.value(QStringLiteral("gate_percent")).toDouble(100.0);
		if (parameters.contains(QStringLiteral("fallback_length_ticks"))) options.fallbackLengthTicks = parameters.value(QStringLiteral("fallback_length_ticks")).toVariant().toLongLong();
		options.boundaryPolicy = midiBoundaryPolicy(parameters);
		const auto plan = planLengthShape(snapshot, indices, options);
		return {plan.success, plan.code, plan.notes, plan.changedCount, plan.clampCount, 0};
	}
	if (action == QStringLiteral("cleanup_notes"))
	{
		AiBridgeMidiCleanupOptions options;
		options.removeExactDuplicates = parameters.value(QStringLiteral("remove_exact_duplicates")).toBool(false);
		const auto overlap = parameters.value(QStringLiteral("same_pitch_overlap_mode")).toString();
		options.samePitchOverlapMode = overlap == QStringLiteral("trim_earlier") ? AiBridgeMidiOverlapMode::TrimEarlier : overlap == QStringLiteral("reject") ? AiBridgeMidiOverlapMode::Reject : AiBridgeMidiOverlapMode::None;
		if (parameters.contains(QStringLiteral("minimum_length_ticks"))) options.minimumLengthTicks = parameters.value(QStringLiteral("minimum_length_ticks")).toVariant().toLongLong();
		if (parameters.contains(QStringLiteral("short_note_action"))) options.shortNoteAction = parameters.value(QStringLiteral("short_note_action")).toString() == QStringLiteral("extend") ? AiBridgeMidiShortNoteAction::Extend : AiBridgeMidiShortNoteAction::Remove;
		const auto plan = planCleanup(snapshot, indices, options);
		return {plan.success, plan.code, plan.notes, plan.changedCount, 0, plan.removedCount};
	}
	AiBridgeMidiScaleConstraintOptions options;
	for (const auto &value : parameters.value(QStringLiteral("allowed_pitch_classes")).toArray()) options.allowedPitchClasses.append(value.toInt());
	const auto direction = parameters.value(QStringLiteral("direction")).toString();
	options.direction = direction == QStringLiteral("up") ? AiBridgeMidiScaleDirection::Up : direction == QStringLiteral("down") ? AiBridgeMidiScaleDirection::Down : AiBridgeMidiScaleDirection::Nearest;
	options.nearestTieBreak = parameters.value(QStringLiteral("nearest_tie_break")).toString() == QStringLiteral("up") ? AiBridgeMidiNearestTieBreak::Up : AiBridgeMidiNearestTieBreak::Down;
	const auto plan = planScaleConstraint(snapshot, indices, options);
	return {plan.success, plan.code, plan.notes, plan.changedCount, plan.clampCount, 0};
}

QJsonObject midiMutationClipJson(const AiBridgeMidiClipSnapshot &before,
	const AiBridgeMidiPlannedMutation &plan, int selectedCount)
{
	AiBridgeMidiClipSnapshot after = before;
	after.notes = plan.notes;
	QJsonArray preview;
	const int rows = qMin(plan.changedCount, AiBridgeMidiLimits::maxPreviewEntries);
	for (int index = 0; index < rows && index < plan.notes.size(); ++index)
		preview.append(QJsonObject{{"after", midiNoteJson(plan.notes[index], QString())}});
	return {{"clip_id", before.clipId}, {"before_midi_state_hash", computeMidiStateHash(before)},
		{"after_midi_state_hash", computeMidiStateHash(after)}, {"selected_count", selectedCount},
		{"changed_count", plan.changedCount}, {"unchanged_count", qMax(0, selectedCount - plan.changedCount)},
		{"removed_count", plan.removedCount}, {"created_count", 0}, {"clamp_count", plan.clampCount},
		{"preview", preview}, {"preview_has_more", plan.changedCount > rows}};
}

AiBridgeProtocolResponse executeMidiMutation(const AiBridgeProtocolRequest &request)
{
	QVector<AiBridgeMidiNativePlan> nativePlans;
	QJsonArray clips;
	bool hasChanges = false;
	for (const auto &value : request.parameters.value(QStringLiteral("clips")).toArray())
	{
		const auto clipRequest = value.toObject();
		AiBridgeMidiClipSnapshot snapshot;
		QString code;
		if (!midiSnapshot(clipRequest.value(QStringLiteral("clip_id")).toString(), snapshot, code))
			return {2, request.requestId, false, code, QStringLiteral("MIDI clip is not live")};
		const auto resolved = AiBridgeMidiContext::resolveSelection(snapshot,
			clipRequest.value(QStringLiteral("expected_midi_state_hash")).toString(), midiSelectionFromJson(clipRequest.value(QStringLiteral("selection")).toObject()));
		if (!resolved.success) return {2, request.requestId, false, resolved.code, QStringLiteral("MIDI selection is no longer valid")};
		const auto plan = planMidiMutation(request.action, snapshot, resolved.canonicalIndices, request.parameters);
		if (!plan.success) return {2, request.requestId, false, plan.code, QStringLiteral("MIDI plan could not be created")};
		hasChanges = hasChanges || plan.changedCount > 0;
		clips.append(midiMutationClipJson(snapshot, plan, resolved.canonicalIndices.size()));
		nativePlans.append({snapshot.clipId, snapshot.projectGeneration, computeMidiStateHash(snapshot), plan.notes});
	}
	QJsonObject data{{"dry_run", request.parameters.value(QStringLiteral("dry_run")).toBool(false)}, {"clips", clips}};
	if (request.parameters.value(QStringLiteral("dry_run")).toBool(false))
		return {2, request.requestId, true, hasChanges ? QStringLiteral("ok") : QStringLiteral("no_effect"), QStringLiteral("MIDI plan generated"), data};
	if (!hasChanges) return {2, request.requestId, true, QStringLiteral("no_effect"), QStringLiteral("MIDI mutation has no effect"), data};
	const auto execution = AiBridgeMidiActions::execute(nativePlans);
	QJsonArray applied;
	QJsonArray unapplied;
	for (const auto &plan : execution.appliedPlans) applied.append(plan.clipId);
	for (const auto &plan : execution.unappliedPlans) unapplied.append(plan.clipId);
	data.insert(QStringLiteral("preflight_succeeded"), execution.preflightSucceeded);
	data.insert(QStringLiteral("applied_clip_ids"), applied);
	data.insert(QStringLiteral("unapplied_clip_ids"), unapplied);
	data.insert(QStringLiteral("failed_plan_index"), execution.failedPlanIndex);
	if (execution.failedPlanIndex >= 0) data.insert(QStringLiteral("failed_clip_id"), execution.failedPlan.clipId);
	return {2, request.requestId, execution.success, execution.success ? QStringLiteral("ok") : execution.code,
		execution.success ? QStringLiteral("MIDI mutation applied") : QStringLiteral("MIDI mutation execution failed"), data};
}

bool isMixerAction(const QString &action)
{
	return action == "get_mixer_structure" || action == "create_mixer_channel" || action == "delete_mixer_channel" || action == "set_mixer_channel_properties" || action == "route_track_to_mixer_channel" || action == "create_mixer_route" || action == "delete_mixer_route" || action == "set_mixer_route_properties";
}

bool isEffectAction(const QString &action)
{
	return action == "list_native_effects" || action == "add_effect" || action == "remove_effect" || action == "move_effect" || action == "set_effect_state" || action == "get_effect_parameters" || action == "set_effect_parameters" || action == "get_mixer_meters";
}

AiBridgeEffectParameterType parameterType(const QString &value)
{
	if (value == "integer") return AiBridgeEffectParameterType::Integer;
	if (value == "boolean") return AiBridgeEffectParameterType::Boolean;
	if (value == "enum") return AiBridgeEffectParameterType::Enum;
	return AiBridgeEffectParameterType::Float;
}

QJsonObject timelineClipJson(const AiBridgeTimelineClipSnapshot &value)
{
	return {{"clip_id", value.clipId}, {"track_id", value.trackId}, {"type", value.type},
		{"name", value.name}, {"start_tick", value.startTick}, {"end_tick", value.endTick}};
}

QJsonObject automationPointJson(const AiBridgeAutomationPointSnapshot &value)
{
	return {{"point_id", value.pointId}, {"tick", value.tick}, {"value", value.value}, {"curve", value.curve}};
}

QJsonObject automationClipJson(const AiBridgeAutomationClipSnapshot &value)
{
	QJsonArray points;
	for (const auto &point : value.points) points.append(automationPointJson(point));
	return {{"automation_clip_id", value.automationClipId}, {"automation_track_id", value.automationTrackId},
		{"automation_target_id", value.automationTargetId}, {"start_tick", value.startTick},
		{"length_ticks", value.lengthTicks}, {"curve", value.curve}, {"points", points}};
}

QJsonObject automationTargetJson(const AiBridgeAutomationTargetSnapshot &value)
{
	return {{"automation_target_id", value.automationTargetId}, {"semantic_class", value.semanticClass},
		{"owner_id", value.ownerId}, {"parameter_id", value.parameterId}, {"value_type", value.valueType},
		{"minimum", value.minimum}, {"maximum", value.maximum}, {"default", value.defaultValue},
		{"step", value.step}, {"unit", value.unit}, {"capability", value.capability}};
}

QJsonObject automationActionJson(const AiBridgeAutomationActionResult &value)
{
	QJsonArray applied; for (const auto &id : value.appliedPointIds) applied.append(id);
	QJsonArray unapplied; for (const auto &id : value.unappliedPointIds) unapplied.append(id);
	return {{"automation_track_id", value.automationTrackId}, {"automation_clip_id", value.automationClipId},
		{"automation_target_id", value.automationTargetId}, {"applied_point_ids", applied}, {"unapplied_point_ids", unapplied}};
}

QJsonObject arrangementPlanJson(const AiBridgeArrangementPlan &plan)
{
	QJsonArray operations; for (const auto &item : plan.orderedOperations) operations.append(item);
	QJsonArray steps;
	for (const auto &step : plan.steps)
	{
		const auto kind = step.kind == AiBridgeArrangementStepKind::DeleteClip ? QStringLiteral("delete_clip")
			: step.kind == AiBridgeArrangementStepKind::DuplicateClip ? QStringLiteral("duplicate_clip")
			: step.kind == AiBridgeArrangementStepKind::MoveClip ? QStringLiteral("move_clip") : QStringLiteral("shift_clip");
		steps.append(QJsonObject{{"kind", kind}, {"clip_id", step.clipId}, {"track_id", step.trackId},
			{"source_tick", step.sourceTick}, {"destination_tick", step.destinationTick}});
	}
	QJsonArray automationSteps;
	for (const auto &step : plan.automationSteps)
	{
		const auto kind = step.kind == AiBridgeArrangementAutomationStepKind::DeleteClip ? QStringLiteral("delete_automation_clip")
			: step.kind == AiBridgeArrangementAutomationStepKind::DuplicateClip ? QStringLiteral("duplicate_automation_clip")
			: step.kind == AiBridgeArrangementAutomationStepKind::MoveClip ? QStringLiteral("move_automation_clip") : QStringLiteral("shift_automation_clip");
		QJsonArray points;
		for (const auto &point : step.points)
			points.append(QJsonObject{{"point_id", point.pointId}, {"tick", point.tick}, {"value", point.value}, {"curve", point.curve}});
		automationSteps.append(QJsonObject{{"kind", kind}, {"automation_track_id", step.automationTrackId},
			{"automation_clip_id", step.automationClipId}, {"automation_target_id", step.automationTargetId},
			{"source_start_tick", step.sourceStartTick}, {"source_end_tick", step.sourceEndTick},
			{"destination_start_tick", step.destinationStartTick}, {"destination_end_tick", step.destinationEndTick}, {"points", points}});
	}
	QJsonArray affected; for (const auto &item : plan.affectedClips) affected.append(timelineClipJson(item));
	QJsonArray deletes; for (const auto &item : plan.deleteClipIds) deletes.append(item);
	QJsonArray overwrites; for (const auto &item : plan.overwriteDeleteIds) overwrites.append(item);
	QJsonArray shifted; for (const auto &item : plan.shiftedClipIds) shifted.append(item);
	QJsonArray conflicts;
	for (const auto &item : plan.conflicts) conflicts.append(QJsonObject{{"track_id", item.trackId}, {"clip_id", item.clipId}, {"automation_clip_id", item.automationClipId}, {"start_tick", item.range.startTick}, {"end_tick", item.range.endTick}, {"overwrite_would_resolve", item.overwriteWouldResolve}});
	return {{"operations", operations}, {"steps", steps}, {"automation_steps", automationSteps}, {"affected_clips", affected}, {"delete_clip_ids", deletes}, {"overwrite_delete_ids", overwrites}, {"shifted_clip_ids", shifted}, {"conflicts", conflicts}, {"shift_tick", plan.shiftTick}, {"include_automation", plan.includeAutomation}, {"overwrite", plan.overwrite}, {"dry_run", plan.dryRun}, {"no_effect", plan.noEffect}, {"no_effect_reason", plan.noEffectReason}, {"resulting_start_tick", plan.resultingBounds.startTick}, {"resulting_end_tick", plan.resultingBounds.endTick}};
}

AiBridgeArrangementSnapshot arrangementSnapshot()
{
	AiBridgeArrangementSnapshot result;
	result.clips = AiBridgeArrangementContext{}.snapshot(0, AiBridgeTimelineLimits::maxTimelineClipsPerPage).items;
	AiBridgeProjectContext project;
	result.projectGeneration = project.projectGeneration();
	AiBridgeAutomationContext automation(aiBridgeAutomationRegistry(), aiBridgeAutomationTargets());
	const auto page = automation.listClips(0, AiBridgeAutomationLimits::maxAutomationClipsPerPage);
	for (const auto &clip : page.items)
	{
		AiBridgeArrangementAutomationSnapshot copied;
		copied.automationTrackId = clip.automationTrackId;
		copied.automationClipId = clip.automationClipId;
		copied.automationTargetId = clip.automationTargetId;
		AiBridgeAutomationTargetSnapshot target;
		if (!clip.automationTargetId.isEmpty() && aiBridgeAutomationTargets().resolve(clip.automationTargetId, target))
			copied.ownerId = target.ownerId;
		copied.startTick = clip.startTick;
		copied.endTick = clip.startTick + clip.lengthTicks;
		for (const auto &point : clip.points)
			copied.points.append({point.pointId, point.tick, point.value, point.curve});
		result.automationClips.append(copied);
	}
	return result;
}

QString arrangementStepKind(AiBridgeArrangementStepKind kind)
{
	return kind == AiBridgeArrangementStepKind::DeleteClip ? QStringLiteral("delete_clip")
		: kind == AiBridgeArrangementStepKind::DuplicateClip ? QStringLiteral("duplicate_clip")
		: kind == AiBridgeArrangementStepKind::MoveClip ? QStringLiteral("move_clip") : QStringLiteral("shift_clip");
}

QJsonObject arrangementStepJson(const AiBridgeArrangementStep &step)
{
	return { {"kind", arrangementStepKind(step.kind)}, {"clip_id", step.clipId},
		{"track_id", step.trackId}, {"source_tick", step.sourceTick}, {"destination_tick", step.destinationTick} };
}

QJsonObject automationStepJson(const AiBridgeArrangementAutomationStep &step)
{
	const auto kind = step.kind == AiBridgeArrangementAutomationStepKind::DeleteClip ? QStringLiteral("delete_automation_clip")
		: step.kind == AiBridgeArrangementAutomationStepKind::DuplicateClip ? QStringLiteral("duplicate_automation_clip")
		: step.kind == AiBridgeArrangementAutomationStepKind::MoveClip ? QStringLiteral("move_automation_clip") : QStringLiteral("shift_automation_clip");
	QJsonArray points;
	for (const auto &point : step.points)
		points.append(QJsonObject{{"point_id", point.pointId}, {"tick", point.tick}, {"value", point.value}, {"curve", point.curve}});
	return {{"kind", kind}, {"automation_track_id", step.automationTrackId}, {"automation_clip_id", step.automationClipId},
		{"automation_target_id", step.automationTargetId}, {"source_start_tick", step.sourceStartTick},
		{"source_end_tick", step.sourceEndTick}, {"destination_start_tick", step.destinationStartTick},
		{"destination_end_tick", step.destinationEndTick}, {"points", points}};
}

QJsonObject arrangementSnapshotJson(const AiBridgeArrangementSnapshot &snapshot)
{
	QJsonArray clips;
	for (const auto &clip : snapshot.clips) clips.append(timelineClipJson(clip));
	QJsonArray automation;
	for (const auto &clip : snapshot.automationClips)
	{
		QJsonArray points;
		for (const auto &point : clip.points)
			points.append(QJsonObject{{"point_id", point.pointId}, {"tick", point.tick}, {"value", point.value}, {"curve", point.curve}});
		automation.append(QJsonObject{{"automation_track_id", clip.automationTrackId}, {"automation_clip_id", clip.automationClipId},
			{"automation_target_id", clip.automationTargetId}, {"start_tick", clip.startTick}, {"end_tick", clip.endTick}, {"points", points}});
	}
	return {{"project_generation", static_cast<qint64>(snapshot.projectGeneration)}, {"clips", clips}, {"automation_clips", automation}};
}

bool automationPointsEqual(const QVector<AiBridgeArrangementAutomationSnapshot::Point> &left,
	const QVector<AiBridgeArrangementAutomationSnapshot::Point> &right)
{
	if (left.size() != right.size()) return false;
	for (int i = 0; i < left.size(); ++i)
		if (left.at(i).pointId != right.at(i).pointId || left.at(i).tick != right.at(i).tick
			|| left.at(i).value != right.at(i).value || left.at(i).curve != right.at(i).curve) return false;
	return true;
}

bool automationPointContentEqual(const QVector<AiBridgeArrangementAutomationSnapshot::Point> &left,
	const QVector<AiBridgeArrangementAutomationSnapshot::Point> &right)
{
	if (left.size() != right.size()) return false;
	for (int i = 0; i < left.size(); ++i)
		if (left.at(i).tick != right.at(i).tick || left.at(i).value != right.at(i).value || left.at(i).curve != right.at(i).curve) return false;
	return true;
}

QJsonObject verifyArrangementPostState(const AiBridgeArrangementPlan &plan, const AiBridgeArrangementSnapshot &observed)
{
	QJsonArray expected; QJsonArray observedFacts; bool success = true;
	const auto findClip = [&](const QString &id) {
		for (const auto &clip : observed.clips) if (clip.clipId == id) return std::optional<AiBridgeTimelineClipSnapshot>(clip);
		return std::optional<AiBridgeTimelineClipSnapshot>{};
	};
	const auto findAutomation = [&](const QString &id) {
		for (const auto &clip : observed.automationClips) if (clip.automationClipId == id) return std::optional<AiBridgeArrangementAutomationSnapshot>(clip);
		return std::optional<AiBridgeArrangementAutomationSnapshot>{};
	};
	for (const auto &step : plan.steps)
	{
		const auto actual = findClip(step.clipId);
		const bool shouldExist = step.kind != AiBridgeArrangementStepKind::DeleteClip;
		bool matches = false;
		std::optional<AiBridgeTimelineClipSnapshot> matched;
		if (step.kind == AiBridgeArrangementStepKind::DuplicateClip)
		{
			qint64 length = 0;
			for (const auto &source : plan.affectedClips)
				if (source.clipId == step.clipId) { length = source.endTick - source.startTick; break; }
			for (const auto &clip : observed.clips)
				if (clip.clipId != step.clipId && clip.trackId == step.trackId && clip.startTick == step.destinationTick
					&& clip.endTick == step.destinationTick + length) { matched = clip; matches = true; break; }
		}
		else matches = shouldExist ? actual.has_value() && actual->trackId == step.trackId && actual->startTick == step.destinationTick : !actual.has_value();
		success = success && matches;
		expected.append(QJsonObject{{"kind", arrangementStepKind(step.kind)}, {"clip_id", step.clipId}, {"track_id", step.trackId},
			{"exists", shouldExist}, {"start_tick", step.destinationTick}, {"matches", matches}});
		const auto observedClip = step.kind == AiBridgeArrangementStepKind::DuplicateClip ? matched : actual;
		observedFacts.append(observedClip.has_value() ? timelineClipJson(*observedClip) : QJsonObject{{"clip_id", step.clipId}, {"stale", true}});
	}
	for (const auto &step : plan.automationSteps)
	{
		const auto actual = findAutomation(step.automationClipId);
		const bool shouldExist = step.kind != AiBridgeArrangementAutomationStepKind::DeleteClip;
		bool matches = false;
		std::optional<AiBridgeArrangementAutomationSnapshot> matched;
		if (step.kind == AiBridgeArrangementAutomationStepKind::DuplicateClip)
		{
			for (const auto &clip : observed.automationClips)
				if (clip.automationClipId != step.automationClipId && clip.automationTrackId == step.automationTrackId
					&& clip.automationTargetId == step.automationTargetId && clip.startTick == step.destinationStartTick
					&& clip.endTick == step.destinationEndTick && automationPointContentEqual(clip.points, step.points)) { matched = clip; matches = true; break; }
		}
		else matches = shouldExist ? actual.has_value() && actual->automationTrackId == step.automationTrackId
			&& actual->automationTargetId == step.automationTargetId && actual->startTick == step.destinationStartTick
			&& actual->endTick == step.destinationEndTick && automationPointsEqual(actual->points, step.points) : !actual.has_value();
		success = success && matches;
		expected.append(automationStepJson(step));
		const auto observedClip = step.kind == AiBridgeArrangementAutomationStepKind::DuplicateClip ? matched : actual;
		observedFacts.append(observedClip.has_value() ? QJsonObject{{"automation_clip_id", observedClip->automationClipId}, {"automation_track_id", observedClip->automationTrackId},
			{"automation_target_id", observedClip->automationTargetId}, {"start_tick", observedClip->startTick}, {"end_tick", observedClip->endTick}} : QJsonObject{{"automation_clip_id", step.automationClipId}, {"stale", true}});
	}
	return {{"success", success}, {"expected", expected}, {"observed", observedFacts},
		{"affected_start_tick", plan.resultingBounds.startTick}, {"affected_end_tick", plan.resultingBounds.endTick},
		{"project_generation", static_cast<qint64>(observed.projectGeneration)}};
}

bool isPhase3GAction(const QString &action)
{
	return action.startsWith(QStringLiteral("get_arrangement")) || action.startsWith(QStringLiteral("get_timeline_region"))
		|| action.startsWith(QStringLiteral("set_timeline_clip")) || action.startsWith(QStringLiteral("duplicate_timeline_clip"))
		|| action.startsWith(QStringLiteral("delete_timeline_clip")) || action.startsWith(QStringLiteral("batch_edit_timeline_clips"))
		|| action.startsWith(QStringLiteral("list_automation")) || action.startsWith(QStringLiteral("get_automation_"))
		|| action.startsWith(QStringLiteral("create_automation_")) || action.startsWith(QStringLiteral("delete_automation_"))
		|| action.startsWith(QStringLiteral("set_automation_")) || action == QStringLiteral("edit_automation_points")
		|| action.endsWith(QStringLiteral("_arrangement_region")) || action == QStringLiteral("insert_arrangement_space")
		|| action == QStringLiteral("close_arrangement_gap");
}
}

AiBridgeIpcServer::AiBridgeIpcServer(QObject *parent, QString endpoint) : QObject(parent), m_server(this), m_endpoint(std::move(endpoint))
{
	connect(&m_server, &QLocalServer::newConnection, this, &AiBridgeIpcServer::acceptConnection);
}

AiBridgeIpcServer::~AiBridgeIpcServer()
{
	stop();
}

bool AiBridgeIpcServer::start()
{
	if (m_server.isListening()) return true;
	return m_server.listen(m_endpoint);
}

void AiBridgeIpcServer::stop()
{
	if (m_client != nullptr)
	{
		m_client->disconnect(this);
		m_client->close();
		m_client->deleteLater();
		m_client = nullptr;
	}
	m_input.clear();
	m_busy = false;
	m_server.close();
}

void AiBridgeIpcServer::acceptConnection()
{
	while (m_server.hasPendingConnections())
	{
		auto *socket = m_server.nextPendingConnection();
		if (m_client != nullptr)
		{
			socket->close();
			socket->deleteLater();
			continue;
		}
		m_client = socket;
		connect(m_client, &QLocalSocket::readyRead, this, &AiBridgeIpcServer::readProbe);
		connect(m_client, &QLocalSocket::disconnected, this, &AiBridgeIpcServer::clientDisconnected);
	}
}

void AiBridgeIpcServer::readProbe()
{
	if (m_client == nullptr) return;
	m_input += m_client->readAll();
	if (m_input.size() > AiBridgeProtocol::MaxFrameBytes)
	{
		m_client->close();
		return;
	}
	for (auto newline = m_input.indexOf('\n'); newline >= 0; newline = m_input.indexOf('\n'))
	{
		const auto frame = m_input.left(newline + 1);
		m_input.remove(0, newline + 1);
		if (frame == QByteArrayLiteral("ping\n"))
		{
			m_client->write(QByteArrayLiteral("pong\n"));
			m_client->flush();
			continue;
		}
		const auto parsed = AiBridgeProtocol::parseRequest(frame);
		if (!parsed.success)
		{
			sendResponse({2, parsed.request.requestId, false, parsed.code, parsed.message});
			continue;
		}
		if (m_busy)
		{
			sendResponse({2, parsed.request.requestId, false, QStringLiteral("busy"), QStringLiteral("Another LMMS request is in flight")});
			continue;
		}
		m_busy = true;
		const auto request = parsed.request;
		QMetaObject::invokeMethod(this, [this, request] { executeRequest(request); }, Qt::QueuedConnection);
	}
}

void AiBridgeIpcServer::executeRequest(const AiBridgeProtocolRequest &request)
{
	AiBridgeProtocolResponse response{2, request.requestId, false, QStringLiteral("internal_error"), QStringLiteral("Request did not execute")};
	if (request.action == QStringLiteral("get_context"))
	{
		const auto snapshot = AiBridgeProjectContext{}.currentContext();
		QJsonObject data{{QStringLiteral("song_name"), snapshot.songName}, {QStringLiteral("tempo"), snapshot.tempo}, {QStringLiteral("has_current_midi_clip"), snapshot.hasCurrentMidiClip}, {QStringLiteral("clip_id"), snapshot.clipId}, {QStringLiteral("clip_length_ticks"), snapshot.clipLengthTicks}, {QStringLiteral("has_owning_instrument_track"), snapshot.hasOwningInstrumentTrack}, {QStringLiteral("track_id"), snapshot.trackId}, {QStringLiteral("track_name"), snapshot.trackName}, {QStringLiteral("track_volume"), snapshot.trackVolume}};
		response = {2, request.requestId, snapshot.success, snapshot.code, snapshot.message, data};
	}
	else if (request.action == QStringLiteral("get_midi_capabilities"))
	{
		response = {2, request.requestId, true, QStringLiteral("ok"),
			QStringLiteral("MIDI capabilities read"), midiCapabilitiesJson()};
	}
	else if (request.action == QStringLiteral("get_midi_notes"))
	{
		AiBridgeMidiClipSnapshot snapshot; QString code;
		if (!midiSnapshot(request.parameters.value("clip_id").toString(), snapshot, code)) response = {2, request.requestId, false, code, QStringLiteral("MIDI clip is not live")};
		else { const auto page = AiBridgeMidiContext::pageNotes(snapshot, request.parameters.value("offset").toInt(), request.parameters.contains("limit") ? request.parameters.value("limit").toInt() : AiBridgeMidiLimits::maxNotesPerPage); QJsonArray notes; for (int i = 0; i < page.notes.size(); ++i) notes.append(midiNoteJson(page.notes[i], page.noteRefs[i])); response = {2, request.requestId, true, QStringLiteral("ok"), QStringLiteral("MIDI notes read"), QJsonObject{{"clip_id", snapshot.clipId}, {"midi_state_hash", page.midiStateHash}, {"notes", notes}, {"total", page.total}, {"offset", page.offset}, {"limit", page.limit}, {"has_more", page.hasMore}}}; }
	}
	else if (request.action == QStringLiteral("analyze_midi"))
	{
		QVector<AiBridgeMidiClipSnapshot> clips; QString code;
		for (const auto &value : request.parameters.value("clip_ids").toArray()) { AiBridgeMidiClipSnapshot snapshot; if (!midiSnapshot(value.toString(), snapshot, code)) break; clips.append(snapshot); }
		if (!code.isEmpty()) response = {2, request.requestId, false, code, QStringLiteral("MIDI clip is not live")};
		else { int noteCount = 0; for (const auto &snapshot : clips) noteCount += snapshot.notes.size(); if (request.parameters.contains("max_notes") && noteCount > request.parameters.value("max_notes").toInt()) response = {2, request.requestId, false, QStringLiteral("note_limit_exceeded"), QStringLiteral("analysis exceeds requested note limit")}; else { AiBridgeMidiAnalysisOptions options; if (request.parameters.contains("grid_ticks")) options.gridTicks = request.parameters.value("grid_ticks").toVariant().toLongLong(); if (request.parameters.contains("short_note_threshold_ticks")) options.shortNoteThresholdTicks = request.parameters.value("short_note_threshold_ticks").toVariant().toLongLong(); const auto analysis = analyzeMidi(clips, options); response = {2, request.requestId, analysis.success, analysis.success ? QStringLiteral("ok") : analysis.code, analysis.success ? QStringLiteral("MIDI analysis complete") : QStringLiteral("MIDI analysis failed"), midiAnalysisJson(analysis)}; } }
	}
	else if (request.action == QStringLiteral("quantize_notes") || request.action == QStringLiteral("humanize_notes")
		|| request.action == QStringLiteral("apply_groove") || request.action == QStringLiteral("shape_note_velocities")
		|| request.action == QStringLiteral("shape_note_lengths") || request.action == QStringLiteral("cleanup_notes")
		|| request.action == QStringLiteral("constrain_notes_to_scale"))
	{
		response = executeMidiMutation(request);
	}
	else if (request.action == QStringLiteral("get_windows"))
	{
		const auto snapshot = AiBridgeWindowContext{}.snapshot();
		QJsonArray windows;
		for (const auto& item : snapshot.windows)
		{
			windows.append(QJsonObject{{QStringLiteral("id"), item.id}, {QStringLiteral("kind"), item.kind}, {QStringLiteral("title"), item.title}, {QStringLiteral("x"), item.geometry.x}, {QStringLiteral("y"), item.geometry.y}, {QStringLiteral("width"), item.geometry.width}, {QStringLiteral("height"), item.geometry.height}, {QStringLiteral("coordinate_space"), item.coordinateSpace}, {QStringLiteral("visible"), item.visible}, {QStringLiteral("active"), item.active}, {QStringLiteral("detached"), item.detached}, {QStringLiteral("maximized"), item.maximized}, {QStringLiteral("minimized"), item.minimized}, {QStringLiteral("overlaps"), QJsonArray::fromStringList(item.overlaps)}});
		}
		response = {2, request.requestId, snapshot.success, snapshot.code, snapshot.message, QJsonObject{{QStringLiteral("success"), snapshot.success}, {QStringLiteral("code"), snapshot.code}, {QStringLiteral("geometry"), QJsonObject{{QStringLiteral("width"), snapshot.geometry.width}, {QStringLiteral("height"), snapshot.geometry.height}}}, {QStringLiteral("windows"), windows}}};
	}
	else if (isPhase3GAction(request.action))
	{
		const auto &p = request.parameters;
		const int offset = p.value(QStringLiteral("offset")).toInt();
		const int limit = p.contains(QStringLiteral("limit")) ? p.value(QStringLiteral("limit")).toInt() : 64;
		if (request.action == QStringLiteral("get_arrangement") || request.action == QStringLiteral("get_timeline_region"))
		{
			AiBridgeTimelineRange range{p.value(QStringLiteral("start_tick")).toVariant().toLongLong(), p.value(QStringLiteral("end_tick")).toVariant().toLongLong()};
			AiBridgeTrackScope scope;
			scope.mode = p.value(QStringLiteral("track_scope")).toString();
			for (const auto &id : p.value(QStringLiteral("track_ids")).toArray()) scope.trackIds.append(id.toString());
			if (scope.mode.isEmpty()) scope.mode = scope.trackIds.isEmpty() ? QStringLiteral("all_tracks") : QStringLiteral("track_ids");
			const auto page = AiBridgeArrangementContext{}.region(range, scope, offset, limit);
			QJsonArray clips;
			for (const auto &item : page.items) clips.append(timelineClipJson(item));
			response = {2, request.requestId, true, QStringLiteral("ok"), QStringLiteral("arrangement read"),
				QJsonObject{{"clips", clips}, {"total", page.page.total}, {"offset", page.page.offset}, {"limit", page.page.limit}, {"has_more", page.page.hasMore}, {"markers_supported", false}}};
		}
		else if (request.action == QStringLiteral("set_timeline_clip") || request.action == QStringLiteral("duplicate_timeline_clip") || request.action == QStringLiteral("delete_timeline_clip"))
		{
			AiBridgeTimelineActionResult result;
			if (request.action == QStringLiteral("set_timeline_clip"))
			{
				AiBridgeTimelineEdit edit;
				edit.clipId = p.value(QStringLiteral("clip_id")).toString();
				if (p.contains(QStringLiteral("start_tick"))) edit.startTick = p.value(QStringLiteral("start_tick")).toVariant().toLongLong();
				if (p.contains(QStringLiteral("length_ticks"))) edit.lengthTicks = p.value(QStringLiteral("length_ticks")).toVariant().toLongLong();
				if (p.contains(QStringLiteral("auto_resize"))) edit.autoResize = p.value(QStringLiteral("auto_resize")).toBool();
				result = AiBridgeTimelineActions::setClip(edit);
			}
			else if (request.action == QStringLiteral("duplicate_timeline_clip")) result = AiBridgeTimelineActions::duplicateClip(p.value(QStringLiteral("clip_id")).toString(), p.value(QStringLiteral("destination_tick")).toVariant().toLongLong());
			else result = AiBridgeTimelineActions::deleteClip(p.value(QStringLiteral("clip_id")).toString());
			QJsonArray applied; for (const auto &id : result.appliedClipIds) applied.append(id);
			QJsonArray unapplied; for (const auto &id : result.unappliedClipIds) unapplied.append(id);
			response = {2, request.requestId, result.success, result.code, result.message,
				QJsonObject{{"clip_id", result.clipId}, {"track_id", result.trackId}, {"applied_clip_ids", applied}, {"unapplied_clip_ids", unapplied}}};
		}
		else if (request.action == QStringLiteral("batch_edit_timeline_clips"))
		{
			QVector<AiBridgeTimelineEdit> edits;
			for (const auto &value : p.value(QStringLiteral("edits")).toArray())
			{
				const auto item = value.toObject(); AiBridgeTimelineEdit edit; edit.clipId = item.value(QStringLiteral("clip_id")).toString();
				if (item.contains(QStringLiteral("start_tick"))) edit.startTick = item.value(QStringLiteral("start_tick")).toVariant().toLongLong();
				if (item.contains(QStringLiteral("length_ticks"))) edit.lengthTicks = item.value(QStringLiteral("length_ticks")).toVariant().toLongLong();
				if (item.contains(QStringLiteral("auto_resize"))) edit.autoResize = item.value(QStringLiteral("auto_resize")).toBool();
				edits.append(edit);
			}
			const auto result = AiBridgeTimelineActions::batchEdit(edits); QJsonArray applied; for (const auto &id : result.appliedClipIds) applied.append(id);
			QJsonArray unapplied; for (const auto &id : result.unappliedClipIds) unapplied.append(id);
			response = {2, request.requestId, result.success, result.code, result.message, QJsonObject{{"applied_clip_ids", applied}, {"unapplied_clip_ids", unapplied}}};
		}
		else if (request.action == QStringLiteral("list_automation_targets") || request.action == QStringLiteral("get_automation_capabilities"))
		{
			AiBridgeProjectContext project;
			AiBridgeAutomationReconciler(project, aiBridgeAutomationRegistry(),
				aiBridgeAutomationTargets()).reconcile();
			const auto items = request.action == QStringLiteral("list_automation_targets") ? AiBridgeAutomationContext{}.listTargets(offset, limit) : AiBridgeAutomationContext{}.capabilities(offset, limit);
			QJsonArray targets; for (const auto &item : items) targets.append(automationTargetJson(item));
			response = {2, request.requestId, true, QStringLiteral("ok"), QStringLiteral("automation targets read"), QJsonObject{{"targets", targets}, {"markers_supported", false}}};
		}
		else if (request.action == QStringLiteral("list_automation"))
		{
			const auto tracks = AiBridgeAutomationContext{}.listTracks(offset, limit); QJsonArray items;
			for (const auto &track : tracks.items) { QJsonArray clips; for (const auto &id : track.automationClipIds) clips.append(id); items.append(QJsonObject{{"automation_track_id", track.automationTrackId}, {"name", track.name}, {"index", track.index}, {"automation_clip_ids", clips}}); }
			response = {2, request.requestId, true, QStringLiteral("ok"), QStringLiteral("automation read"), QJsonObject{{"tracks", items}, {"total", tracks.page.total}, {"offset", tracks.page.offset}, {"limit", tracks.page.limit}, {"has_more", tracks.page.hasMore}}};
		}
		else if (request.action == QStringLiteral("get_automation_clip"))
		{
			AiBridgeAutomationClipSnapshot clip; AiBridgeAutomationPointPage points;
			const bool ok = AiBridgeAutomationContext{}.getClip(p.value(QStringLiteral("automation_clip_id")).toString(), offset, limit, clip, points);
			QJsonObject data = automationClipJson(clip); data.insert("total", points.page.total); data.insert("offset", points.page.offset); data.insert("limit", points.page.limit); data.insert("has_more", points.page.hasMore);
			response = {2, request.requestId, ok, ok ? QStringLiteral("ok") : QStringLiteral("stale_automation_clip"), ok ? QStringLiteral("automation clip read") : QStringLiteral("automation clip is not live"), data};
		}
		else if (request.action == QStringLiteral("create_automation_track") || request.action == QStringLiteral("delete_automation_track") || request.action == QStringLiteral("create_automation_clip") || request.action == QStringLiteral("delete_automation_clip") || request.action == QStringLiteral("set_automation_clip_properties") || request.action == QStringLiteral("set_automation_clip_target") || request.action == QStringLiteral("edit_automation_points"))
		{
			AiBridgeAutomationActionResult result;
			if (request.action == QStringLiteral("create_automation_track")) result = AiBridgeAutomationActions::createTrack(p.value(QStringLiteral("name")).toString());
			else if (request.action == QStringLiteral("delete_automation_track")) result = AiBridgeAutomationActions::deleteTrack(p.value(QStringLiteral("automation_track_id")).toString());
			else if (request.action == QStringLiteral("create_automation_clip")) result = AiBridgeAutomationActions::createClip(p.value(QStringLiteral("automation_track_id")).toString(), p.value(QStringLiteral("start_tick")).toVariant().toLongLong(), p.value(QStringLiteral("length_ticks")).toVariant().toLongLong(), p.value(QStringLiteral("automation_target_id")).toString());
			else if (request.action == QStringLiteral("delete_automation_clip")) result = AiBridgeAutomationActions::deleteClip(p.value(QStringLiteral("automation_clip_id")).toString());
			else if (request.action == QStringLiteral("set_automation_clip_properties")) result = AiBridgeAutomationActions::setClipProperties(p.value(QStringLiteral("automation_clip_id")).toString(), p.contains(QStringLiteral("start_tick")) ? std::optional<qint64>(p.value(QStringLiteral("start_tick")).toVariant().toLongLong()) : std::nullopt, p.contains(QStringLiteral("length_ticks")) ? std::optional<qint64>(p.value(QStringLiteral("length_ticks")).toVariant().toLongLong()) : std::nullopt);
			else if (request.action == QStringLiteral("set_automation_clip_target")) result = AiBridgeAutomationActions::setClipTarget(p.value(QStringLiteral("automation_clip_id")).toString(), p.value(QStringLiteral("automation_target_id")).toString());
			else
			{
				QVector<AiBridgeAutomationPointMutation> mutations;
				for (const auto &value : p.value(QStringLiteral("mutations")).toArray())
				{
					const auto item = value.toObject(); AiBridgeAutomationPointMutation mutation;
					const auto operation = item.value(QStringLiteral("operation")).toString();
					mutation.kind = operation == QStringLiteral("update") ? AiBridgeAutomationPointMutation::Kind::Update : operation == QStringLiteral("delete") ? AiBridgeAutomationPointMutation::Kind::Delete : AiBridgeAutomationPointMutation::Kind::Add;
					mutation.pointId = item.value(QStringLiteral("point_id")).toString();
					if (item.contains(QStringLiteral("tick"))) mutation.tick = item.value(QStringLiteral("tick")).toVariant().toLongLong();
					if (item.contains(QStringLiteral("value"))) mutation.value = static_cast<float>(item.value(QStringLiteral("value")).toDouble());
					if (item.contains(QStringLiteral("curve"))) mutation.curve = item.value(QStringLiteral("curve")).toString();
					mutations.append(mutation);
				}
				result = AiBridgeAutomationActions::editPoints(p.value(QStringLiteral("automation_clip_id")).toString(), mutations);
			}
			response = {2, request.requestId, result.success, result.code, result.message, automationActionJson(result)};
		}
		else
		{
			AiBridgeArrangementRequest arrangement;
			arrangement.operation = request.action == QStringLiteral("duplicate_arrangement_region") ? QStringLiteral("duplicate") : request.action == QStringLiteral("move_arrangement_region") ? QStringLiteral("move") : request.action == QStringLiteral("delete_arrangement_region") ? QStringLiteral("delete") : request.action == QStringLiteral("insert_arrangement_space") ? QStringLiteral("insert_space") : request.action == QStringLiteral("close_arrangement_gap") ? QStringLiteral("close_gap") : QStringLiteral("replace");
			arrangement.source.startTick = p.value(QStringLiteral("start_tick")).toVariant().toLongLong(); arrangement.source.endTick = p.value(QStringLiteral("end_tick")).toVariant().toLongLong();
			arrangement.destinationTick = p.value(QStringLiteral("destination_tick")).toVariant().toLongLong(); arrangement.deltaTick = p.value(QStringLiteral("length_ticks")).toVariant().toLongLong();
			arrangement.scope.mode = p.value(QStringLiteral("track_scope")).toString();
			for (const auto &id : p.value(QStringLiteral("track_ids")).toArray()) arrangement.scope.trackIds.append(id.toString());
			if (arrangement.scope.mode.isEmpty()) arrangement.scope.mode = arrangement.scope.trackIds.isEmpty() ? QStringLiteral("all_tracks") : QStringLiteral("track_ids");
			arrangement.includeAutomation = p.value(QStringLiteral("include_automation")).toBool(true); arrangement.overwrite = p.value(QStringLiteral("overwrite")).toBool(false); arrangement.ripple = p.value(QStringLiteral("ripple")).toBool(false); arrangement.dryRun = p.value(QStringLiteral("dry_run")).toBool(false);
			const auto snapshot = arrangementSnapshot();
			const auto plan = AiBridgeArrangementPlanner::plan(arrangement, snapshot);
			const auto valid = AiBridgeArrangementPlanner::validate(arrangement);
			if (!valid.success() || arrangement.dryRun || !plan.conflicts.isEmpty() || plan.noEffect)
			{
				const bool noEffect = valid.success() && !arrangement.dryRun && plan.conflicts.isEmpty() && plan.noEffect;
				const bool ok = valid.success() && (arrangement.dryRun || noEffect || plan.conflicts.isEmpty());
				response = {2, request.requestId, ok, noEffect ? QStringLiteral("no_effect") : ok ? QStringLiteral("ok") : (plan.conflicts.isEmpty() ? valid.code : QStringLiteral("destination_conflict")), noEffect ? plan.noEffectReason : ok ? (arrangement.dryRun ? QStringLiteral("arrangement plan generated") : QStringLiteral("arrangement execution preflight complete")) : (plan.conflicts.isEmpty() ? valid.message : QStringLiteral("destination conflicts require overwrite")), arrangementPlanJson(plan)};
			}
			else
			{
				AiBridgeProjectContext context;
				const auto generation = context.projectGeneration();
				bool preflight = true;
				for (const auto &step : plan.steps)
					if (step.kind != AiBridgeArrangementStepKind::DuplicateClip && !context.resolveClip(step.clipId)) preflight = false;
				if (!preflight)
				{
					response = {2, request.requestId, false, QStringLiteral("stale_clip"), QStringLiteral("arrangement plan contains a stale clip"), arrangementPlanJson(plan)};
				}
				else
				{
					const auto executed = AiBridgeArrangementActions::executeTyped(plan, generation, context.projectGeneration(),
						[&](const AiBridgeArrangementStep &step) {
							if (step.kind == AiBridgeArrangementStepKind::DeleteClip) return AiBridgeTimelineActions::deleteClip(step.clipId).success;
							if (step.kind == AiBridgeArrangementStepKind::DuplicateClip) return AiBridgeTimelineActions::duplicateClip(step.clipId, step.destinationTick).success;
							AiBridgeTimelineEdit edit; edit.clipId = step.clipId; edit.startTick = step.destinationTick;
							return AiBridgeTimelineActions::setClip(edit).success;
						}, {});
					int automationFailureIndex = -1;
					AiBridgeAutomationActionResult automationFailure;
					if (executed.success && arrangement.includeAutomation)
					{
						for (int i = 0; i < plan.automationSteps.size(); ++i)
						{
							const auto &step = plan.automationSteps.at(i);
							if (step.kind == AiBridgeArrangementAutomationStepKind::DeleteClip)
								automationFailure = AiBridgeAutomationActions::deleteClip(step.automationClipId);
							else if (step.kind == AiBridgeArrangementAutomationStepKind::DuplicateClip)
								automationFailure = AiBridgeAutomationActions::duplicateClip(step.automationClipId, step.automationTrackId, step.destinationStartTick);
							else
								automationFailure = AiBridgeAutomationActions::setClipProperties(step.automationClipId, step.destinationStartTick, std::nullopt);
							if (!automationFailure.success) { automationFailureIndex = i; break; }
						}
					}
					QJsonObject data = arrangementPlanJson(plan);
					QJsonArray applied; for (const auto &item : executed.appliedSteps) applied.append(item);
					QJsonArray unapplied; for (const auto &item : executed.unappliedSteps) unapplied.append(item);
					data.insert("applied_steps", applied); data.insert("unapplied_steps", unapplied);
					QJsonArray appliedTyped; for (const auto &item : executed.appliedTypedSteps) appliedTyped.append(arrangementStepJson(item));
					QJsonArray unappliedTyped; for (const auto &item : executed.unappliedTypedSteps) unappliedTyped.append(arrangementStepJson(item));
					data.insert("preflight_succeeded", executed.preflightSucceeded);
					data.insert("execution_complete", executed.executionComplete);
					data.insert("partial_mutation", executed.partialMutation);
					data.insert("failed_step_index", executed.failedStepIndex);
					if (executed.hasFailedTypedStep) data.insert("failed_step", arrangementStepJson(executed.failedTypedStep));
					data.insert("applied_typed_steps", appliedTyped); data.insert("unapplied_typed_steps", unappliedTyped);
					QJsonArray affectedTracks; for (const auto &trackId : executed.affectedTrackIds) affectedTracks.append(trackId);
					data.insert("affected_track_ids", affectedTracks);
					data.insert("affected_start_tick", executed.affectedStartTick); data.insert("affected_end_tick", executed.affectedEndTick);
					data.insert("project_generation", static_cast<qint64>(executed.projectGeneration));
					data.insert("native_failure_code", executed.nativeFailureCode); data.insert("native_failure_message", executed.nativeFailureMessage);
					QJsonArray appliedAutomation;
					QJsonArray unappliedAutomation;
					for (int i = 0; i < plan.automationSteps.size(); ++i)
						(i < (automationFailureIndex < 0 ? plan.automationSteps.size() : automationFailureIndex) ? appliedAutomation : unappliedAutomation).append(i);
					data.insert("applied_automation_steps", appliedAutomation); data.insert("unapplied_automation_steps", unappliedAutomation);
					const bool automationOk = automationFailureIndex < 0;
					const auto observed = arrangementSnapshot();
					const auto verification = verifyArrangementPostState(plan, observed);
					const bool postVerificationOk = verification.value("success").toBool();
					data.insert("post_verification", verification);
					data.insert("observed_post_state", arrangementSnapshotJson(observed));
					response = {2, request.requestId, executed.success && automationOk && postVerificationOk,
						!postVerificationOk ? QStringLiteral("post_verification_failed") : automationOk ? executed.code : QStringLiteral("partial_failure"),
						!postVerificationOk ? QStringLiteral("native result did not match post-execution verification") : automationOk ? executed.message : automationFailure.message, data};
				}
			}
		}
	}
	else
	{
		if (request.action == QStringLiteral("get_project_structure") || request.action == QStringLiteral("list_tracks") || request.action == QStringLiteral("list_clips") || request.action == QStringLiteral("get_clip") || request.action == QStringLiteral("open_clip_in_piano_roll") || request.action == QStringLiteral("list_instruments") || request.action == QStringLiteral("create_instrument_track") || request.action == QStringLiteral("rename_track") || request.action == QStringLiteral("create_midi_clip") || request.action == QStringLiteral("set_clip_properties") || request.action == QStringLiteral("set_track_instrument") || request.action == QStringLiteral("set_track_properties") || request.action == QStringLiteral("list_bundled_samples") || request.action == QStringLiteral("load_bundled_sample") || request.action == QStringLiteral("list_bundled_presets") || request.action == QStringLiteral("load_bundled_preset"))
		{
			const auto& p = request.parameters;
			const int offset = p.value(QStringLiteral("offset")).toInt();
			const int limit = p.contains(QStringLiteral("limit")) ? p.value(QStringLiteral("limit")).toInt() : 64;
			AiBridgeProjectActionResult result;
			AiBridgeAssetFilter assetFilter;
			assetFilter.instrumentId = p.value(QStringLiteral("instrument_id")).toString();
			assetFilter.query = p.value(QStringLiteral("query")).toString();
			assetFilter.category = p.value(QStringLiteral("category")).toString();
			assetFilter.format = p.value(QStringLiteral("format")).toString();
			assetFilter.offset = offset;
			assetFilter.limit = limit;
			if (request.action == QStringLiteral("get_project_structure")) result = AiBridgeProjectActions::getProjectStructure();
			else if (request.action == QStringLiteral("list_tracks")) result = AiBridgeProjectActions::listTracks(offset,limit);
			else if (request.action == QStringLiteral("list_clips")) result = AiBridgeProjectActions::listClips(p.value(QStringLiteral("track_id")).toString(),offset,limit);
			else if (request.action == QStringLiteral("get_clip")) result = AiBridgeProjectActions::getClip(p.value(QStringLiteral("clip_id")).toString(),offset,limit);
			else if (request.action == QStringLiteral("open_clip_in_piano_roll")) result = AiBridgeProjectActions::openClipInPianoRoll(p.value(QStringLiteral("clip_id")).toString());
			else if (request.action == QStringLiteral("list_instruments")) result = AiBridgeProjectActions::listInstruments(offset,limit);
			else if (request.action == QStringLiteral("create_instrument_track")) result = AiBridgeProjectActions::createInstrumentTrack(p.contains(QStringLiteral("name")) ? std::optional<QString>(p.value(QStringLiteral("name")).toString()) : std::nullopt);
			else if (request.action == QStringLiteral("rename_track")) result = AiBridgeProjectActions::renameTrack(p.value(QStringLiteral("track_id")).toString(),p.value(QStringLiteral("name")).toString());
			else if (request.action == QStringLiteral("create_midi_clip")) result = AiBridgeProjectActions::createMidiClip(p.value(QStringLiteral("track_id")).toString(),p.value(QStringLiteral("start_ticks")).toVariant().toLongLong(),p.contains(QStringLiteral("length_ticks")) ? std::optional<qint64>(p.value(QStringLiteral("length_ticks")).toVariant().toLongLong()) : std::nullopt,p.value(QStringLiteral("auto_resize")).toBool(false));
			else if (request.action == QStringLiteral("set_clip_properties")) result = AiBridgeProjectActions::setClipProperties(p.value(QStringLiteral("clip_id")).toString(),p.contains(QStringLiteral("start_ticks")) ? std::optional<qint64>(p.value(QStringLiteral("start_ticks")).toVariant().toLongLong()) : std::nullopt,p.contains(QStringLiteral("length_ticks")) ? std::optional<qint64>(p.value(QStringLiteral("length_ticks")).toVariant().toLongLong()) : std::nullopt,p.contains(QStringLiteral("auto_resize")) ? std::optional<bool>(p.value(QStringLiteral("auto_resize")).toBool()) : std::nullopt,p.contains(QStringLiteral("expected_content_hash")) ? std::optional<QString>(p.value(QStringLiteral("expected_content_hash")).toString()) : std::nullopt,p.contains(QStringLiteral("expected_start_ticks")) ? std::optional<qint64>(p.value(QStringLiteral("expected_start_ticks")).toVariant().toLongLong()) : std::nullopt);
			else if (request.action == QStringLiteral("list_bundled_samples")) result = AiBridgeProjectActions::listBundledSamples(assetFilter);
			else if (request.action == QStringLiteral("list_bundled_presets")) result = AiBridgeProjectActions::listBundledPresets(assetFilter);
			else if (request.action == QStringLiteral("load_bundled_sample")) result = AiBridgeProjectActions::loadBundledSample(p.value(QStringLiteral("track_id")).toString(), p.value(QStringLiteral("asset_id")).toString());
			else if (request.action == QStringLiteral("load_bundled_preset")) result = AiBridgeProjectActions::loadBundledPreset(p.value(QStringLiteral("track_id")).toString(), p.value(QStringLiteral("asset_id")).toString());
			else if (request.action == QStringLiteral("set_track_instrument")) result = AiBridgeProjectActions::setTrackInstrument(p.value(QStringLiteral("track_id")).toString(),p.value(QStringLiteral("instrument_id")).toString());
			else result = AiBridgeProjectActions::setTrackProperties(p.value(QStringLiteral("track_id")).toString(),p.contains(QStringLiteral("volume")) ? std::optional<int>(p.value(QStringLiteral("volume")).toInt()) : std::nullopt,p.contains(QStringLiteral("panning")) ? std::optional<int>(p.value(QStringLiteral("panning")).toInt()) : std::nullopt);
			QJsonObject data;
			if (request.action == QStringLiteral("get_project_structure"))
			{
				QJsonArray tracks; for (const auto& item : result.project.tracks) tracks.append(trackJson(item));
				data = {{"project_generation", static_cast<qint64>(AiBridgeProjectContext{}.projectGeneration())}, {"song", QJsonObject{{"name",result.project.songName},{"tempo",result.project.tempo},{"length_bars",result.project.lengthBars},{"length_ticks",result.project.lengthTicks},{"track_count",result.project.trackCount}}}, {"tracks",tracks}};
			}
			else if (request.action == QStringLiteral("list_tracks")) { QJsonArray items; for(const auto& item:result.tracks)items.append(trackJson(item)); data=pageJson(result.page);data.insert("tracks",items); }
			else if (request.action == QStringLiteral("list_clips")) { QJsonArray items; for(const auto& item:result.clips)items.append(clipJson(item)); data=pageJson(result.page);data.insert("clips",items); }
			else if (request.action == QStringLiteral("list_instruments")) { QJsonArray items; for(const auto& item:result.instruments)items.append(instrumentJson(item)); data=pageJson(result.page);data.insert("instruments",items); }
			else if (request.action == QStringLiteral("create_instrument_track") || request.action == QStringLiteral("rename_track") || request.action == QStringLiteral("set_track_instrument")) data=constructionTrackJson(result.track);
			else if (request.action == QStringLiteral("create_midi_clip")) data=constructionClipJson(result.constructionClip);
			else if (request.action == QStringLiteral("set_clip_properties")) data=clipPropertiesJson(result.clipProperties);
			else if (request.action == QStringLiteral("set_track_properties")) data=trackPropertiesJson(result.trackProperties);
			else if (request.action == QStringLiteral("list_bundled_samples")) data=samplePageJson(result.samples);
			else if (request.action == QStringLiteral("list_bundled_presets")) data=presetPageJson(result.presets);
			else if (request.action == QStringLiteral("load_bundled_sample") || request.action == QStringLiteral("load_bundled_preset")) data=QJsonObject{{"asset_id",result.assetLoad.assetId},{"track_id",result.assetLoad.trackId},{"track_name",result.assetLoad.trackName},{"instrument_id",result.assetLoad.instrumentId}};
			else { data=clipJson(result.clip); data.insert("content_hash",result.clip.contentHash); data.insert("auto_resize",result.clip.autoResize); data.insert("total",result.page.total);data.insert("offset",result.page.offset);data.insert("limit",result.page.limit);data.insert("has_more",result.page.hasMore); QJsonArray notes;for(const auto& n:result.clip.notes)notes.append(QJsonObject{{"position",n.position},{"length",n.length},{"key",n.key},{"volume",n.volume},{"panning",n.panning},{"note_type",n.noteType}});data.insert("notes",notes); if(request.action==QStringLiteral("open_clip_in_piano_roll")){const auto& c=result.context;data.insert("context",QJsonObject{{"clip_id",c.clipId},{"track_id",c.trackId},{"track_name",c.trackName},{"project_generation",static_cast<qint64>(c.projectGeneration)}});} }
			response = {2,request.requestId,result.success,result.code,result.message,data};
		}
		else if (isMixerAction(request.action))
		{
			const auto &p = request.parameters;
			AiBridgeProtocolResponse mixerResponse{2, request.requestId, false, "internal_error", "mixer request did not execute"};
			if (request.action == "get_mixer_structure")
			{
				const auto structure = AiBridgeMixerContext(aiBridgeMixerRegistry()).snapshot(p.value("offset").toInt(), p.contains("limit") ? p.value("limit").toInt() : 64);
				mixerResponse = {2, request.requestId, true, "ok", "mixer structure read", mixerStructureJson(structure)};
			}
			else
			{
				AiBridgeMixerActionResult result;
				if (request.action == "create_mixer_channel") result = AiBridgeMixerActions::createChannel(p.contains("name") ? std::optional<QString>(p.value("name").toString()) : std::nullopt);
				else if (request.action == "delete_mixer_channel") result = AiBridgeMixerActions::deleteChannel(p.value("mixer_channel_id").toString());
				else if (request.action == "set_mixer_channel_properties")
				{
					AiBridgeMixerChannelProperties properties;
					if (p.contains("name")) properties.name = p.value("name").toString();
					if (p.contains("volume")) properties.volume = static_cast<float>(p.value("volume").toDouble());
					if (p.contains("mute")) properties.mute = p.value("mute").toBool();
					if (p.contains("solo")) properties.solo = p.value("solo").toBool();
					result = AiBridgeMixerActions::setChannelProperties(p.value("mixer_channel_id").toString(), properties);
				}
				else if (request.action == "route_track_to_mixer_channel") result = AiBridgeMixerActions::routeTrackToChannel(p.value("track_id").toString(), p.value("mixer_channel_id").toString());
				else if (request.action == "create_mixer_route") result = AiBridgeMixerActions::createRoute(p.value("source_channel_id").toString(), p.value("destination_channel_id").toString(), static_cast<float>(p.value("amount").toDouble()));
				else if (request.action == "delete_mixer_route") result = AiBridgeMixerActions::deleteRoute(p.value("mixer_route_id").toString());
				else { AiBridgeMixerRouteProperties properties; properties.amount = static_cast<float>(p.value("amount").toDouble()); result = AiBridgeMixerActions::setRouteProperties(p.value("mixer_route_id").toString(), properties); }
				QJsonObject data = !result.route.mixerRouteId.isEmpty() ? routeJson(result.route) : channelJson(result.channel);
				if (!result.trackId.isEmpty()) data.insert("track_id", result.trackId);
				mixerResponse = {2, request.requestId, result.success, result.code, result.message, data};
			}
			response = mixerResponse;
		}
		else if (isEffectAction(request.action))
		{
			const auto &p = request.parameters;
			if (request.action == "list_native_effects")
			{
				const auto page = AiBridgeEffectCatalog::list(p.value("offset").toInt(), p.contains("limit") ? p.value("limit").toInt() : 64, p.value("filter").toString());
				QJsonArray items; for (const auto &item : page.items) items.append(QJsonObject{{"effect_id", item.id}, {"name", item.name}, {"description", item.description}});
				response = {2, request.requestId, true, "ok", "effect catalog read", QJsonObject{{"effects", items}, {"total", page.page.total}, {"offset", page.page.offset}, {"limit", page.page.limit}, {"has_more", page.page.hasMore}}};
			}
			else if (request.action == "get_mixer_meters")
			{
				std::optional<QVector<QString>> requested;
				if (p.contains("mixer_channel_ids")) { QVector<QString> ids; for (const auto &value : p.value("mixer_channel_ids").toArray()) ids.append(value.toString()); requested = ids; }
				const auto result = AiBridgeMeterActions::get(requested, p.value("offset").toInt(), p.contains("limit") ? p.value("limit").toInt() : 64);
				response = {2, request.requestId, result.success, result.code, result.message, meterPageJson(result)};
			}
			else if (request.action == "get_effect_parameters" || request.action == "set_effect_parameters")
			{
				AiBridgeEffectParameterActionResult result;
				const auto id = p.value("effect_instance_id").toString();
				if (request.action == "get_effect_parameters") result = AiBridgeEffectActions::getParameters(id, p.value("offset").toInt(), p.contains("limit") ? p.value("limit").toInt() : 128);
				else { QVector<AiBridgeEffectParameterChange> changes; for (const auto &value : p.value("changes").toArray()) { const auto object = value.toObject(); changes.append({object.value("id").toString(), parameterType(object.value("type").toString()), static_cast<float>(object.value("value").toDouble())}); } result = AiBridgeEffectActions::setParameters(id, changes); }
				response = {2, request.requestId, result.success, result.code, result.message, parameterPageJson(result)};
			}
			else
			{
				AiBridgeEffectActionResult result;
				if (request.action == "add_effect") result = AiBridgeEffectActions::add(p.value("mixer_channel_id").toString(), p.value("effect_id").toString(), p.value("position").toInt());
				else if (request.action == "remove_effect") result = AiBridgeEffectActions::remove(p.value("effect_instance_id").toString());
				else if (request.action == "move_effect") result = AiBridgeEffectActions::move(p.value("effect_instance_id").toString(), p.value("position").toInt());
				else result = AiBridgeEffectActions::setState(p.value("effect_instance_id").toString(), p.value("enabled").toBool(), static_cast<float>(p.value("wet_dry").toDouble()));
				response = {2, request.requestId, result.success, result.code, result.message, effectActionJson(result)};
			}
		}
		else
		{
		if (request.action == QStringLiteral("add_notes") || request.action == QStringLiteral("delete_notes") || request.action == QStringLiteral("transpose_notes") || request.action == QStringLiteral("move_notes") || request.action == QStringLiteral("duplicate_region"))
		{
			const auto &p = request.parameters;
			AiBridgeProjectActionResult result;
			const auto id = p.value(QStringLiteral("clip_id")).toString();
			const auto expected = p.value(QStringLiteral("expected_content_hash")).toString();
			if (request.action == QStringLiteral("add_notes"))
			{
				QVector<AiBridgeCopiedNote> notes;
				for (const auto &value : p.value(QStringLiteral("notes")).toArray()) notes.append(copiedNote(value));
				result = AiBridgeProjectActions::addNotes(id, notes, expected);
			}
			else if (request.action == QStringLiteral("delete_notes"))
				result = AiBridgeProjectActions::deleteNotes(id, selection(p.value(QStringLiteral("selection")).toObject()), expected);
			else if (request.action == QStringLiteral("transpose_notes"))
				result = AiBridgeProjectActions::transposeNotes(id, selection(p.value(QStringLiteral("selection")).toObject()), p.value(QStringLiteral("semitones")).toInt(), expected);
			else if (request.action == QStringLiteral("move_notes"))
				result = AiBridgeProjectActions::moveNotes(id, selection(p.value(QStringLiteral("selection")).toObject()), p.value(QStringLiteral("delta_ticks")).toVariant().toLongLong(), expected);
			else
				result = AiBridgeProjectActions::duplicateRegion(id, p.value(QStringLiteral("source_start")).toVariant().toLongLong(), p.value(QStringLiteral("source_end")).toVariant().toLongLong(), p.value(QStringLiteral("destination_start")).toVariant().toLongLong(), expected);
			response = {2, request.requestId, result.success, result.code, result.message, mutationJson(result.mutation)};
		}
		else
		{
		AiBridgeContext context;
		AiBridgeActionResult result;
		if (request.action == QStringLiteral("set_tempo")) result = AiBridgeActions::setSongTempo(context, request.parameters.value(QStringLiteral("bpm")).toInt());
		else if (request.action == QStringLiteral("adjust_current_track_volume")) result = AiBridgeActions::adjustCurrentInstrumentVolume(context, static_cast<float>(request.parameters.value(QStringLiteral("delta")).toDouble()));
		else if (request.action == QStringLiteral("add_note_to_current_clip")) result = AiBridgeActions::insertNoteIntoCurrentMidiClip(context, request.parameters.value(QStringLiteral("key")).toInt(), request.parameters.value(QStringLiteral("position")).toInt(), request.parameters.value(QStringLiteral("length")).toInt(), request.parameters.value(QStringLiteral("velocity")).toInt());
		else
		{
			AiBridgeWindowContext windowContext;
			if (request.action == QStringLiteral("focus_window")) result = AiBridgeWindowActions::focusWindow(windowContext, request.parameters.value(QStringLiteral("id")).toString());
			else if (request.action == QStringLiteral("set_window_geometry")) result = AiBridgeWindowActions::setWindowGeometry(windowContext, request.parameters.value(QStringLiteral("id")).toString(), {request.parameters.value(QStringLiteral("x")).toInt(), request.parameters.value(QStringLiteral("y")).toInt(), request.parameters.value(QStringLiteral("width")).toInt(), request.parameters.value(QStringLiteral("height")).toInt()});
			else result = AiBridgeWindowActions::arrangeWindows(windowContext, request.parameters.value(QStringLiteral("strategy")).toString());
		}
		response = {2, request.requestId, result.success, result.code, result.message};
		}
		}
	}
	m_busy = false;
	sendResponse(response);
}

void AiBridgeIpcServer::sendResponse(const AiBridgeProtocolResponse &response)
{
	if (m_client == nullptr || m_client->state() != QLocalSocket::ConnectedState) return;
	m_client->write(AiBridgeProtocol::serializeResponse(response));
	m_client->flush();
}

void AiBridgeIpcServer::clientDisconnected()
{
	if (m_client == nullptr) return;
	const auto *socket = sender();
	if (socket != m_client) return;
	m_client->deleteLater();
	m_client = nullptr;
	m_input.clear();
}

} // namespace lmms
