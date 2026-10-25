#include "RecordingInputController.h"
#include <QRandomGenerator>
#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>
namespace lmms::recording {
namespace {
bool validProfile(const SelectedInputProfile& selected) noexcept
{
 const auto& profile=selected.input;
 return selected.wasapiShared && selected.hostApiName==QStringLiteral("WASAPI") && !selected.deviceId.isEmpty()
  && (profile.sampleRate==44100 || profile.sampleRate==48000) && profile.inputChannels>0 && profile.inputChannels<=32
  && profile.selectedChannel<profile.inputChannels && profile.maxCallback>0 && profile.maxCallback<=RecordingRing::MaximumCallback
  && profile.maxTakeFrames>0 && profile.maxTakeFrames<=std::uint64_t(profile.sampleRate)*120;
}
bool sameProfile(const SelectedInputProfile& left,const SelectedInputProfile& right) noexcept
{
 const auto& a=left.input;const auto& b=right.input;
 return left.deviceId==right.deviceId && left.hostApiName==right.hostApiName && left.wasapiShared==right.wasapiShared
  && a.sampleRate==b.sampleRate && a.inputChannels==b.inputChannels && a.selectedChannel==b.selectedChannel
  && a.maxCallback==b.maxCallback && a.maxTakeFrames==b.maxTakeFrames;
}
}
struct RecordingInputRuntimeStorage
{
 explicit RecordingInputRuntimeStorage(const SelectedInputProfile& selected)
  :profile(selected),ring(selected.input.sampleRate,RecordingRing::Capacity,selected.input.maxTakeFrames),runtime(selected.input,ring){}
 SelectedInputProfile profile;
 RecordingRing ring;
 RecordingInputRuntime runtime;
};
RecordingRing& InputRuntimeLease::ring() const noexcept{return m_storage->ring;}
std::shared_ptr<void> InputRuntimeLease::retainOwner() const noexcept{return m_storage;}
LocalInputTicket::LocalInputTicket(SelectedInputProfile selection,InputEpoch epoch,std::uint64_t cookie,std::uint64_t revision)
 :m_selection(std::move(selection)),m_epoch(epoch),m_processCookie(cookie),m_rootRevision(revision),m_valid(true){}
struct RecordingInputController::Impl
{
 explicit Impl(RecordingInputDriver& value):driver(value),processCookie([]{const auto cookie=QRandomGenerator::system()->generate64();return cookie?cookie:std::uint64_t(1);}()){}
 RecordingInputDriver& driver;
 const std::uint64_t processCookie;
 bool initialized=false,streamOpen=false,started=false,quiescenceProven=false;
 RecordingControllerState state=RecordingControllerState::Closed;
 QString errorCode;
 std::vector<RecordingInputDevice> knownDevices;
 std::shared_ptr<RecordingInputRuntimeStorage> storage;
 std::atomic<RecordingInputRuntimeStorage*> activeStorage{nullptr};
 InputEpoch epoch;
 std::uint64_t startedAtMilliseconds=0;
 std::atomic<std::uint32_t> callbacksInFlight{0};
 std::atomic_bool firstCallbackSeen{false},acceptingCallbacks{false};
 static InputDecision callback(void* context,const InputBlock& block) noexcept
 {
  auto* self=static_cast<Impl*>(context);
  const auto entered=self->callbacksInFlight.fetch_add(1,std::memory_order_acq_rel);
  if(entered==std::numeric_limits<std::uint32_t>::max())
  {self->callbacksInFlight.store(std::numeric_limits<std::uint32_t>::max(),std::memory_order_release);self->acceptingCallbacks.store(false,std::memory_order_release);return InputDecision::Stop;}
  auto result=InputDecision::Stop;
  auto* storage=self->activeStorage.load(std::memory_order_acquire);
  if(self->acceptingCallbacks.load(std::memory_order_acquire) && storage)
  {result=storage->runtime.process(block);self->firstCallbackSeen.store(true,std::memory_order_release);}
  self->callbacksInFlight.fetch_sub(1,std::memory_order_acq_rel);
  return result;
 }
 RecordingControllerSnapshot snapshot() const noexcept
 {
  RecordingControllerSnapshot result;result.state=state;result.errorCode=errorCode;result.driverInitialized=initialized;
  result.callbacksInFlight=callbacksInFlight.load(std::memory_order_acquire);
  result.firstCallbackSeen=firstCallbackSeen.load(std::memory_order_acquire);
  if(storage)result.input=storage->runtime.snapshot();
  return result;
 }
};
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
RecordingInputController::RecordingInputController(RecordingInputDriver& driver):m_impl(std::make_unique<Impl>(driver)){}
RecordingInputController::~RecordingInputController()=default;
RecordingInputDevicePage RecordingInputController::enumerateInputs(std::uint32_t offset,std::uint32_t limit)
{
 if(!limit || limit>64)return {"invalid_page",{},offset,false};
 if(m_impl->state!=RecordingControllerState::Closed)return {"input_busy",{},offset,false};
 if(!m_impl->initialized)
 {const auto initialized=m_impl->driver.initialize();if(!initialized.ok())return {initialized.code.isEmpty()?QStringLiteral("init_failed"):initialized.code,{},offset,false};m_impl->initialized=true;}
 auto page=m_impl->driver.enumerateInputs(offset,limit);
 if(!page.code.isEmpty() && page.code!=QStringLiteral("ok")){m_impl->errorCode=page.code;return page;}
 if(page.devices.size()>limit)return {"invalid_device_page",{},offset,false};
 for(const auto& device:page.devices)
 {
  if(device.id.isEmpty() || device.name.isEmpty() || device.maxInputChannels==0)return {"invalid_device_page",{},offset,false};
  const auto duplicate=std::find_if(m_impl->knownDevices.begin(),m_impl->knownDevices.end(),[&](const auto& old){return old.id==device.id;});
  if(duplicate==m_impl->knownDevices.end())m_impl->knownDevices.push_back(device);
  else *duplicate=device;
 }
 page.code=QStringLiteral("ok");return page;
}
RecordingTicketResult RecordingInputController::arm(const SelectedInputProfile& selection,InputEpoch epoch,std::uint64_t rootRevision)
{
 if(!m_impl->initialized)return {"device_selection_required",{},false};
 if(m_impl->state!=RecordingControllerState::Closed)return {"input_busy",{},false};
 if(!epoch || !rootRevision || !validProfile(selection))return {"invalid_input_profile",{},false};
 const auto device=std::find_if(m_impl->knownDevices.begin(),m_impl->knownDevices.end(),[&](const auto& item){return item.id==selection.deviceId;});
 if(device==m_impl->knownDevices.end() || device->hostApiName!=selection.hostApiName || selection.input.inputChannels>device->maxInputChannels)
  return {"selected_device_stale",{},false};
 return {"ok",LocalInputTicket(selection,epoch,m_impl->processCookie,rootRevision),true};
}
InputRuntimeLease RecordingInputController::createRuntimeLease(const SelectedInputProfile& selection)
{
 InputRuntimeLease lease;if(!validProfile(selection))return lease;
 auto storage=std::make_shared<RecordingInputRuntimeStorage>(selection);
 if(!storage->ring.isReady())return lease;
 lease.m_storage=std::move(storage);return lease;
}
RecordingControllerResult RecordingInputController::open(LocalInputTicket&& ticket,InputRuntimeLease&& lease)
{
 if(!ticket.m_valid)return {"no_local_ticket",m_impl->state,false};
 if(!m_impl->initialized)return {"device_selection_required",m_impl->state,false};
 if(m_impl->state!=RecordingControllerState::Closed)return {"input_busy",m_impl->state,false};
 if(ticket.m_processCookie!=m_impl->processCookie || !ticket.m_epoch || !ticket.m_rootRevision
  || !lease.m_storage || !sameProfile(ticket.m_selection,lease.m_storage->profile))return {"ticket_or_runtime_mismatch",m_impl->state,false};
 m_impl->state=RecordingControllerState::Opening;m_impl->errorCode.clear();m_impl->epoch=ticket.m_epoch;
 m_impl->storage=std::move(lease.m_storage);m_impl->firstCallbackSeen.store(false,std::memory_order_release);
 m_impl->activeStorage.store(m_impl->storage.get(),std::memory_order_release);
 m_impl->acceptingCallbacks.store(false,std::memory_order_release);
 const auto opened=m_impl->driver.openInputOnly(ticket.m_selection,&Impl::callback,m_impl.get());ticket.m_valid=false;
 if(!opened.ok())
 {
  m_impl->errorCode=opened.code.isEmpty()?QStringLiteral("open_failed"):opened.code;
  if(opened.callbackQuiescent){m_impl->quiescenceProven=true;m_impl->activeStorage.store(nullptr,std::memory_order_release);m_impl->storage.reset();m_impl->state=RecordingControllerState::Closed;return {m_impl->errorCode,m_impl->state,false};}
  m_impl->state=RecordingControllerState::FailedRetained;return {m_impl->errorCode,m_impl->state,false};
 }
 m_impl->streamOpen=true;m_impl->state=RecordingControllerState::Ready;return {"ok",m_impl->state,true};
}
RecordingControllerResult RecordingInputController::beginCapture(InputEpoch epoch,std::uint64_t now)
{
 if(m_impl->state!=RecordingControllerState::Ready || epoch!=m_impl->epoch || !m_impl->storage)return {"stale_or_not_ready",m_impl->state,false};
 if(!m_impl->storage->runtime.enableCapture(epoch))return {"capture_enable_failed",m_impl->state,false};
 m_impl->startedAtMilliseconds=now;m_impl->firstCallbackSeen.store(false,std::memory_order_release);
 m_impl->acceptingCallbacks.store(true,std::memory_order_release);
 const auto result=m_impl->driver.start();
 if(!result.ok())
 {
  m_impl->acceptingCallbacks.store(false,std::memory_order_release);m_impl->storage->runtime.requestStop(epoch);
  m_impl->errorCode=result.code.isEmpty()?QStringLiteral("start_failed"):result.code;
  if(result.callbackQuiescent && m_impl->callbacksInFlight.load(std::memory_order_acquire)==0)
  {m_impl->quiescenceProven=true;m_impl->state=RecordingControllerState::Quiescent;}
  else m_impl->state=RecordingControllerState::FailedRetained;
  return {m_impl->errorCode,m_impl->state,false};
 }
 m_impl->started=true;m_impl->state=RecordingControllerState::Capturing;return {"ok",m_impl->state,true};
}
RecordingControllerResult RecordingInputController::requestStop(InputEpoch epoch) noexcept
{
 if(epoch!=m_impl->epoch || !epoch)return {"stale_epoch",m_impl->state,false};
 if(m_impl->state==RecordingControllerState::Quiescent)return {"ok",m_impl->state,true};
 if(m_impl->state==RecordingControllerState::Ready)
 {m_impl->quiescenceProven=true;m_impl->state=RecordingControllerState::Quiescent;return {"ok",m_impl->state,true};}
 if(m_impl->state!=RecordingControllerState::Capturing && m_impl->state!=RecordingControllerState::Stopping
  && m_impl->state!=RecordingControllerState::FailedRetained)return {"not_capturing",m_impl->state,false};
 if(m_impl->storage)m_impl->storage->runtime.requestStop(epoch);
 m_impl->acceptingCallbacks.store(false,std::memory_order_release);m_impl->state=RecordingControllerState::Stopping;
 const auto stopped=m_impl->driver.stop();
 if(!stopped.ok())
 {m_impl->quiescenceProven=false;m_impl->errorCode=stopped.code.isEmpty()?QStringLiteral("stop_failed"):stopped.code;m_impl->state=RecordingControllerState::FailedRetained;return {m_impl->errorCode,m_impl->state,false};}
 if(!stopped.callbackQuiescent || m_impl->callbacksInFlight.load(std::memory_order_acquire)!=0)
 {m_impl->quiescenceProven=false;m_impl->errorCode=QStringLiteral("callback_not_quiescent");m_impl->state=RecordingControllerState::FailedRetained;return {m_impl->errorCode,m_impl->state,false};}
 m_impl->started=false;m_impl->quiescenceProven=true;m_impl->state=RecordingControllerState::Quiescent;return {"ok",m_impl->state,true};
}
RecordingControllerSnapshot RecordingInputController::poll(std::uint64_t now) noexcept
{
 if(m_impl->state==RecordingControllerState::Capturing && !m_impl->firstCallbackSeen.load(std::memory_order_acquire)
  && now>=m_impl->startedAtMilliseconds && now-m_impl->startedAtMilliseconds>=2000)
 {
  m_impl->errorCode=QStringLiteral("first_callback_timeout");
  const auto stopped=requestStop(m_impl->epoch);
  if(!stopped.ok)m_impl->state=RecordingControllerState::FailedRetained;
 }
 return m_impl->snapshot();
}
RecordingControllerResult RecordingInputController::closeAfterQuiescence()
{
 if(m_impl->state==RecordingControllerState::Closed)
 {
  if(!m_impl->initialized)return {"ok",m_impl->state,true};
  m_impl->quiescenceProven=true;
  const auto terminated=m_impl->driver.terminate();
  if(!terminated.ok()){m_impl->errorCode=terminated.code;m_impl->state=RecordingControllerState::FailedRetained;return {"terminate_failed",m_impl->state,false};}
  m_impl->initialized=false;m_impl->knownDevices.clear();return {"ok",m_impl->state,true};
 }
 if((m_impl->state!=RecordingControllerState::Quiescent && !(m_impl->state==RecordingControllerState::FailedRetained && m_impl->quiescenceProven))
  || m_impl->callbacksInFlight.load(std::memory_order_acquire)!=0)
  return {"not_quiescent",m_impl->state,false};
 if(m_impl->streamOpen)
 {
  const auto closed=m_impl->driver.close();
  if(!closed.callbackQuiescent)m_impl->quiescenceProven=false;
  if(!closed.ok() || !closed.callbackQuiescent){m_impl->errorCode=closed.code.isEmpty()?QStringLiteral("close_failed"):closed.code;m_impl->state=RecordingControllerState::FailedRetained;return {m_impl->errorCode,m_impl->state,false};}
  m_impl->streamOpen=false;
 }
 if(m_impl->initialized)
 {
  const auto terminated=m_impl->driver.terminate();
  if(!terminated.ok()){m_impl->errorCode=terminated.code.isEmpty()?QStringLiteral("terminate_failed"):terminated.code;m_impl->state=RecordingControllerState::FailedRetained;return {m_impl->errorCode,m_impl->state,false};}
  m_impl->initialized=false;
 }
 m_impl->activeStorage.store(nullptr,std::memory_order_release);m_impl->storage.reset();m_impl->knownDevices.clear();m_impl->errorCode.clear();m_impl->quiescenceProven=false;m_impl->state=RecordingControllerState::Closed;return {"ok",m_impl->state,true};
}
RecordingControllerSnapshot RecordingInputController::snapshot() const noexcept{return m_impl->snapshot();}
}
