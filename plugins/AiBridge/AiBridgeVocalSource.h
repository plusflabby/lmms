#ifndef LMMS_AI_BRIDGE_VOCAL_SOURCE_H
#define LMMS_AI_BRIDGE_VOCAL_SOURCE_H
#include "AiBridgeVocalTypes.h"
namespace lmms {
VocalScanResult scanVocalSource(const VocalSourceTicket&,const PcmWaveChunkVisitor&,const std::atomic_bool&);
}
#endif
