#ifndef LMMS_AIBRIDGE_PROTOCOL_H
#define LMMS_AIBRIDGE_PROTOCOL_H

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace lmms
{

struct AiBridgeProtocolRequest
{
	int version = 0;
	QString requestId;
	QString action;
	QJsonObject parameters;
};

struct AiBridgeProtocolParseResult
{
	bool success = false;
	QString code;
	QString message;
	AiBridgeProtocolRequest request;
};

struct AiBridgeProtocolResponse
{
	int version = 2;
	QString requestId;
	bool success = false;
	QString code;
	QString message;
	QJsonValue data = QJsonValue(QJsonValue::Undefined);
};

class AiBridgeProtocol
{
public:
	static constexpr int MaxFrameBytes = 64 * 1024;
	static constexpr int MaxRequestIdBytes = 128;

	static AiBridgeProtocolParseResult parseRequest(const QByteArray &frame);
	static QByteArray serializeResponse(const AiBridgeProtocolResponse &response);
};

} // namespace lmms

#endif // LMMS_AIBRIDGE_PROTOCOL_H
