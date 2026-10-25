#ifndef LMMS_RECORDING_PORTAUDIO_INPUT_H
#define LMMS_RECORDING_PORTAUDIO_INPUT_H
#include "RecordingInputController.h"
#include "lmmsconfig.h"
#if defined(Q_OS_WIN) && defined(LMMS_HAVE_PORTAUDIO)
#include <portaudio.h>
#endif
namespace lmms::recording {
#if defined(Q_OS_WIN) && defined(LMMS_HAVE_PORTAUDIO)
// A copied API table lets synthetic tests exercise the actual driver without initializing audio.
struct RecordingPortAudioApi
{
 decltype(&Pa_Initialize) initialize=&Pa_Initialize;
 decltype(&Pa_Terminate) terminate=&Pa_Terminate;
 decltype(&Pa_GetDeviceCount) deviceCount=&Pa_GetDeviceCount;
 decltype(&Pa_GetDeviceInfo) deviceInfo=&Pa_GetDeviceInfo;
 decltype(&Pa_GetHostApiInfo) hostApiInfo=&Pa_GetHostApiInfo;
 decltype(&Pa_GetErrorText) errorText=&Pa_GetErrorText;
 decltype(&Pa_GetLastHostErrorInfo) lastHostError=&Pa_GetLastHostErrorInfo;
 decltype(&Pa_IsFormatSupported) formatSupported=&Pa_IsFormatSupported;
 decltype(&Pa_OpenStream) open=&Pa_OpenStream;
 decltype(&Pa_StartStream) start=&Pa_StartStream;
 decltype(&Pa_IsStreamStopped) isStopped=&Pa_IsStreamStopped;
 decltype(&Pa_StopStream) stop=&Pa_StopStream;
 decltype(&Pa_CloseStream) close=&Pa_CloseStream;
};
#endif
class RecordingPortAudioInputDriver final : public RecordingInputDriver
{
public:
 RecordingPortAudioInputDriver();
#if defined(Q_OS_WIN) && defined(LMMS_HAVE_PORTAUDIO)
 explicit RecordingPortAudioInputDriver(const RecordingPortAudioApi&);
#endif
 ~RecordingPortAudioInputDriver() override;
 RecordingDriverResult initialize() noexcept override;
 RecordingInputDevicePage enumerateInputs(std::uint32_t offset,std::uint32_t limit) override;
 RecordingDriverResult openInputOnly(const SelectedInputProfile&,RecordingInputCallback,void*) noexcept override;
 RecordingDriverResult start() noexcept override;
 RecordingDriverResult stop() noexcept override;
 RecordingDriverResult close() noexcept override;
 RecordingDriverResult terminate() noexcept override;
private:
 struct Impl;
 std::unique_ptr<Impl> m_impl;
};
std::unique_ptr<RecordingInputDriver> createPlatformRecordingInputDriver();
}
#endif
