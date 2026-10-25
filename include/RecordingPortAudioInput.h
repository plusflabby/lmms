#ifndef LMMS_RECORDING_PORTAUDIO_INPUT_H
#define LMMS_RECORDING_PORTAUDIO_INPUT_H
#include "RecordingInputController.h"
namespace lmms::recording {
class RecordingPortAudioInputDriver final : public RecordingInputDriver
{
public:
 RecordingPortAudioInputDriver();
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
