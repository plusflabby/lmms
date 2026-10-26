#ifndef LMMS_RECORDING_SERVICE_HOST_H
#define LMMS_RECORDING_SERVICE_HOST_H
#include "RecordingInputController.h"
#include "RecordingWriter.h"
#include "RecordingLibrary.h"
#include "RecordingProjectIdentity.h"
#include <memory>
#include <optional>
namespace lmms::recording {
enum class RecordingHostState : std::uint32_t {Closed,Armed,WriterStarting,OpeningInput,Capturing,Stopping,Finalizing,FailedRetained};
struct RecordingHostResult {bool ok=false;QString code;RecordingHostState state=RecordingHostState::Closed;};
struct RecordingHostSnapshot
{
 RecordingHostState state=RecordingHostState::Closed;
 QString errorCode;
 RecordingControllerSnapshot input;
 WriterSnapshot writer;
 bool armed=false,quiescent=true;
 QString receiptCode;
 int receiptRecords=0;
};
struct RecordingLibrarySelectionResult
{
 QString code;
 ProjectIdentity project;
 std::uint64_t rootRevision=0;
 bool ok=false;
};
class RecordingServiceHost
{
public:
 explicit RecordingServiceHost(std::unique_ptr<RecordingInputDriver>);
 ~RecordingServiceHost();
 RecordingServiceHost(const RecordingServiceHost&)=delete;
 RecordingServiceHost& operator=(const RecordingServiceHost&)=delete;
 RecordingInputDevicePage enumerateInputs(std::uint32_t offset,std::uint32_t limit);
 RecordingLibrarySelectionResult selectRecordingRoot(const QString&,RecordingProjectIdentity&);
 RecordingLibrary::NewTakeResult createUniqueTake(const TakeIdentity&,std::uint32_t sampleRate,std::uint64_t maxRawFrames);
 ProjectIdentity projectIdentity() const noexcept;
 std::uint64_t rootRevision() const noexcept;
 RecordingHostResult arm(const SelectedInputProfile&,InputEpoch,std::uint64_t rootRevision);
 RecordingHostResult startTake(TakeDescriptor,std::unique_ptr<RecordingWriterSink>,std::uint64_t nowMilliseconds,
  std::unique_ptr<RecordingReceiptSink> receipt={});
 RecordingHostResult requestStop(InputEpoch);
 RecordingHostResult disarm();
 RecordingHostResult notifyAuthorizationChanged(std::uint64_t newRootRevision);
 RecordingHostResult requestBackendChange();
 RecordingHostSnapshot poll(std::uint64_t nowMilliseconds) noexcept;
 RecordingHostResult requestShutdown();
 RecordingHostResult requestShutdown(std::uint64_t nowMilliseconds);
 bool quiescent() const noexcept;
private:
 struct Impl;
 std::unique_ptr<Impl> m_impl;
};
}
#endif
