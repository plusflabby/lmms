#ifndef LMMS_VOCAL_RUNTIME_QUALIFICATION_H
#define LMMS_VOCAL_RUNTIME_QUALIFICATION_H

#include "lmms_export.h"

namespace lmms {
// Call only on the application thread. No dependency I/O reaches audio workers.
LMMS_EXPORT bool qualifiedVocalSamplerateRuntime();
}
#endif
