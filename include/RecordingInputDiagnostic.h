#ifndef LMMS_RECORDING_INPUT_DIAGNOSTIC_H
#define LMMS_RECORDING_INPUT_DIAGNOSTIC_H
#include <QString>
#include <array>
#include <cstdint>
namespace lmms::recording {
enum class InputDiagnosticStage : std::uint32_t
{Initialize,ResolveEndpoint,ValidateChannels,ValidateFormat,FormatSupport,OpenStream,StartStream,FirstCallback,
 StopStream,CloseStream,Terminate,Count,None};
enum class InputDiagnosticOutcome : std::uint32_t {NotAttempted,Succeeded,Failed};
const char* recordingInputStageName(InputDiagnosticStage) noexcept;
struct RecordingInputDiagnostic
{
 InputDiagnosticStage failedStage=InputDiagnosticStage::None;
 std::array<InputDiagnosticOutcome,std::size_t(InputDiagnosticStage::Count)> outcomes{};
 bool hasPortAudioError=false,hasHostError=false,wasapiShared=false;
 int portAudioError=0,hostApiType=0;
 long hostErrorCode=0;
 QString description,hostApiName,deviceDescription,hostErrorDescription;
 std::uint32_t requestedRate=0,inputChannels=0,selectedChannel=0,maxInputChannels=0,maxCallbackFrames=0;
 double defaultSampleRate=0,defaultLowInputLatency=0;
 std::uint64_t receivedFrames=0,acceptedFrames=0;
 InputDiagnosticOutcome outcome(InputDiagnosticStage stage) const noexcept
 {return stage<InputDiagnosticStage::Count?outcomes[std::size_t(stage)]:InputDiagnosticOutcome::NotAttempted;}
 QString localSummary() const;
};
}
#endif
