#ifndef LMMS_AIBRIDGE_RECORDING_TYPES_H
#define LMMS_AIBRIDGE_RECORDING_TYPES_H

#include "RecordingTypes.h"
#include <QJsonObject>
#include <QString>

namespace lmms
{
enum class AiBridgeRecordingOperation
{
	GetCapabilities,
	GetStatus,
	Prepare,
	Start,
	Stop,
	List
};

struct AiBridgeRecordingRequest
{
	AiBridgeRecordingOperation operation = AiBridgeRecordingOperation::GetCapabilities;
	recording::PrepareTakeRequest prepare;
	recording::StartTakeRequest start;
	QString takeId;
	recording::PageRequest page;
};

struct AiBridgeRecordingParseResult
{
	bool success = false;
	QString code;
	QString message;
	AiBridgeRecordingRequest request;
};

class AiBridgeRecordingTypes
{
public:
	static bool handles(const QString& action);
	static AiBridgeRecordingParseResult parse(const QString& action, const QJsonObject& parameters);
	static QJsonObject capabilitiesJson(const recording::RecordingCapabilities&);
	static QJsonObject statusJson(const recording::RecordingStatus&);
	static QJsonObject takePageJson(const recording::TakePage&, recording::PageRequest);
};
}

#endif
