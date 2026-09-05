#ifndef LMMS_AIBRIDGE_EFFECT_CATALOG_H
#define LMMS_AIBRIDGE_EFFECT_CATALOG_H
#include "AiBridgeEffectTypes.h"
namespace lmms {
class AiBridgeEffectCatalog {
public: static bool isAllowed(const QString& key); static AiBridgeEffectPage list(int offset, int limit, const QString& filter = {});
};
}
#endif
