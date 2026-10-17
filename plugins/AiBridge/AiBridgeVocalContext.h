#ifndef LMMS_AI_BRIDGE_VOCAL_CONTEXT_H
#define LMMS_AI_BRIDGE_VOCAL_CONTEXT_H
#include "AiBridgeVocalTypes.h"
namespace lmms {
VocalSnapshotResult captureVocalInputs(const VocalAnalyzeRequest&);
QString validateVocalSnapshot(const VocalInputSnapshot&);
QString vocalClipStateHash(const QString&);
}
#endif
