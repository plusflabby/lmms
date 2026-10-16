#ifndef LMMS_AIBRIDGE_AUDIO_WIRE_H
#define LMMS_AIBRIDGE_AUDIO_WIRE_H
#include "AiBridgeProtocol.h"
namespace lmms {
class AiBridgeAudioWire
{
public:
	static bool handles(const QString& action);
	static AiBridgeProtocolResponse execute(const AiBridgeProtocolRequest& request);
};
}
#endif
