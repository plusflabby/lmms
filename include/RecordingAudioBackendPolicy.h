#ifndef LMMS_RECORDING_AUDIO_BACKEND_POLICY_H
#define LMMS_RECORDING_AUDIO_BACKEND_POLICY_H
#include "lmms_export.h"
#include <QString>
namespace lmms::recording {
class LMMS_EXPORT RecordingAudioBackendPolicy
{
public:
 static bool independentInputOutputAllowed(const QString& actualBackend,bool supportsCapture,bool qualificationBuild) noexcept;
 static bool legacyInputAllowed(bool qualificationBuild) noexcept;
 static bool qualificationBuild() noexcept;
};
}
#endif
