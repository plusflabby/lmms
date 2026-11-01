#include "AiBridgeRecordingTypes.h"

#include <QJsonArray>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace lmms
{
namespace
{
AiBridgeRecordingParseResult invalid(const QString& code, const QString& message)
{
	return {false, code, message, {}};
}

bool onlyKeys(const QJsonObject& object, std::initializer_list<QString> keys)
{
	for (auto it = object.constBegin(); it != object.constEnd(); ++it)
		if (std::find(keys.begin(), keys.end(), it.key()) == keys.end()) return false;
	return true;
}

bool integer(const QJsonValue& value, qint64 minimum, qint64 maximum, qint64& output)
{
	if (!value.isDouble()) return false;
	const auto number = value.toDouble();
	if (!std::isfinite(number) || std::floor(number) != number || number < double(minimum) || number > double(maximum)) return false;
	output = qint64(number);
	return true;
}

bool canonicalGeneration(const QJsonValue& value, quint64& output)
{
	if (!value.isString()) return false;
	const auto text = value.toString();
	if (text.isEmpty()) return false;
	for (const auto character : text)
		if (character < QChar('0') || character > QChar('9')) return false;
	bool ok = false;
	output = text.toULongLong(&ok);
	return ok && output != 0 && QString::number(output) == text;
}

bool canonicalUuid(const QJsonValue& value, QString& output)
{
	if (!value.isString()) return false;
	const auto text = value.toString();
	const QUuid id(text);
	if (id.isNull() || id.toString(QUuid::WithoutBraces) != text) return false;
	output = text;
	return true;
}

QString phaseName(recording::Phase phase)
{
	switch (phase)
	{
	case recording::Phase::Disarmed: return QStringLiteral("disarmed");
	case recording::Phase::Armed: return QStringLiteral("armed");
	case recording::Phase::Prepared: return QStringLiteral("prepared");
	case recording::Phase::Priming: return QStringLiteral("priming");
	case recording::Phase::Preroll: return QStringLiteral("preroll");
	case recording::Phase::Recording: return QStringLiteral("recording");
	case recording::Phase::Quiescing: return QStringLiteral("quiescing");
	case recording::Phase::Finalizing: return QStringLiteral("finalizing");
	}
	return QStringLiteral("unavailable");
}

QString integrityName(recording::CaptureIntegrity value)
{
	switch (value)
	{
	case recording::CaptureIntegrity::Pending: return QStringLiteral("pending");
	case recording::CaptureIntegrity::Complete: return QStringLiteral("complete");
	case recording::CaptureIntegrity::Incomplete: return QStringLiteral("incomplete");
	case recording::CaptureIntegrity::Failed: return QStringLiteral("failed");
	}
	return QStringLiteral("failed");
}

QString placementName(recording::PlacementState value)
{
	switch (value)
	{
	case recording::PlacementState::AwaitingLocalPlacement: return QStringLiteral("awaiting_local_placement");
	case recording::PlacementState::ConfirmedUnattached: return QStringLiteral("confirmed_unattached");
	case recording::PlacementState::Attached: return QStringLiteral("attached");
	case recording::PlacementState::SavedUnattached: return QStringLiteral("saved_unattached");
	}
	return QStringLiteral("saved_unattached");
}

QString timestampQualityName(recording::InputTimestampQuality value)
{
	switch (value)
	{
	case recording::InputTimestampQuality::Observed: return QStringLiteral("observed_uncalibrated");
	case recording::InputTimestampQuality::Unavailable: return QStringLiteral("unavailable");
	case recording::InputTimestampQuality::Discontinuous: return QStringLiteral("discontinuous");
	}
	return QStringLiteral("unavailable");
}
}

bool AiBridgeRecordingTypes::handles(const QString& action)
{
	return action == QStringLiteral("get_recording_capabilities")
		|| action == QStringLiteral("get_recording_status")
		|| action == QStringLiteral("prepare_recording_take")
		|| action == QStringLiteral("start_recording_take")
		|| action == QStringLiteral("stop_recording_take")
		|| action == QStringLiteral("list_recorded_takes");
}

AiBridgeRecordingParseResult AiBridgeRecordingTypes::parse(const QString& action, const QJsonObject& parameters)
{
	AiBridgeRecordingRequest request;
	if (action == QStringLiteral("get_recording_capabilities"))
	{
		if (!parameters.isEmpty()) return invalid(QStringLiteral("unknown_parameter"), QStringLiteral("capabilities accepts no parameters"));
		request.operation = AiBridgeRecordingOperation::GetCapabilities;
	}
	else if (action == QStringLiteral("get_recording_status"))
	{
		if (!parameters.isEmpty()) return invalid(QStringLiteral("unknown_parameter"), QStringLiteral("status accepts no parameters"));
		request.operation = AiBridgeRecordingOperation::GetStatus;
	}
	else if (action == QStringLiteral("prepare_recording_take"))
	{
		if (!onlyKeys(parameters, {QStringLiteral("start_tick"), QStringLiteral("take_label"), QStringLiteral("count_in_bars"),
			QStringLiteral("metronome_enabled"), QStringLiteral("expected_project_generation")}))
			return invalid(QStringLiteral("unknown_parameter"), QStringLiteral("prepare contains an unknown parameter"));
		qint64 startTick = 0;
		qint64 countInBars = 0;
		quint64 generation = 0;
		if (!parameters.contains(QStringLiteral("start_tick"))
			|| !integer(parameters.value(QStringLiteral("start_tick")), 0, std::numeric_limits<int>::max(), startTick)
			|| !parameters.value(QStringLiteral("take_label")).isString()
			|| parameters.value(QStringLiteral("take_label")).toString().trimmed().isEmpty()
			|| parameters.value(QStringLiteral("take_label")).toString().size() > 128
			|| !integer(parameters.value(QStringLiteral("count_in_bars")), 0, 2, countInBars)
			|| !parameters.value(QStringLiteral("metronome_enabled")).isBool()
			|| !canonicalGeneration(parameters.value(QStringLiteral("expected_project_generation")), generation))
			return invalid(QStringLiteral("invalid_parameter"), QStringLiteral("prepare fields or bounds are invalid"));
		request.operation = AiBridgeRecordingOperation::Prepare;
		request.prepare.startTick = startTick;
		request.prepare.takeLabel = parameters.value(QStringLiteral("take_label")).toString();
		request.prepare.countInBars = int(countInBars);
		request.prepare.metronomeEnabled = parameters.value(QStringLiteral("metronome_enabled")).toBool();
		request.prepare.expectedProjectGeneration = generation;
	}
	else if (action == QStringLiteral("start_recording_take"))
	{
		if (!onlyKeys(parameters, {QStringLiteral("prepared_take_id"), QStringLiteral("expected_project_generation"),
			QStringLiteral("start_request_id")}))
			return invalid(QStringLiteral("unknown_parameter"), QStringLiteral("start contains an unknown parameter"));
		quint64 generation = 0;
		if (!canonicalUuid(parameters.value(QStringLiteral("prepared_take_id")), request.start.preparedTakeId)
			|| !canonicalGeneration(parameters.value(QStringLiteral("expected_project_generation")), generation)
			|| !canonicalUuid(parameters.value(QStringLiteral("start_request_id")), request.start.startRequestId))
			return invalid(QStringLiteral("invalid_parameter"), QStringLiteral("start fields are invalid"));
		request.operation = AiBridgeRecordingOperation::Start;
		request.start.expectedProjectGeneration = generation;
	}
	else if (action == QStringLiteral("stop_recording_take"))
	{
		if (!onlyKeys(parameters, {QStringLiteral("take_id")}))
			return invalid(QStringLiteral("unknown_parameter"), QStringLiteral("stop contains an unknown parameter"));
		if (!canonicalUuid(parameters.value(QStringLiteral("take_id")), request.takeId))
			return invalid(QStringLiteral("invalid_parameter"), QStringLiteral("take_id must be a canonical UUID"));
		request.operation = AiBridgeRecordingOperation::Stop;
	}
	else if (action == QStringLiteral("list_recorded_takes"))
	{
		if (!onlyKeys(parameters, {QStringLiteral("offset"), QStringLiteral("limit")}))
			return invalid(QStringLiteral("unknown_parameter"), QStringLiteral("take listing contains an unknown parameter"));
		qint64 offset = 0;
		qint64 limit = 25;
		if ((parameters.contains(QStringLiteral("offset"))
				&& !integer(parameters.value(QStringLiteral("offset")), 0, std::numeric_limits<int>::max(), offset))
			|| (parameters.contains(QStringLiteral("limit"))
				&& !integer(parameters.value(QStringLiteral("limit")), 1, 100, limit)))
			return invalid(QStringLiteral("invalid_parameter"), QStringLiteral("take listing page is invalid"));
		request.operation = AiBridgeRecordingOperation::List;
		request.page = {offset, int(limit)};
	}
	else return invalid(QStringLiteral("unknown_action"), QStringLiteral("Action is not a recording operation"));
	return {true, QStringLiteral("ok"), QStringLiteral("Recording request accepted"), std::move(request)};
}

QJsonObject AiBridgeRecordingTypes::capabilitiesJson(const recording::RecordingCapabilities& value)
{
	QJsonArray rates;
	for (const auto rate : value.sampleRates) rates.append(int(rate));
	return {{QStringLiteral("sample_rates"), rates},
		{QStringLiteral("channels"), int(value.channels)},
		{QStringLiteral("maximum_take_seconds"), int(value.maximumTakeSeconds)},
		{QStringLiteral("requires_local_arm"), value.requiresLocalArm},
		{QStringLiteral("metronome_supported"), value.metronomeSupported},
		{QStringLiteral("placement_implemented"), value.placementImplemented},
		{QStringLiteral("placement_mode"), QStringLiteral("manual_uncalibrated")},
		{QStringLiteral("drift_quality"), QStringLiteral("unmeasured")},
		{QStringLiteral("sample_accuracy_verified"), value.sampleAccuracyVerified},
		{QStringLiteral("microphone_monitoring"), false}};
}

QJsonObject AiBridgeRecordingTypes::statusJson(const recording::RecordingStatus& value)
{
	return {{QStringLiteral("phase"), phaseName(value.phase)},
		{QStringLiteral("armed"), value.armed},
		{QStringLiteral("prepared_take_id"), value.preparedId},
		{QStringLiteral("take_id"), value.takeId},
		{QStringLiteral("project_generation"), QString::number(value.projectGeneration)},
		{QStringLiteral("take_label"), value.takeLabel},
		{QStringLiteral("code"), value.code},
		{QStringLiteral("capture_code"), value.captureCode},
		{QStringLiteral("capture_integrity"), integrityName(value.captureIntegrity)},
		{QStringLiteral("placement_state"), placementName(value.placementState)},
		{QStringLiteral("placement_confirmed"), value.placementConfirmed},
		{QStringLiteral("placement_quality"), value.placementQuality},
		{QStringLiteral("drift_quality"), value.driftQuality},
		{QStringLiteral("sample_accuracy_verified"), value.sampleAccuracyVerified},
		{QStringLiteral("performance_reached"), value.performanceReached},
		{QStringLiteral("cancelled"), value.cancelled},
		{QStringLiteral("intended_target_tick"), qint64(value.intendedTargetTick)},
		{QStringLiteral("preroll_origin_tick"), qint64(value.prerollOriginTick)},
		{QStringLiteral("native_tick"), qint64(value.nativeTick)},
		{QStringLiteral("count_in_ticks_remaining"), qint64(value.countInTicksRemaining)},
		{QStringLiteral("received_frames"), QString::number(value.receivedFrames)},
		{QStringLiteral("accepted_frames"), QString::number(value.acceptedFrames)},
		{QStringLiteral("written_frames"), QString::number(value.writtenFrames)},
		{QStringLiteral("durable_frames"), QString::number(value.durableFrames)},
		{QStringLiteral("application_dropped_frames"), QString::number(value.applicationDroppedFrames)},
		{QStringLiteral("over_limit_frames"), QString::number(value.intentionallyUnrecordedFrames)},
		{QStringLiteral("clipped_samples"), QString::number(value.clippedSamples)},
		{QStringLiteral("invalid_samples"), QString::number(value.invalidSamples)},
		{QStringLiteral("observations_dropped"), QString::number(value.observationsDropped)},
		{QStringLiteral("hardware_loss_count_known"), value.hardwareLossCountKnown},
		{QStringLiteral("hardware_lost_frames"), value.hardwareLossCountKnown
			? QJsonValue(QString::number(value.hardwareLostFrames)) : QJsonValue(QJsonValue::Null)},
		{QStringLiteral("input_peak"), value.inputPeak},
		{QStringLiteral("input_timestamp_quality"), timestampQualityName(value.inputTimestampQuality)},
		{QStringLiteral("output_continuity"), value.outputContinuity}};
}

QJsonObject AiBridgeRecordingTypes::takePageJson(const recording::TakePage& value, recording::PageRequest request)
{
	QJsonArray takes;
	for (const auto& take : value.takes) takes.append(statusJson(take));
	return {{QStringLiteral("takes"), takes},
		{QStringLiteral("offset"), qint64(request.offset)},
		{QStringLiteral("limit"), request.limit},
		{QStringLiteral("next_offset"), qint64(value.nextOffset)},
		{QStringLiteral("has_more"), value.hasMore}};
}
}
