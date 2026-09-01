#include "AiBridgeProtocol.h"

#include <QJsonDocument>
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

bool hasOnlyKeys(const QJsonObject &object, const QSet<QString> &allowed)
{
	for (auto it = object.constBegin(); it != object.constEnd(); ++it)
	{
		if (!allowed.contains(it.key())) return false;
	}
	return true;
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
	if (!QSet<QString>{QStringLiteral("get_context"), QStringLiteral("set_tempo"), QStringLiteral("adjust_current_track_volume"), QStringLiteral("add_note_to_current_clip")}.contains(action))
		return failure(QStringLiteral("unknown_action"), QStringLiteral("Action is not allowlisted"));
	if (!object.contains(QStringLiteral("parameters")) || !object.value(QStringLiteral("parameters")).isObject())
		return failure(QStringLiteral("invalid_parameters"), QStringLiteral("parameters must be an object"));

	const auto parameters = object.value(QStringLiteral("parameters")).toObject();
	if (action == QStringLiteral("get_context") && !parameters.isEmpty())
		return failure(QStringLiteral("unknown_parameter"), QStringLiteral("get_context accepts no parameters"));
	if (action == QStringLiteral("set_tempo") && (!hasOnlyKeys(parameters, {QStringLiteral("bpm")}) || !isInteger(parameters.value(QStringLiteral("bpm")))))
		return failure(QStringLiteral("invalid_parameter"), QStringLiteral("set_tempo requires integer bpm"));
	if (action == QStringLiteral("adjust_current_track_volume") && (!hasOnlyKeys(parameters, {QStringLiteral("delta")}) || !parameters.value(QStringLiteral("delta")).isDouble()))
		return failure(QStringLiteral("invalid_parameter"), QStringLiteral("adjust_current_track_volume requires numeric delta"));
	if (action == QStringLiteral("add_note_to_current_clip") && (!hasOnlyKeys(parameters, {QStringLiteral("key"), QStringLiteral("position"), QStringLiteral("length"), QStringLiteral("velocity")}) || !isInteger(parameters.value(QStringLiteral("key"))) || !isInteger(parameters.value(QStringLiteral("position"))) || !isInteger(parameters.value(QStringLiteral("length"))) || !isInteger(parameters.value(QStringLiteral("velocity")))))
		return failure(QStringLiteral("invalid_parameter"), QStringLiteral("add_note_to_current_clip requires integer note parameters"));

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
	return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

} // namespace lmms
