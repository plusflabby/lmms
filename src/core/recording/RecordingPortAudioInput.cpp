#include "RecordingPortAudioInput.h"
#include "lmmsconfig.h"
#include <algorithm>
#include <limits>
#include <vector>
#include <QStringList>
#if defined(Q_OS_WIN) && defined(LMMS_HAVE_PORTAUDIO)
#include <portaudio.h>
#include <pa_win_wasapi.h>
#include <QHash>
#endif
namespace lmms::recording {
#if defined(Q_OS_WIN) && defined(LMMS_HAVE_PORTAUDIO)
namespace {
QString errorCode(PaError error){return error==paNoError?QStringLiteral("ok"):QStringLiteral("portaudio_error_%1").arg(int(error));}
QString deviceId(PaDeviceIndex index,const PaDeviceInfo* device)
{return QString::number(index)+QChar('|')+QString::fromUtf8(device->name);}
}
struct RecordingPortAudioInputDriver::Impl
{
 PaStream* stream=nullptr;
 bool initialized=false;
 PaDeviceIndex selected=paNoDevice;
 int channels=0;
 RecordingInputCallback callback=nullptr;
 void* context=nullptr;
 static int streamCallback(const void* input,void*,unsigned long frameCount,const PaStreamCallbackTimeInfo* times,
  PaStreamCallbackFlags flags,void* userData) noexcept
 {
  auto* self=static_cast<Impl*>(userData);
  if(!self || !self->callback)return paComplete;
  const auto frames=frameCount>std::numeric_limits<std::uint32_t>::max()?std::numeric_limits<std::uint32_t>::max():std::uint32_t(frameCount);
  const auto hostFlags=(flags&paInputOverflow)?InputHostOverflow:0u;
  const InputBlock block{static_cast<const float*>(input),frames,std::uint32_t(self->channels),hostFlags,
   times?times->inputBufferAdcTime:0.0,times?times->currentTime:0.0};
  return self->callback(self->context,block)==InputDecision::Continue?paContinue:paComplete;
 }
};
RecordingPortAudioInputDriver::RecordingPortAudioInputDriver():m_impl(std::make_unique<Impl>()){}
RecordingPortAudioInputDriver::~RecordingPortAudioInputDriver()=default;
RecordingDriverResult RecordingPortAudioInputDriver::initialize() noexcept
{
 if(m_impl->initialized)return {"ok",true};
 const auto error=Pa_Initialize();if(error!=paNoError)return {errorCode(error),true};
 m_impl->initialized=true;return {"ok",true};
}
RecordingInputDevicePage RecordingPortAudioInputDriver::enumerateInputs(std::uint32_t offset,std::uint32_t limit)
{
 if(!m_impl->initialized)return {"not_initialized",{},offset,false};
 if(!limit || limit>64)return {"invalid_page",{},offset,false};
 const auto count=Pa_GetDeviceCount();if(count<0)return {errorCode(PaError(count)),{},offset,false};
 std::vector<RecordingInputDevice> eligible;QHash<QString,std::uint32_t> nameCounts;
 for(PaDeviceIndex index=0;index<count;++index)
 {
  const auto* device=Pa_GetDeviceInfo(index);if(!device || device->maxInputChannels<=0)continue;
  const auto* host=Pa_GetHostApiInfo(device->hostApi);if(!host || host->type!=paWASAPI)continue;
  const auto name=QString::fromUtf8(device->name);++nameCounts[name];
  eligible.push_back({deviceId(index,device),name,QStringLiteral("WASAPI"),std::uint32_t(device->maxInputChannels)});
 }
 eligible.erase(std::remove_if(eligible.begin(),eligible.end(),[&](const auto& item){return nameCounts.value(item.name)!=1;}),eligible.end());
 if(offset>eligible.size())return {"invalid_page",{},offset,false};
 const auto end=std::min<std::size_t>(eligible.size(),std::size_t(offset)+limit);
 RecordingInputDevicePage result;result.code="ok";result.nextOffset=std::uint32_t(end);result.hasMore=end<eligible.size();
 result.devices.insert(result.devices.end(),eligible.begin()+offset,eligible.begin()+end);return result;
}
RecordingDriverResult RecordingPortAudioInputDriver::openInputOnly(const SelectedInputProfile& profile,RecordingInputCallback callback,void* context) noexcept
{
 if(!m_impl->initialized)return {"not_initialized",true};
 if(m_impl->stream)return {"stream_already_open",false};
 if(!callback || !profile.wasapiShared || profile.hostApiName!=QStringLiteral("WASAPI")
  || (profile.input.sampleRate!=44100 && profile.input.sampleRate!=48000)
  || profile.input.inputChannels==0 || profile.input.inputChannels>32 || profile.input.maxCallback==0
  || profile.input.maxCallback>RecordingRing::MaximumCallback || profile.input.selectedChannel>=profile.input.inputChannels
  || !profile.input.maxTakeFrames || profile.input.maxTakeFrames>std::uint64_t(profile.input.sampleRate)*120)
  return {"invalid_input_profile",true};
 const auto separator=profile.deviceId.indexOf(QChar('|'));bool indexOk=false;
 const auto index=profile.deviceId.left(separator).toInt(&indexOk);
 if(separator<=0 || !indexOk)return {"selected_device_stale",true};
 const auto count=Pa_GetDeviceCount();if(count<0 || index<0 || index>=count)return {"selected_device_stale",true};
 const auto* device=Pa_GetDeviceInfo(index);if(!device || device->maxInputChannels<int(profile.input.inputChannels)
  || deviceId(index,device)!=profile.deviceId)return {"selected_device_stale",true};
 const auto* host=Pa_GetHostApiInfo(device->hostApi);if(!host || host->type!=paWASAPI)
  return {"selected_device_stale",true};
 PaWasapiStreamInfo wasapi{};wasapi.size=sizeof(wasapi);wasapi.hostApiType=paWASAPI;wasapi.version=1;
 wasapi.flags=0;wasapi.channelMask=0;wasapi.hostProcessorOutput=nullptr;wasapi.hostProcessorInput=nullptr;
 wasapi.threadPriority=eThreadPriorityCapture;wasapi.streamCategory=eAudioCategoryOther;wasapi.streamOption=eStreamOptionNone;
 PaStreamParameters inputParameters{};inputParameters.device=index;inputParameters.channelCount=int(profile.input.inputChannels);
 inputParameters.sampleFormat=paFloat32;inputParameters.suggestedLatency=device->defaultLowInputLatency;inputParameters.hostApiSpecificStreamInfo=&wasapi;
 const auto supported=Pa_IsFormatSupported(&inputParameters,nullptr,double(profile.input.sampleRate));
 if(supported!=paFormatIsSupported)return {errorCode(supported),true};
 m_impl->callback=callback;m_impl->context=context;m_impl->channels=int(profile.input.inputChannels);m_impl->selected=index;
 const auto opened=Pa_OpenStream(&m_impl->stream,&inputParameters,nullptr,double(profile.input.sampleRate),
  profile.input.maxCallback,paNoFlag,&Impl::streamCallback,m_impl.get());
 if(opened!=paNoError){m_impl->stream=nullptr;m_impl->callback=nullptr;m_impl->context=nullptr;return {errorCode(opened),true};}
 return {"ok",true};
}
RecordingDriverResult RecordingPortAudioInputDriver::start() noexcept
{
 if(!m_impl->stream)return {"stream_not_open",true};const auto error=Pa_StartStream(m_impl->stream);
 if(error==paNoError)return {"ok",false};const auto stopped=Pa_IsStreamStopped(m_impl->stream);return {errorCode(error),stopped==1};
}
RecordingDriverResult RecordingPortAudioInputDriver::stop() noexcept
{
 if(!m_impl->stream)return {"stream_not_open",false};
 const auto error=Pa_StopStream(m_impl->stream);const auto stopped=Pa_IsStreamStopped(m_impl->stream);
 if(error!=paNoError && !(error==paStreamIsStopped && stopped==1))return {errorCode(error),false};
 return {stopped==1?QStringLiteral("ok"):QStringLiteral("callback_not_quiescent"),stopped==1};
}
RecordingDriverResult RecordingPortAudioInputDriver::close() noexcept
{
 if(!m_impl->stream)return {"ok",true};
 const auto stopped=Pa_IsStreamStopped(m_impl->stream);if(stopped!=1)return {"callback_not_quiescent",false};
 const auto error=Pa_CloseStream(m_impl->stream);if(error!=paNoError)return {errorCode(error),false};
 m_impl->stream=nullptr;m_impl->callback=nullptr;m_impl->context=nullptr;m_impl->selected=paNoDevice;m_impl->channels=0;return {"ok",true};
}
RecordingDriverResult RecordingPortAudioInputDriver::terminate() noexcept
{
 if(!m_impl->initialized)return {"ok",true};if(m_impl->stream)return {"stream_still_open",false};
 const auto error=Pa_Terminate();if(error!=paNoError)return {errorCode(error),true};m_impl->initialized=false;return {"ok",true};
}
#else
struct RecordingPortAudioInputDriver::Impl {};
RecordingPortAudioInputDriver::RecordingPortAudioInputDriver():m_impl(std::make_unique<Impl>()){}
RecordingPortAudioInputDriver::~RecordingPortAudioInputDriver()=default;
RecordingDriverResult RecordingPortAudioInputDriver::initialize() noexcept{return {"unsupported_backend",true};}
RecordingInputDevicePage RecordingPortAudioInputDriver::enumerateInputs(std::uint32_t offset,std::uint32_t){return {"unsupported_backend",{},offset,false};}
RecordingDriverResult RecordingPortAudioInputDriver::openInputOnly(const SelectedInputProfile&,RecordingInputCallback,void*) noexcept{return {"unsupported_backend",true};}
RecordingDriverResult RecordingPortAudioInputDriver::start() noexcept{return {"unsupported_backend",true};}
RecordingDriverResult RecordingPortAudioInputDriver::stop() noexcept{return {"unsupported_backend",true};}
RecordingDriverResult RecordingPortAudioInputDriver::close() noexcept{return {"unsupported_backend",true};}
RecordingDriverResult RecordingPortAudioInputDriver::terminate() noexcept{return {"unsupported_backend",true};}
#endif
std::unique_ptr<RecordingInputDriver> createPlatformRecordingInputDriver(){return std::make_unique<RecordingPortAudioInputDriver>();}
}
