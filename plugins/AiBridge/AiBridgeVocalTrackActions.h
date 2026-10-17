#ifndef LMMS_AI_BRIDGE_VOCAL_TRACK_ACTIONS_H
#define LMMS_AI_BRIDGE_VOCAL_TRACK_ACTIONS_H
#include "AiBridgeVocalTypes.h"
namespace lmms {
VocalTrackResult captureVocalTrack(const QString&);
VocalTrackActionResult setAudioTrackProperties(const VocalTrackRequest&);
}
#endif
