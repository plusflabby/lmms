#ifndef LMMS_AI_BRIDGE_VOCAL_WIRE_H
#define LMMS_AI_BRIDGE_VOCAL_WIRE_H
#include "AiBridgeProtocol.h"
#include "AiBridgeVocalJobs.h"
#include <QPointer>
namespace lmms {
class AiBridgeVocalWire
{
public:
	static bool handles(const QString&);
	static bool validRequest(const QString&,const QJsonObject&);
	static AiBridgeProtocolResponse execute(const AiBridgeProtocolRequest&,QPointer<AiBridgeVocalJobs>);
};
}
#endif
