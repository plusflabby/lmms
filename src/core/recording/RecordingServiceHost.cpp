#include "RecordingServiceHost.h"
#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <QUuid>
#include <limits>
#include <utility>
namespace lmms::recording {
struct RecordingServiceHost::Impl
{
 explicit Impl(std::unique_ptr<RecordingInputDriver> value)
  :driver(std::move(value)),controller(*driver),ownerThread(QThread::currentThread()){}
 std::unique_ptr<RecordingInputDriver> driver;
 RecordingInputController controller;
 std::unique_ptr<RecordingLibrary> library;
 ProjectIdentity project;
 QString libraryRoot;
 QThread* ownerThread;
 std::optional<LocalInputTicket> ticket;
 SelectedInputProfile selected;
 InputEpoch epoch;
 std::uint64_t rootRevision=0,localTicketRevision=0;
 InputEpoch latestLocalTicket;
 QString authorizationId;
 std::uint64_t lastNowMilliseconds=0;
 InputRuntimeLease runtimeLease;
 std::shared_ptr<void> retainedRuntime;
 std::unique_ptr<RecordingWriter> writer;
 std::unique_ptr<RecordingTakeReceipt> receipt;
 WriterSnapshot lastWriter;
 QString receiptCode;
 int receiptRecords=0;
 RecordingStopAction stopAction=RecordingStopAction::None;
 void markStop(RecordingStopAction action){if(stopAction==RecordingStopAction::None)stopAction=action;}
 RecordingHostState state=RecordingHostState::Closed;
 QString errorCode;
 bool startRequested=false,inputClosed=false,shutdownRequested=false;
 void assertOwner() const {Q_ASSERT(QThread::currentThread()==ownerThread);}
 bool hasStream() const
 {const auto state=controller.snapshot().state;return state==RecordingControllerState::Opening || state==RecordingControllerState::Ready
   || state==RecordingControllerState::Capturing || state==RecordingControllerState::Stopping || state==RecordingControllerState::FailedRetained;}
 void closeInputIfQuiescent()
 {
  if(!writer || inputClosed)return;
  const auto snapshot=controller.snapshot();
  if(snapshot.state!=RecordingControllerState::Quiescent && snapshot.state!=RecordingControllerState::Closed)return;
  InputSnapshot finalInput=snapshot.input;finalInput.epoch=epoch;
  writer->requestInputClosed(epoch,finalInput);inputClosed=true;state=RecordingHostState::Finalizing;
 }
 RecordingHostSnapshot snapshot() const noexcept
 {
  RecordingHostSnapshot result;result.state=state;result.errorCode=errorCode;result.input=controller.snapshot();
  result.armed=ticket.has_value();result.writer=writer?writer->poll():lastWriter;
  result.receiptCode=receiptCode;result.receiptRecords=receiptRecords;
  if(result.errorCode.isEmpty() && !receiptCode.isEmpty() && receiptCode!="receipt_saved" && receiptCode!="receipt_pending")
   result.errorCode=receiptCode;
  result.quiescent=state==RecordingHostState::Closed && !writer && !retainedRuntime && !ticket
   && result.input.state==RecordingControllerState::Closed && !result.input.driverInitialized;return result;
 }
};
RecordingServiceHost::RecordingServiceHost(std::unique_ptr<RecordingInputDriver> driver)
 :m_impl(std::make_unique<Impl>(std::move(driver))){}
RecordingServiceHost::~RecordingServiceHost()
{
 if(!m_impl || m_impl->writer || m_impl->retainedRuntime)return;
 if(!quiescent())std::ignore=disarm();
 Q_ASSERT(quiescent());
}
RecordingInputDevicePage RecordingServiceHost::enumerateInputs(std::uint32_t offset,std::uint32_t limit)
{
 m_impl->assertOwner();if(m_impl->state!=RecordingHostState::Closed)return {"input_busy",{},offset,false};
 return m_impl->controller.enumerateInputs(offset,limit);
}
RecordingLibrarySelectionResult RecordingServiceHost::selectRecordingRoot(const QString& path,RecordingProjectIdentity& identity)
{
 m_impl->assertOwner();
 if(m_impl->writer || m_impl->state==RecordingHostState::Capturing || m_impl->state==RecordingHostState::Stopping
  || m_impl->state==RecordingHostState::Finalizing || m_impl->state==RecordingHostState::FailedRetained)
  return {"recording_busy",{},m_impl->rootRevision,false};
 const auto normalized=QFileInfo(path).absoluteFilePath();
 if(m_impl->library && QDir::cleanPath(m_impl->libraryRoot)==QDir::cleanPath(normalized))
 {
  const auto current=identity.getOrCreateForCurrentProject(*m_impl->library);
  if(current.code!="ok")return {current.code,{},m_impl->rootRevision,false};
  if(current.identity.id!=m_impl->project.id)
  {
   if(m_impl->rootRevision==std::numeric_limits<std::uint64_t>::max())return {"resource_limit",{},m_impl->rootRevision,false};
   ++m_impl->rootRevision;m_impl->ticket.reset();m_impl->project=current.identity;
  }
  return {"ok",m_impl->project,m_impl->rootRevision,true};
 }
 auto approval=RecordingLibrary::authorizeLocalRoot(path);if(!approval.library)return {approval.code,{},m_impl->rootRevision,false};
 const auto prepared=identity.getOrCreateForCurrentProject(*approval.library);
 if(prepared.code!="ok")return {prepared.code,{},m_impl->rootRevision,false};
 const auto revision=identity.revision();if(!revision)return {"invalid_project_revision",{},m_impl->rootRevision,false};
 if(m_impl->rootRevision==std::numeric_limits<std::uint64_t>::max())return {"resource_limit",{},m_impl->rootRevision,false};
 ++m_impl->rootRevision;if(!m_impl->rootRevision)++m_impl->rootRevision;
 m_impl->library=std::move(approval.library);m_impl->project=prepared.identity;m_impl->libraryRoot=normalized;
 m_impl->ticket.reset();m_impl->state=RecordingHostState::Closed;return {"ok",m_impl->project,m_impl->rootRevision,true};
}
RecordingLibrary::NewTakeResult RecordingServiceHost::createUniqueTake(const TakeIdentity& take,std::uint32_t rate,std::uint64_t maximum)
{
 m_impl->assertOwner();if(!m_impl->library || m_impl->project.id.isEmpty())return {"unauthorized_library",{}};
 return m_impl->library->createUniqueTake(m_impl->project,take,rate,maximum);
}
RecordingLibrary::SealedSourceResult RecordingServiceHost::openSealedTake(const ProjectIdentity& project,const QString& takeId)
{
	m_impl->assertOwner();
	if(!m_impl->library || m_impl->project.id!=project.id)return {QStringLiteral("recording_library_required"),{}};
	return m_impl->library->openSealedTake(project,takeId);
}
RecordingLibrary::SealedSourceResult RecordingServiceHost::reopenRecordedTake(RecordingProjectIdentity& identity,
	const ProjectIdentity& sourceProject,const QString& takeId)
{
	m_impl->assertOwner();
	if(!m_impl->library || m_impl->project.id.isEmpty())return {QStringLiteral("recording_library_required"),{}};
	const auto current=identity.getOrCreateForCurrentProject(*m_impl->library);
	if(current.code!=QStringLiteral("ok"))return {current.code,{}};
	if(!identity.sourceIdentityTrusted())return {QStringLiteral("project_identity_unverified"),{}};
	return m_impl->library->openSealedTake(sourceProject,takeId);
}
ProjectIdentity RecordingServiceHost::projectIdentity() const noexcept{return m_impl->project;}
std::uint64_t RecordingServiceHost::rootRevision() const noexcept{return m_impl->rootRevision;}
RecordingHostResult RecordingServiceHost::arm(const SelectedInputProfile& profile,InputEpoch epoch,std::uint64_t revision)
{
 m_impl->assertOwner();if(m_impl->state!=RecordingHostState::Closed || m_impl->ticket)return {false,"input_busy",m_impl->state};
 auto result=m_impl->controller.arm(profile,epoch,revision);if(!result.ok)return {false,result.code,m_impl->state};
 m_impl->ticket.emplace(std::move(result.ticket));m_impl->selected=profile;m_impl->epoch=epoch;m_impl->rootRevision=revision;
 m_impl->lastWriter={};m_impl->receiptCode.clear();m_impl->receiptRecords=0;
 m_impl->state=RecordingHostState::Armed;return {true,"armed",m_impl->state};
}
RecordingTicketResult RecordingServiceHost::localArmTicket(const SelectedInputProfile& profile,std::uint64_t generation)
{
 m_impl->assertOwner();if(!m_impl->library || m_impl->project.id.isEmpty())return {"library_required",{},false};
 if(m_impl->state!=RecordingHostState::Closed || m_impl->ticket || m_impl->writer)return {"input_busy",{},false};
 if(!generation || m_impl->localTicketRevision==std::numeric_limits<std::uint64_t>::max())return {"resource_limit",{},false};
 m_impl->latestLocalTicket={generation,++m_impl->localTicketRevision};
 return m_impl->controller.arm(profile,m_impl->latestLocalTicket,m_impl->rootRevision);
}
RecordingHostResult RecordingServiceHost::adoptLocalArm(LocalInputTicket&& ticket)
{
 m_impl->assertOwner();if(m_impl->state!=RecordingHostState::Closed || m_impl->ticket || m_impl->writer)return {false,"input_busy",m_impl->state};
 if(!m_impl->library || ticket.m_rootRevision!=m_impl->rootRevision || ticket.m_epoch!=m_impl->latestLocalTicket
  || !m_impl->controller.ownsLocalTicket(ticket))return {false,"invalid_local_ticket",m_impl->state};
 m_impl->selected=ticket.m_selection;m_impl->epoch=ticket.m_epoch;m_impl->ticket.emplace(std::move(ticket));ticket.m_valid=false;
 m_impl->authorizationId=QUuid::createUuid().toString(QUuid::WithoutBraces);m_impl->lastWriter={};m_impl->receiptCode.clear();m_impl->receiptRecords=0;
 m_impl->state=RecordingHostState::Armed;return {true,"armed",m_impl->state};
}
RecordingLocalArmSnapshot RecordingServiceHost::localArmSnapshot() const noexcept
{
 return {m_impl->epoch,m_impl->authorizationId,m_impl->rootRevision,m_impl->selected.input.maxTakeFrames,m_impl->selected.input.sampleRate,
  m_impl->state==RecordingHostState::Armed && m_impl->ticket.has_value()};
}
RecordingHostSnapshot RecordingServiceHost::snapshot() const noexcept{return m_impl->snapshot();}
RecordingHostResult RecordingServiceHost::startTake(TakeDescriptor descriptor,std::unique_ptr<RecordingWriterSink> sink,std::uint64_t now,
 std::unique_ptr<RecordingReceiptSink> receipt)
{
 m_impl->assertOwner();if(!m_impl->ticket)return {false,"local_arm_required",m_impl->state};
 if(m_impl->state!=RecordingHostState::Armed || !sink || descriptor.epoch!=m_impl->epoch
  || descriptor.projectGeneration!=m_impl->epoch.projectGeneration || descriptor.sampleRate!=m_impl->selected.input.sampleRate
  || descriptor.maxRawFrames>m_impl->selected.input.maxTakeFrames)return {false,"invalid_take",m_impl->state};
 m_impl->runtimeLease=m_impl->controller.createRuntimeLease(m_impl->selected);
 if(!m_impl->runtimeLease)return {false,"input_runtime_unavailable",m_impl->state};
 m_impl->retainedRuntime=m_impl->runtimeLease.retainOwner();m_impl->writer=std::make_unique<RecordingWriter>();
 const auto begun=m_impl->writer->begin(descriptor,std::move(sink),m_impl->runtimeLease.ring());
 if(!begun.ok){m_impl->writer.reset();m_impl->retainedRuntime.reset();m_impl->runtimeLease=InputRuntimeLease{};return {false,begun.code,m_impl->state};}
 m_impl->lastWriter={};m_impl->receiptRecords=0;m_impl->receiptCode.clear();m_impl->stopAction=RecordingStopAction::None;
 if(receipt)
 {m_impl->receipt=std::make_unique<RecordingTakeReceipt>(descriptor,std::move(receipt));m_impl->receiptCode="receipt_pending";}
 m_impl->startRequested=true;m_impl->inputClosed=false;m_impl->shutdownRequested=false;m_impl->errorCode.clear();
 m_impl->lastNowMilliseconds=now;m_impl->state=RecordingHostState::WriterStarting;return {true,"writer_starting",m_impl->state};
}
bool RecordingServiceHost::noteSessionFacts(InputEpoch epoch,const TakeSessionFacts& facts) noexcept
{
 m_impl->assertOwner();if(epoch!=m_impl->epoch || !m_impl->receipt)return false;
 m_impl->receipt->noteSessionFacts(facts);return true;
}
RecordingHostResult RecordingServiceHost::requestStop(InputEpoch epoch)
{
 m_impl->assertOwner();if(epoch!=m_impl->epoch || !epoch)return {false,"stale_epoch",m_impl->state};
 if(!m_impl->writer)return {false,"no_take",m_impl->state};
 m_impl->markStop(RecordingStopAction::Stop);
 m_impl->startRequested=false;
 const auto input=m_impl->controller.snapshot();
 if(input.state==RecordingControllerState::Capturing || input.state==RecordingControllerState::Stopping
  || input.state==RecordingControllerState::FailedRetained || input.state==RecordingControllerState::Ready)
 {
  const auto stopped=m_impl->controller.requestStop(epoch);
  if(!stopped.ok){m_impl->errorCode=stopped.code;m_impl->state=RecordingHostState::FailedRetained;return {false,stopped.code,m_impl->state};}
 }
 m_impl->closeInputIfQuiescent();return {true,"stop_requested",m_impl->state};
}
RecordingHostResult RecordingServiceHost::disarm()
{
 m_impl->assertOwner();m_impl->ticket.reset();m_impl->authorizationId.clear();m_impl->startRequested=false;
 if(m_impl->writer)
 {
  m_impl->markStop(RecordingStopAction::Disarm);
  const auto state=m_impl->controller.snapshot().state;
  if(state==RecordingControllerState::Capturing || state==RecordingControllerState::Stopping || state==RecordingControllerState::FailedRetained)
   return requestStop(m_impl->epoch);
  m_impl->closeInputIfQuiescent();m_impl->state=RecordingHostState::Finalizing;return {true,"disarmed",m_impl->state};
 }
 const auto closed=m_impl->controller.closeAfterQuiescence();
 if(!closed.ok){m_impl->errorCode=closed.code;m_impl->state=RecordingHostState::FailedRetained;return {false,closed.code,m_impl->state};}
 m_impl->state=RecordingHostState::Closed;m_impl->errorCode.clear();return {true,"disarmed",m_impl->state};
}
RecordingHostResult RecordingServiceHost::notifyAuthorizationChanged(std::uint64_t revision)
{
 m_impl->assertOwner();if(revision==m_impl->rootRevision)return {true,"unchanged",m_impl->state};
 m_impl->rootRevision=revision;m_impl->ticket.reset();m_impl->authorizationId.clear();
 if(m_impl->writer){m_impl->markStop(RecordingStopAction::ProjectChange);return requestStop(m_impl->epoch);}
 return disarm();
}
RecordingHostResult RecordingServiceHost::requestBackendChange()
{
 m_impl->assertOwner();m_impl->ticket.reset();m_impl->authorizationId.clear();
 if(m_impl->writer){m_impl->markStop(RecordingStopAction::BackendChange);return requestStop(m_impl->epoch);}
 return disarm();
}
RecordingHostSnapshot RecordingServiceHost::poll(std::uint64_t now) noexcept
{
 m_impl->assertOwner();m_impl->lastNowMilliseconds=now;
 auto input=m_impl->controller.poll(now);
 if(m_impl->writer && m_impl->startRequested && m_impl->writer->poll().state==WriterState::Ready && m_impl->ticket)
 {
  m_impl->state=RecordingHostState::OpeningInput;
  const auto opened=m_impl->controller.open(std::move(*m_impl->ticket),std::move(m_impl->runtimeLease));m_impl->ticket.reset();
  if(!opened.ok)
  {m_impl->errorCode=opened.code;m_impl->state=RecordingHostState::Finalizing;m_impl->startRequested=false;m_impl->closeInputIfQuiescent();}
  else
  {
   const auto started=m_impl->controller.beginCapture(m_impl->epoch,now);
   if(!started.ok){m_impl->errorCode=started.code;m_impl->state=RecordingHostState::FailedRetained;}
   else m_impl->state=RecordingHostState::Capturing;
  }
  input=m_impl->controller.poll(now);
 }
 if(m_impl->writer && input.state==RecordingControllerState::Quiescent)m_impl->closeInputIfQuiescent();
 if(m_impl->writer)
 {
  const auto written=m_impl->writer->poll();
  const bool terminal=written.state==WriterState::Sealed || written.state==WriterState::Incomplete || written.state==WriterState::NoTakeCreated;
  if(terminal && m_impl->inputClosed)
  {
   const auto joined=m_impl->writer->joinAfterExit();
   if(joined.ok)
   {
    const auto closed=m_impl->controller.closeAfterQuiescence();
    m_impl->lastWriter=written;
    if(m_impl->receipt)
    {
     std::ignore=m_impl->receipt->append(m_impl->controller.snapshot(),written,closed.ok,m_impl->stopAction);
     m_impl->receiptCode=m_impl->receipt->code();m_impl->receiptRecords=m_impl->receipt->recordCount();
    }
    if(closed.ok)
    {m_impl->receipt.reset();m_impl->writer.reset();m_impl->retainedRuntime.reset();m_impl->runtimeLease=InputRuntimeLease{};m_impl->ticket.reset();m_impl->state=RecordingHostState::Closed;m_impl->startRequested=false;m_impl->inputClosed=false;m_impl->errorCode.clear();}
    else{m_impl->state=RecordingHostState::FailedRetained;m_impl->errorCode=closed.code;}
   }
  }
  else if(written.state==WriterState::StorageBlocked)m_impl->state=RecordingHostState::Finalizing;
 }
 else if(m_impl->state==RecordingHostState::Armed)
  m_impl->state=RecordingHostState::Armed;
 return m_impl->snapshot();
}
RecordingHostResult RecordingServiceHost::requestShutdown(std::uint64_t now)
{
 m_impl->assertOwner();m_impl->shutdownRequested=true;
 if(m_impl->writer)
 {
  m_impl->markStop(RecordingStopAction::Shutdown);
  const auto input=m_impl->controller.snapshot();
  if(input.state==RecordingControllerState::Capturing || input.state==RecordingControllerState::Ready
   || input.state==RecordingControllerState::Stopping || input.state==RecordingControllerState::FailedRetained)
   std::ignore=requestStop(m_impl->epoch);
  else m_impl->closeInputIfQuiescent();
 }
 else std::ignore=disarm();
 const auto current=poll(now);return current.quiescent?RecordingHostResult{true,"quiescent",current.state}:RecordingHostResult{false,"shutdown_pending",current.state};
}
RecordingHostResult RecordingServiceHost::requestShutdown(){return requestShutdown(m_impl->lastNowMilliseconds);}
bool RecordingServiceHost::quiescent() const noexcept{return m_impl->snapshot().quiescent;}
}
