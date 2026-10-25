#ifndef LMMS_RECORDING_INPUT_CONTROLLER_H
#define LMMS_RECORDING_INPUT_CONTROLLER_H
#include "RecordingInputRuntime.h"
#include "RecordingInputDiagnostic.h"
#include <QString>
#include <memory>
#include <vector>
namespace lmms::recording {
struct SelectedInputProfile
{
 QString deviceId,hostApiName;
 InputProfile input;
 bool wasapiShared=false;
};
struct RecordingInputDevice
{
 QString id,name,hostApiName;
 std::uint32_t maxInputChannels=0;
};
struct RecordingInputDevicePage
{
 QString code;
 std::vector<RecordingInputDevice> devices;
 std::uint32_t nextOffset=0;
 bool hasMore=false;
 RecordingInputDiagnostic diagnostic;
};
struct RecordingDriverResult
{
 QString code;
 bool callbackQuiescent=false;
 RecordingInputDiagnostic diagnostic;
 bool ok() const noexcept{return code==QStringLiteral("ok");}
};
using RecordingInputCallback=InputDecision(*)(void*,const InputBlock&) noexcept;
class RecordingInputDriver
{
public:
 virtual ~RecordingInputDriver()=default;
 virtual RecordingDriverResult initialize() noexcept=0;
 virtual RecordingInputDevicePage enumerateInputs(std::uint32_t offset,std::uint32_t limit)=0;
 virtual RecordingDriverResult openInputOnly(const SelectedInputProfile&,RecordingInputCallback,void*) noexcept=0;
 virtual RecordingDriverResult start() noexcept=0;
 virtual RecordingDriverResult stop() noexcept=0;
 virtual RecordingDriverResult close() noexcept=0;
 virtual RecordingDriverResult terminate() noexcept=0;
};
class RecordingInputController;
class RecordingServiceHost;
class LocalInputTicket
{
public:
 LocalInputTicket()=default;
 LocalInputTicket(const LocalInputTicket&)=delete;
 LocalInputTicket& operator=(const LocalInputTicket&)=delete;
 LocalInputTicket(LocalInputTicket&&) noexcept=default;
 LocalInputTicket& operator=(LocalInputTicket&&) noexcept=default;
 explicit operator bool() const noexcept{return m_valid;}
private:
 friend class RecordingInputController;
 friend class RecordingServiceHost;
 LocalInputTicket(SelectedInputProfile,InputEpoch,std::uint64_t processCookie,std::uint64_t rootRevision);
 SelectedInputProfile m_selection;
 InputEpoch m_epoch;
 std::uint64_t m_processCookie=0,m_rootRevision=0;
 bool m_valid=false;
};
struct RecordingInputRuntimeStorage;
class InputRuntimeLease
{
public:
 InputRuntimeLease()=default;
 InputRuntimeLease(const InputRuntimeLease&)=delete;
 InputRuntimeLease& operator=(const InputRuntimeLease&)=delete;
 InputRuntimeLease(InputRuntimeLease&&) noexcept=default;
 InputRuntimeLease& operator=(InputRuntimeLease&&) noexcept=default;
 explicit operator bool() const noexcept{return bool(m_storage);}
private:
 friend class RecordingInputController;
 friend class RecordingServiceHost;
 std::shared_ptr<RecordingInputRuntimeStorage> m_storage;
 RecordingRing& ring() const noexcept;
 std::shared_ptr<void> retainOwner() const noexcept;
};
enum class RecordingControllerState : std::uint32_t {Closed,Opening,Ready,Capturing,Stopping,Quiescent,FailedRetained};
struct RecordingControllerSnapshot
{
 RecordingControllerState state=RecordingControllerState::Closed;
 QString errorCode;
 InputSnapshot input;
 std::uint32_t callbacksInFlight=0;
 bool firstCallbackSeen=false,driverInitialized=false;
 RecordingInputDiagnostic diagnostic;
};
struct RecordingControllerResult
{
 QString code;
 RecordingControllerState state=RecordingControllerState::Closed;
 bool ok=false;
};
struct RecordingTicketResult
{
 QString code;
 LocalInputTicket ticket;
 bool ok=false;
};
class RecordingInputController
{
public:
 explicit RecordingInputController(RecordingInputDriver&);
 ~RecordingInputController();
 RecordingInputController(const RecordingInputController&)=delete;
 RecordingInputController& operator=(const RecordingInputController&)=delete;
 RecordingInputDevicePage enumerateInputs(std::uint32_t offset,std::uint32_t limit);
 RecordingTicketResult arm(const SelectedInputProfile&,InputEpoch,std::uint64_t rootRevision);
 InputRuntimeLease createRuntimeLease(const SelectedInputProfile&);
 RecordingControllerResult open(LocalInputTicket&&,InputRuntimeLease&&);
 RecordingControllerResult beginCapture(InputEpoch,std::uint64_t nowMilliseconds);
 RecordingControllerResult requestStop(InputEpoch) noexcept;
 RecordingControllerSnapshot poll(std::uint64_t nowMilliseconds) noexcept;
 RecordingControllerResult closeAfterQuiescence();
 RecordingControllerSnapshot snapshot() const noexcept;
private:
 struct Impl;
 std::unique_ptr<Impl> m_impl;
};
}
#endif
