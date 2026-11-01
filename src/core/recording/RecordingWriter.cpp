#include "RecordingWriter.h"
#include "RecordingPcm16.h"
#include "RecordingTakeJournal.h"
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>

namespace lmms::recording {
EncodedRecordingWavHeader RecordingWavHeader::encodePcm16(std::uint32_t rate,std::uint64_t frames) noexcept
{
 EncodedRecordingWavHeader result;
 if((rate!=44100 && rate!=48000) || frames>std::numeric_limits<std::uint32_t>::max()/2)return result;
 const std::uint64_t payload=frames*2,total=36+payload;if(total>std::numeric_limits<std::uint32_t>::max())return result;
 auto& b=result.bytes;const auto put16=[&](std::size_t at,std::uint16_t v){b[at]=std::uint8_t(v);b[at+1]=std::uint8_t(v>>8);};
 const auto put32=[&](std::size_t at,std::uint32_t v){for(unsigned i=0;i<4;++i)b[at+i]=std::uint8_t(v>>(8*i));};
 std::memcpy(b.data(),"RIFF",4);put32(4,std::uint32_t(total));std::memcpy(b.data()+8,"WAVEfmt ",8);put32(16,16);put16(20,1);put16(22,1);
 put32(24,rate);put32(28,rate*2);put16(32,2);put16(34,16);std::memcpy(b.data()+36,"data",4);put32(40,std::uint32_t(payload));result.valid=true;return result;
}

struct RecordingWriter::Impl
{
 mutable std::mutex lock;
 std::condition_variable changed;
 std::thread worker;
 RecordingRing* ring=nullptr;
 std::unique_ptr<RecordingWriterSink> sink;
 TakeDescriptor descriptor;
 InputSnapshot completion;
 WriterSnapshot snapshot;
 std::atomic_bool finished{false};
 bool closeRequested=false,retryRequested=false;
 std::array<float,RecordingRing::MaximumCallback> floatScratch{};
 std::array<std::int16_t,RecordingRing::MaximumCallback> pcmScratch{};
 std::array<std::uint8_t,RecordingRing::MaximumCallback*2> byteScratch{};
 std::size_t pendingBytes=0,pendingOffset=0;
 std::uint64_t pendingFrames=0,lastCheckpointFrames=0,progressSequence=0;

