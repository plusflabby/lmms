#ifndef LMMS_RECORDING_WRITER_H
#define LMMS_RECORDING_WRITER_H
#include "RecordingTakeTypes.h"
#include "RecordingRing.h"
#include <cstdint>
#include <memory>
#include <span>
#include <array>
#include <QByteArray>
namespace lmms::recording {
struct EncodedRecordingWavHeader { std::array<std::uint8_t,44> bytes{}; bool valid=false; };
class RecordingWavHeader
{
public:
 static EncodedRecordingWavHeader encodePcm16(std::uint32_t sampleRate,std::uint64_t frames) noexcept;
};
enum class WriterState : std::uint32_t
{
 Idle,Starting,Ready,Writing,StorageBlocked,Closing,Sealed,Incomplete,NoTakeCreated,Exited
};
enum class WriterIoCode : std::uint32_t { Ok,WriteFailed,FlushFailed,DiskFull,SealFailed,Collision,InvalidState,IdentityChanged };
enum class WriterFault : std::uint32_t { None,InvalidInput,InputIncomplete,WriteFailure,CheckpointFailure,SealFailure,JournalFailure,CounterOverflow };
struct WriterIoResult {WriterIoCode code=WriterIoCode::Ok;std::uint32_t bytesWritten=0;};
struct WriterStartResult {QString code;WriterState state=WriterState::Idle;bool ok=false;explicit operator bool() const noexcept{return ok;}};
struct WriterSnapshot
{
 WriterState state=WriterState::Idle;
 WriterFault fault=WriterFault::None;
 std::uint64_t inputAcceptedFrames=0,acceptedFrames=0,removedFrames=0,writtenFrames=0,durableFrames=0;
 std::uint64_t applicationDroppedFrames=0,intentionallyUnrecordedFrames=0,clippedSamples=0,invalidSamples=0;
 std::uint64_t pendingFrames=0,physicalPayloadBytes=0;
 QByteArray sourceSha256;
 bool hardwareLossCountKnown=true,audioSealed=false,journalSealed=false,inputClosed=false;
};
// A capability for one unique library-created partial/journal pair. It has no
// arbitrary path, offset, reopen or replace operation. Tests may supply a
// memory sink implementing the same sequential write/seal boundary.
class RecordingWriterSink
{
public:
 virtual ~RecordingWriterSink()=default;
 virtual WriterIoCode beginTake(const TakeDescriptor&) noexcept=0;
 virtual WriterIoResult appendPayload(std::span<const std::uint8_t>) noexcept=0;
 virtual WriterIoCode appendCheckpoint(const TakeProgress&) noexcept=0;
 virtual WriterIoCode appendSealIntent(const TakeProgress&) noexcept=0;
 virtual WriterIoCode sealPcm16(std::uint32_t sampleRate,std::uint64_t frames) noexcept=0;
 virtual QByteArray sealedAudioSha256() const noexcept{return {};}
 virtual WriterIoCode appendTerminal(TakeTerminal,const TakeProgress&) noexcept=0;
 virtual void preservePartial() noexcept=0;
};
class RecordingWriter
{
public:
 RecordingWriter();
 ~RecordingWriter();
 RecordingWriter(const RecordingWriter&)=delete;
 RecordingWriter& operator=(const RecordingWriter&)=delete;
 WriterStartResult begin(TakeDescriptor,std::unique_ptr<RecordingWriterSink>,RecordingRing&);
 void requestInputClosed(InputEpoch,const InputSnapshot&) noexcept;
 void requestStorageRetry() noexcept;
 WriterSnapshot poll() const noexcept;
 WriterStartResult joinAfterExit();
private:
 struct Impl;
 std::unique_ptr<Impl> m_impl;
};
}
#endif
