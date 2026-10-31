#include "RecordingAudioBackendPolicy.h"
namespace lmms::recording {
bool RecordingAudioBackendPolicy::qualificationBuild() noexcept
{
#ifdef LMMS_ENABLE_RECORDING_QUALIFICATION
 return true;
#else
 return false;
#endif
}
bool RecordingAudioBackendPolicy::independentInputOutputAllowed(const QString& name,bool capture,bool qualification) noexcept
{
 if(capture)return false;
 if(name=="Dummy (no sound output)")return true;
 // These existing qualification paths explicitly suppress legacy input at construction.
 // PortAudio's historical supportsCapture flag alone does not report its duplex input.
 return qualification && (name=="SDL (Simple DirectMedia Layer)" || name=="PortAudio");
}
bool RecordingAudioBackendPolicy::legacyInputAllowed(bool qualificationBuild) noexcept{return !qualificationBuild;}
}