 void update(const std::function<void(WriterSnapshot&)>& fn)
 {std::lock_guard guard(lock);fn(snapshot);changed.notify_all();}
 void setFault(WriterFault fault,WriterState state)
 {update([&](WriterSnapshot& s){s.fault=fault;s.state=state;});}
 bool inputIsComplete()
 {
  WriterSnapshot copy;InputSnapshot end;bool closed=false;
  {std::lock_guard guard(lock);copy=snapshot;end=completion;closed=closeRequested;}
  const auto accepted=ring?ring->acceptedFrames():0;
  return closed && end.epoch==descriptor.epoch && end.acceptedFrames==accepted
   && copy.removedFrames==accepted && copy.writtenFrames==accepted && copy.pendingFrames==0
   && copy.inputAcceptedFrames==accepted && end.applicationDroppedFrames==0 && end.invalidSamples==0
   && end.fault==InputFault::None && end.hardwareLossCountKnown && end.hardwareLostFrames==0
   && accepted<=descriptor.maxRawFrames;
 }
 bool drainPending()
 {
  while(pendingOffset<pendingBytes)
  {
   const auto written=sink->appendPayload(std::span<const std::uint8_t>(byteScratch.data()+pendingOffset,pendingBytes-pendingOffset));
   if(written.bytesWritten>pendingBytes-pendingOffset)
   {setFault(WriterFault::WriteFailure,WriterState::StorageBlocked);sink->preservePartial();return false;}
   pendingOffset+=written.bytesWritten;
   update([&](WriterSnapshot& s){s.physicalPayloadBytes+=written.bytesWritten;});
   if(written.code!=WriterIoCode::Ok || written.bytesWritten==0)
   {setFault(WriterFault::WriteFailure,WriterState::StorageBlocked);sink->preservePartial();return false;}
  }
  update([&](WriterSnapshot& s){s.writtenFrames+=pendingFrames;s.pendingFrames=0;});
  pendingOffset=0;pendingBytes=0;pendingFrames=0;
  update([&](WriterSnapshot& s){if(s.state==WriterState::StorageBlocked){s.fault=WriterFault::None;s.state=closeRequested?WriterState::Closing:WriterState::Writing;}});
  auto progress=takeProgress();
  if(progress.writtenFrames-lastCheckpointFrames>=descriptor.sampleRate)
  {
   if(sink->appendCheckpoint(progress)!=WriterIoCode::Ok)
   {setFault(WriterFault::CheckpointFailure,WriterState::StorageBlocked);sink->preservePartial();return false;}
   lastCheckpointFrames=progress.writtenFrames;progressSequence=progress.sequence;
   update([&](WriterSnapshot& s){s.durableFrames=progress.writtenFrames;});
  }
  return true;
 }
 TakeProgress takeProgress()
 {
  WriterSnapshot s;InputSnapshot end;bool closed=false;{std::lock_guard guard(lock);s=snapshot;end=completion;closed=closeRequested;}
  TakeProgress p;p.sequence=progressSequence+1;p.receivedFrames=closed?end.receivedFrames:s.inputAcceptedFrames;
  p.acceptedFrames=s.inputAcceptedFrames;p.writtenFrames=s.writtenFrames;p.applicationDroppedFrames=s.applicationDroppedFrames;
  p.intentionallyUnrecordedFrames=s.intentionallyUnrecordedFrames;p.clippedSamples=s.clippedSamples;p.invalidSamples=s.invalidSamples;
  p.hardwareLossCountKnown=s.hardwareLossCountKnown;p.hardwareLostFrames=closed?end.hardwareLostFrames:0;
  p.fault=s.fault==WriterFault::None?(closed?end.fault:InputFault::None):InputFault::RingContractMismatch;
  p.timestampQuality=closed?end.timestampQuality:InputTimestampQuality::Unavailable;return p;
 }
 bool convertNext()
 {
  const auto removed=pollCopy().removedFrames;
  if(removed>=descriptor.maxRawFrames)
  {if(ring->acceptedFrames()>removed){setFault(WriterFault::InputIncomplete,WriterState::Incomplete);sink->preservePartial();}return false;}
  const auto capacity=std::size_t(std::min<std::uint64_t>(floatScratch.size(),descriptor.maxRawFrames-removed));
  const auto frames=ring->pop(floatScratch.data(),capacity);if(!frames)return false;
  update([&](WriterSnapshot& s){s.removedFrames+=frames;s.pendingFrames+=frames;s.inputAcceptedFrames=ring->acceptedFrames();s.acceptedFrames=s.inputAcceptedFrames;s.state=WriterState::Writing;});pendingFrames=frames;
  const auto converted=RecordingPcm16::convert(floatScratch.data(),pcmScratch.data(),frames);
  if(!converted.valid){setFault(WriterFault::InvalidInput,WriterState::Incomplete);sink->preservePartial();return false;}
  if(!RecordingPcm16::encodeLittleEndian(pcmScratch.data(),byteScratch.data(),frames))
  {setFault(WriterFault::InvalidInput,WriterState::Incomplete);sink->preservePartial();return false;}
  pendingBytes=frames*2;pendingOffset=0;update([&](WriterSnapshot& s){s.clippedSamples+=converted.clipped;});
  return true;
 }
 void finish()
 {
  WriterSnapshot copy;{std::lock_guard guard(lock);copy=snapshot;}
  if(copy.state==WriterState::StorageBlocked || copy.state==WriterState::Incomplete){sink->preservePartial();return;}
  if(!inputIsComplete())
  {setFault(WriterFault::InputIncomplete,WriterState::Incomplete);sink->preservePartial();return;}
  if(!copy.writtenFrames)
  {update([](WriterSnapshot& s){s.state=WriterState::NoTakeCreated;});sink->preservePartial();return;}
  auto finalProgress=takeProgress();
  if(finalProgress.writtenFrames!=finalProgress.acceptedFrames || sink->appendCheckpoint(finalProgress)!=WriterIoCode::Ok)
  {setFault(WriterFault::CheckpointFailure,WriterState::Incomplete);sink->preservePartial();return;}
  progressSequence=finalProgress.sequence;update([&](WriterSnapshot& s){s.durableFrames=s.writtenFrames;});
  auto sealIntent=finalProgress;sealIntent.sequence=progressSequence+1;
  if(sink->appendSealIntent(sealIntent)!=WriterIoCode::Ok)
  {setFault(WriterFault::JournalFailure,WriterState::Incomplete);sink->preservePartial();return;}
  progressSequence=sealIntent.sequence;
  if(sink->sealPcm16(descriptor.sampleRate,copy.writtenFrames)!=WriterIoCode::Ok)
  {setFault(WriterFault::SealFailure,WriterState::Incomplete);sink->preservePartial();return;}
  const auto digest=sink->sealedAudioSha256();
  if(digest.size()!=32){setFault(WriterFault::SealFailure,WriterState::Incomplete);sink->preservePartial();return;}
  finalProgress.sourceDigestKnown=true;std::memcpy(finalProgress.sourceDigest.data(),digest.constData(),finalProgress.sourceDigest.size());
  update([&](WriterSnapshot& s){s.sourceSha256=digest;});
  update([](WriterSnapshot& s){s.audioSealed=true;});
  auto terminal=finalProgress;terminal.sequence=progressSequence+1;
  if(sink->appendTerminal(TakeTerminal::Complete,terminal)!=WriterIoCode::Ok)
  {setFault(WriterFault::JournalFailure,WriterState::Incomplete);sink->preservePartial();return;}
  update([](WriterSnapshot& s){s.journalSealed=true;s.state=WriterState::Sealed;});
 }
 void run()
 {
  const auto beginCode=sink->beginTake(descriptor);
  if(beginCode!=WriterIoCode::Ok){setFault(WriterFault::WriteFailure,WriterState::NoTakeCreated);sink->preservePartial();finished.store(true,std::memory_order_release);return;}
  update([](WriterSnapshot& s){s.state=WriterState::Ready;});
  for(;;)
  {
   bool retry=false,closed=false;{std::unique_lock guard(lock);changed.wait_for(guard,std::chrono::milliseconds(2),[&]{return retryRequested||closeRequested;});
    retry=retryRequested;retryRequested=false;closed=closeRequested;}
   auto current=pollCopy();
   if(current.state==WriterState::StorageBlocked)
   {
    if(!retry){if(closed){std::unique_lock guard(lock);changed.wait_for(guard,std::chrono::milliseconds(10),[&]{return retryRequested;});}continue;}
    if(!drainPending())continue;
   }
   else if(current.state==WriterState::Incomplete || current.state==WriterState::NoTakeCreated)
   {if(current.state==WriterState::Incomplete)sink->preservePartial();break;}
   if(pendingBytes && !drainPending())continue;
   if(convertNext()){if(!drainPending())continue;continue;}
   if(closed)
   {
    const auto after=pollCopy();if(after.removedFrames>=ring->acceptedFrames()){finish();break;}
   }
  }
  finished.store(true,std::memory_order_release);changed.notify_all();
 }
 WriterSnapshot pollCopy() const noexcept {std::lock_guard guard(lock);return snapshot;}
};

RecordingWriter::RecordingWriter():m_impl(std::make_unique<Impl>()){}
RecordingWriter::~RecordingWriter()
{if(m_impl && m_impl->worker.joinable())m_impl->worker.join();}
WriterStartResult RecordingWriter::begin(TakeDescriptor descriptor,std::unique_ptr<RecordingWriterSink> sink,RecordingRing& ring)
{
 if(!sink || !ring.isReady() || descriptor.epoch==InputEpoch{} || ring.sampleRate()!=descriptor.sampleRate
  || ring.maximumTakeFrames()<descriptor.maxRawFrames || ring.maximumTakeFrames()>std::uint64_t(descriptor.sampleRate)*120
  || (descriptor.sampleRate!=44100 && descriptor.sampleRate!=48000)
  || !descriptor.projectGeneration || !descriptor.maxRawFrames || descriptor.maxRawFrames>std::uint64_t(descriptor.sampleRate)*120
  || !RecordingWavHeader::encodePcm16(descriptor.sampleRate,0).valid
  || RecordingTakeJournalCodec::encodeHeader(descriptor).isEmpty())
 {std::lock_guard guard(m_impl->lock);m_impl->snapshot.state=WriterState::NoTakeCreated;return {QStringLiteral("invalid_take"),WriterState::NoTakeCreated,false};}
 {std::lock_guard guard(m_impl->lock);if(m_impl->worker.joinable() || m_impl->snapshot.state!=WriterState::Idle)return {QStringLiteral("already_started"),m_impl->snapshot.state,false};
  m_impl->ring=&ring;m_impl->sink=std::move(sink);m_impl->descriptor=std::move(descriptor);m_impl->snapshot.state=WriterState::Starting;
  m_impl->snapshot.hardwareLossCountKnown=true;m_impl->snapshot.inputAcceptedFrames=ring.acceptedFrames();m_impl->snapshot.acceptedFrames=ring.acceptedFrames();}
 try{m_impl->worker=std::thread([implementation=m_impl.get()]{implementation->run();});}
 catch(...){std::lock_guard guard(m_impl->lock);m_impl->snapshot.state=WriterState::NoTakeCreated;m_impl->snapshot.fault=WriterFault::WriteFailure;return {QStringLiteral("thread_start_failed"),WriterState::NoTakeCreated,false};}
 return {QStringLiteral("ok"),WriterState::Starting,true};
}
void RecordingWriter::requestInputClosed(InputEpoch epoch,const InputSnapshot& completion) noexcept
{
 std::lock_guard guard(m_impl->lock);
 if(epoch!=m_impl->descriptor.epoch || completion.epoch!=epoch){m_impl->snapshot.fault=WriterFault::InvalidInput;m_impl->snapshot.state=WriterState::Incomplete;m_impl->changed.notify_all();return;}
 m_impl->completion=completion;m_impl->closeRequested=true;m_impl->snapshot.inputAcceptedFrames=completion.acceptedFrames;m_impl->snapshot.acceptedFrames=completion.acceptedFrames;
 m_impl->snapshot.applicationDroppedFrames=completion.applicationDroppedFrames;m_impl->snapshot.intentionallyUnrecordedFrames=completion.intentionallyUnrecordedFrames;
 m_impl->snapshot.invalidSamples=completion.invalidSamples;m_impl->snapshot.hardwareLossCountKnown=completion.hardwareLossCountKnown;
 m_impl->changed.notify_all();
}
void RecordingWriter::requestStorageRetry() noexcept {std::lock_guard guard(m_impl->lock);m_impl->retryRequested=true;m_impl->changed.notify_all();}
WriterSnapshot RecordingWriter::poll() const noexcept {return m_impl->pollCopy();}
WriterStartResult RecordingWriter::joinAfterExit()
{
 if(!m_impl->finished.load(std::memory_order_acquire))return {QStringLiteral("not_exited"),poll().state,false};
 if(m_impl->worker.joinable())m_impl->worker.join();return {QStringLiteral("ok"),poll().state,true};
}
}
