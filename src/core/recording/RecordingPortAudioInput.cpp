#include "RecordingPortAudioInput.h"
#include "lmmsconfig.h"
#include <algorithm>
#include <cmath>
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
QString boundedText(const char* value,int limit)
{
 if(!value)return {};
 int length=0;while(length<limit*4 && value[length])++length;
 auto text=QString::fromUtf8(value,length).left(limit);
 for(auto& character:text)if(character.category()==QChar::Other_Control)character=QChar(' ');
 return text;
}
QString deviceId(PaDeviceIndex index,const PaDeviceInfo* device)
{return QString::number(index)+QChar('|')+QString::fromUtf8(device->name);}
}
struct RecordingPortAudioInputDriver::Impl
{
 RecordingPortAudioApi api;
 RecordingInputDiagnostic diagnostic;
 void succeeded(InputDiagnosticStage stage)
 {diagnostic.outcomes[std::size_t(stage)]=InputDiagnosticOutcome::Succeeded;}
 RecordingDriverResult localFailure(InputDiagnosticStage stage,const QString& code,bool quiescent=true)
 {
  diagnostic.outcomes[std::size_t(stage)]=InputDiagnosticOutcome::Failed;
  if(diagnostic.failedStage==InputDiagnosticStage::None){diagnostic.failedStage=stage;diagnostic.description=code.left(160);}
  return {code,quiescent,diagnostic};
 }
 RecordingDriverResult result(InputDiagnosticStage stage,PaError error,bool quiescent=true)
 {
  if(error==paNoError){succeeded(stage);return {"ok",quiescent,diagnostic};}
  diagnostic.outcomes[std::size_t(stage)]=InputDiagnosticOutcome::Failed;
  if(diagnostic.failedStage==InputDiagnosticStage::None)
  {
   diagnostic.failedStage=stage;diagnostic.hasPortAudioError=true;diagnostic.portAudioError=error;
   // Only paUnanticipatedHostError makes the last-host-error record legitimate.
   // Copy it before any further PortAudio query, including the error-description query.
   if(error==paUnanticipatedHostError)
   {
    if(const auto* host=api.lastHostError())
    {diagnostic.hasHostError=true;diagnostic.hostApiType=int(host->hostApiType);diagnostic.hostErrorCode=host->errorCode;
     diagnostic.hostErrorDescription=boundedText(host->errorText,160);}
   }
   diagnostic.description=boundedText(api.errorText(error),160);
  }
  return {QStringLiteral("portaudio_%1_error_%2").arg(QString::fromLatin1(recordingInputStageName(stage))).arg(int(error)),quiescent,diagnostic};
 }
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
RecordingPortAudioInputDriver::RecordingPortAudioInputDriver(const RecordingPortAudioApi& api):m_impl(std::make_unique<Impl>())
{m_impl->api=api;}
RecordingPortAudioInputDriver::~RecordingPortAudioInputDriver()=default;
RecordingDriverResult RecordingPortAudioInputDriver::initialize() noexcept
{
 if(m_impl->initialized)return {"ok",true,m_impl->diagnostic};
 m_impl->diagnostic={};const auto error=m_impl->api.initialize();
 const auto result=m_impl->result(InputDiagnosticStage::Initialize,error);
 if(error==paNoError)m_impl->initialized=true;
 return result;
}
RecordingInputDevicePage RecordingPortAudioInputDriver::enumerateInputs(std::uint32_t offset,std::uint32_t limit)
{
 if(!m_impl->initialized)return {"not_initialized",{},offset,false};
 if(!limit || limit>64)return {"invalid_page",{},offset,false};
 const auto count=m_impl->api.deviceCount();
 if(count<0){const auto r=m_impl->result(InputDiagnosticStage::ResolveEndpoint,PaError(count));return {r.code,{},offset,false,r.diagnostic};}
 std::vector<RecordingInputDevice> eligible;QHash<QString,std::uint32_t> nameCounts;
 for(PaDeviceIndex index=0;index<count;++index)
 {
  const auto* device=m_impl->api.deviceInfo(index);if(!device || device->maxInputChannels<=0)continue;
  const auto* host=m_impl->api.hostApiInfo(device->hostApi);if(!host || host->type!=paWASAPI)continue;
  const auto name=QString::fromUtf8(device->name);++nameCounts[name];
  eligible.push_back({deviceId(index,device),name,QStringLiteral("WASAPI"),std::uint32_t(device->maxInputChannels)});
 }
 eligible.erase(std::remove_if(eligible.begin(),eligible.end(),[&](const auto& item){return nameCounts.value(item.name)!=1;}),eligible.end());
 if(offset>eligible.size())return {"invalid_page",{},offset,false};
 const auto end=std::min<std::size_t>(eligible.size(),std::size_t(offset)+limit);
 RecordingInputDevicePage result;result.code="ok";result.nextOffset=std::uint32_t(end);result.hasMore=end<eligible.size();result.diagnostic=m_impl->diagnostic;
 result.devices.insert(result.devices.end(),eligible.begin()+offset,eligible.begin()+end);return result;
}
RecordingDriverResult RecordingPortAudioInputDriver::openInputOnly(const SelectedInputProfile& profile,RecordingInputCallback callback,void* context) noexcept
{
 if(m_impl->stream)return m_impl->localFailure(InputDiagnosticStage::OpenStream,"stream_already_open",false);
 m_impl->diagnostic={};auto& diagnostic=m_impl->diagnostic;
 diagnostic.requestedRate=profile.input.sampleRate;diagnostic.inputChannels=profile.input.inputChannels;
 diagnostic.selectedChannel=profile.input.selectedChannel;diagnostic.wasapiShared=profile.wasapiShared;
 diagnostic.maxCallbackFrames=profile.input.maxCallback;
 diagnostic.hostApiName=profile.hostApiName.left(64);
 if(!m_impl->initialized)return m_impl->localFailure(InputDiagnosticStage::Initialize,"not_initialized");
 m_impl->succeeded(InputDiagnosticStage::Initialize);
 const auto separator=profile.deviceId.indexOf(QChar('|'));bool indexOk=false;
 const auto index=profile.deviceId.left(separator).toInt(&indexOk);
 if(separator<=0 || !indexOk)return m_impl->localFailure(InputDiagnosticStage::ResolveEndpoint,"selected_device_stale");
 const auto count=m_impl->api.deviceCount();
 if(count<0)return m_impl->result(InputDiagnosticStage::ResolveEndpoint,PaError(count));
 if(index<0 || index>=count)return m_impl->localFailure(InputDiagnosticStage::ResolveEndpoint,"selected_device_stale");
 const auto* device=m_impl->api.deviceInfo(index);
 if(!device || deviceId(index,device)!=profile.deviceId)return m_impl->localFailure(InputDiagnosticStage::ResolveEndpoint,"selected_device_stale");
 diagnostic.deviceDescription=boundedText(device->name,96);
 diagnostic.maxInputChannels=std::uint32_t(std::max(0,device->maxInputChannels));
 diagnostic.defaultSampleRate=std::isfinite(device->defaultSampleRate)?device->defaultSampleRate:0;
 diagnostic.defaultLowInputLatency=std::isfinite(device->defaultLowInputLatency)?device->defaultLowInputLatency:0;
 const auto* host=m_impl->api.hostApiInfo(device->hostApi);if(!host || host->type!=paWASAPI)
  return m_impl->localFailure(InputDiagnosticStage::ResolveEndpoint,"selected_device_host_mismatch");
 diagnostic.hostApiName=boundedText(host->name,64);
 if(profile.hostApiName!=QStringLiteral("WASAPI"))return m_impl->localFailure(InputDiagnosticStage::ResolveEndpoint,"selected_host_mismatch");
 m_impl->succeeded(InputDiagnosticStage::ResolveEndpoint);
 if(profile.input.inputChannels==0 || profile.input.inputChannels>32 || profile.input.inputChannels>diagnostic.maxInputChannels
  || profile.input.selectedChannel>=profile.input.inputChannels)
  return m_impl->localFailure(InputDiagnosticStage::ValidateChannels,"invalid_input_channels");
 m_impl->succeeded(InputDiagnosticStage::ValidateChannels);
 if(!callback || !profile.wasapiShared || (profile.input.sampleRate!=44100 && profile.input.sampleRate!=48000)
  || profile.input.maxCallback==0 || profile.input.maxCallback>RecordingRing::MaximumCallback
  || !profile.input.maxTakeFrames || profile.input.maxTakeFrames>std::uint64_t(profile.input.sampleRate)*120)
  return m_impl->localFailure(InputDiagnosticStage::ValidateFormat,"invalid_input_profile");
 m_impl->succeeded(InputDiagnosticStage::ValidateFormat);
 PaWasapiStreamInfo wasapi{};wasapi.size=sizeof(wasapi);wasapi.hostApiType=paWASAPI;wasapi.version=1;
 wasapi.flags=0;wasapi.channelMask=0;wasapi.hostProcessorOutput=nullptr;wasapi.hostProcessorInput=nullptr;
 wasapi.threadPriority=eThreadPriorityCapture;wasapi.streamCategory=eAudioCategoryOther;wasapi.streamOption=eStreamOptionNone;
 PaStreamParameters inputParameters{};inputParameters.device=index;inputParameters.channelCount=int(profile.input.inputChannels);
 inputParameters.sampleFormat=paFloat32;inputParameters.suggestedLatency=device->defaultLowInputLatency;inputParameters.hostApiSpecificStreamInfo=&wasapi;
 const auto supported=m_impl->api.formatSupported(&inputParameters,nullptr,double(profile.input.sampleRate));
 const auto formatResult=m_impl->result(InputDiagnosticStage::FormatSupport,supported);
 if(!formatResult.ok())return formatResult;
 m_impl->callback=callback;m_impl->context=context;m_impl->channels=int(profile.input.inputChannels);m_impl->selected=index;
 const auto opened=m_impl->api.open(&m_impl->stream,&inputParameters,nullptr,double(profile.input.sampleRate),
  profile.input.maxCallback,paNoFlag,&Impl::streamCallback,m_impl.get());
 const auto openResult=m_impl->result(InputDiagnosticStage::OpenStream,opened);
 if(opened!=paNoError){m_impl->stream=nullptr;m_impl->callback=nullptr;m_impl->context=nullptr;}
 return openResult;
}
RecordingDriverResult RecordingPortAudioInputDriver::start() noexcept
{
 if(!m_impl->stream)return m_impl->localFailure(InputDiagnosticStage::StartStream,"stream_not_open");
 const auto error=m_impl->api.start(m_impl->stream);
 auto result=m_impl->result(InputDiagnosticStage::StartStream,error,false);
 if(error!=paNoError)result.callbackQuiescent=m_impl->api.isStopped(m_impl->stream)==1;
 return result;
}
RecordingDriverResult RecordingPortAudioInputDriver::stop() noexcept
{
 if(!m_impl->stream)return m_impl->localFailure(InputDiagnosticStage::StopStream,"stream_not_open",false);
 const auto error=m_impl->api.stop(m_impl->stream);
 if(error==paStreamIsStopped)
 {
  if(m_impl->api.isStopped(m_impl->stream)==1)return m_impl->result(InputDiagnosticStage::StopStream,paNoError);
  return m_impl->result(InputDiagnosticStage::StopStream,error,false);
 }
 auto result=m_impl->result(InputDiagnosticStage::StopStream,error,false);
 const auto stopped=m_impl->api.isStopped(m_impl->stream);
 if(!result.ok())return result;
 if(stopped!=1)return m_impl->localFailure(InputDiagnosticStage::StopStream,"callback_not_quiescent",false);
 result.callbackQuiescent=true;return result;
}
RecordingDriverResult RecordingPortAudioInputDriver::close() noexcept
{
 if(!m_impl->stream)return {"ok",true,m_impl->diagnostic};
 const auto stopped=m_impl->api.isStopped(m_impl->stream);if(stopped!=1)return m_impl->localFailure(InputDiagnosticStage::CloseStream,"callback_not_quiescent",false);
 const auto error=m_impl->api.close(m_impl->stream);const auto result=m_impl->result(InputDiagnosticStage::CloseStream,error,error==paNoError);
 if(error!=paNoError)return result;
 m_impl->stream=nullptr;m_impl->callback=nullptr;m_impl->context=nullptr;m_impl->selected=paNoDevice;m_impl->channels=0;return result;
}
RecordingDriverResult RecordingPortAudioInputDriver::terminate() noexcept
{
 if(!m_impl->initialized)return {"ok",true,m_impl->diagnostic};
 if(m_impl->stream)return m_impl->localFailure(InputDiagnosticStage::Terminate,"stream_still_open",false);
 const auto error=m_impl->api.terminate();const auto result=m_impl->result(InputDiagnosticStage::Terminate,error);
 if(error==paNoError)m_impl->initialized=false;
 return result;
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
