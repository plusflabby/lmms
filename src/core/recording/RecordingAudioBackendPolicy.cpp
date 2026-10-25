#include "RecordingAudioBackendPolicy.h"
namespace lmms::recording {
bool RecordingAudioBackendPolicy::legacyInputAllowed(bool qualificationBuild) noexcept{return !qualificationBuild;}
}
