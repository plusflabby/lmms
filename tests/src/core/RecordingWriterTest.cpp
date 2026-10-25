#include "RecordingWriter.h"
#include <QtTest>
#include <QCryptographicHash>
#include <array>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <vector>
using namespace lmms::recording;
namespace {
struct MemorySinkState
{
 std::mutex lock;
 std::condition_variable changed;
 std::vector<std::uint8_t> bytes;
 std::vector<TakeProgress> checkpoints;
 unsigned maxWriteBytes=std::numeric_limits<unsigned>::max();
 std::int64_t failAfterBytes=-1;
 bool beginOk=true,checkpointOk=true,audioSealOk=true,terminalOk=true;
 bool blockNextWrite=false,writeEntered=false,releaseBlockedWrite=false;
 bool wavSealed=false,journalSealed=false,preserved=false,sealIntent=false,terminalAfterWav=true;
 unsigned sealCalls=0;
};
class MemoryWriterSink final : public RecordingWriterSink
{
public:
 explicit MemoryWriterSink(std::shared_ptr<MemorySinkState> state):m_state(std::move(state)){}
 WriterIoCode beginTake(const TakeDescriptor& descriptor) noexcept override
 {
  std::lock_guard guard(m_state->lock);m_descriptor=descriptor;
  const auto header=RecordingWavHeader::encodePcm16(descriptor.sampleRate,0);
  if(!m_state->beginOk || !header.valid)return WriterIoCode::WriteFailed;
  m_state->bytes.insert(m_state->bytes.end(),header.bytes.begin(),header.bytes.end());m_ready=true;
  m_state->changed.notify_all();return WriterIoCode::Ok;
 }
 WriterIoResult appendPayload(std::span<const std::uint8_t> data) noexcept override
 {
  try{
   std::unique_lock guard(m_state->lock);
   if(m_state->blockNextWrite){m_state->writeEntered=true;m_state->changed.notify_all();m_state->changed.wait(guard,[this]{return m_state->releaseBlockedWrite;});m_state->blockNextWrite=false;}
   const auto payloadBytes=std::int64_t(m_state->bytes.size()-44);
   if(m_state->failAfterBytes>=0 && payloadBytes>=m_state->failAfterBytes)return {WriterIoCode::DiskFull,0};
   auto count=std::min<std::size_t>(data.size(),m_state->maxWriteBytes);
   if(m_state->failAfterBytes>=0)count=std::min<std::size_t>(count,std::size_t(m_state->failAfterBytes-payloadBytes));
   m_state->bytes.insert(m_state->bytes.end(),data.begin(),data.begin()+count);return {WriterIoCode::Ok,std::uint32_t(count)};
  }catch(...){return {WriterIoCode::DiskFull,0};}
 }
 WriterIoCode appendCheckpoint(const TakeProgress& progress) noexcept override
 {
  std::lock_guard guard(m_state->lock);if(!m_state->checkpointOk)return WriterIoCode::FlushFailed;
  m_state->checkpoints.push_back(progress);return WriterIoCode::Ok;
 }
 WriterIoCode appendSealIntent(const TakeProgress& progress) noexcept override
 {std::lock_guard guard(m_state->lock);m_state->sealIntent=true;m_state->checkpoints.push_back(progress);return WriterIoCode::Ok;}
 WriterIoCode sealPcm16(std::uint32_t rate,std::uint64_t frames) noexcept override
 {
  std::lock_guard guard(m_state->lock);++m_state->sealCalls;if(!m_state->audioSealOk || m_state->bytes.size()!=44+frames*2)return WriterIoCode::SealFailed;
  const auto header=RecordingWavHeader::encodePcm16(rate,frames);if(!header.valid)return WriterIoCode::SealFailed;
  std::copy(header.bytes.begin(),header.bytes.end(),m_state->bytes.begin());m_state->wavSealed=true;return WriterIoCode::Ok;
 }
 WriterIoCode appendTerminal(TakeTerminal terminal,const TakeProgress& progress) noexcept override
 {
  std::lock_guard guard(m_state->lock);if(!m_state->terminalOk)return WriterIoCode::FlushFailed;
  m_state->terminalAfterWav=m_state->wavSealed;
  m_terminal=terminal;m_state->checkpoints.push_back(progress);m_state->journalSealed=true;return WriterIoCode::Ok;
 }
 QByteArray sealedAudioSha256() const noexcept override
 {std::lock_guard guard(m_state->lock);return m_state->wavSealed?QCryptographicHash::hash(QByteArray(reinterpret_cast<const char*>(m_state->bytes.data()),int(m_state->bytes.size())),QCryptographicHash::Sha256):QByteArray{};}
 void preservePartial() noexcept override {std::lock_guard guard(m_state->lock);m_state->preserved=true;}
private:
 std::shared_ptr<MemorySinkState> m_state;
 TakeDescriptor m_descriptor;
 TakeTerminal m_terminal=TakeTerminal::Incomplete;
 bool m_ready=false;
};
TakeDescriptor descriptor(std::uint64_t maximum=5760000)
{
 TakeDescriptor d;d.projectId=QStringLiteral("00000000-0000-4000-8000-000000000001");d.takeId=QStringLiteral("00000000-0000-4000-8000-000000000002");
 d.label=QStringLiteral("Take 1");d.epoch={7,3};d.projectGeneration=7;d.targetTick=21;d.sampleRate=48000;d.countInBars=2;d.maxRawFrames=maximum;return d;
}
InputSnapshot completion(std::uint64_t frames)
{
 InputSnapshot result;result.epoch={7,3};result.receivedFrames=frames;result.acceptedFrames=frames;return result;
}
bool waitFor(RecordingWriter& writer,WriterState wanted)
{
 QElapsedTimer timer;timer.start();
 while(timer.elapsed()<3000){if(writer.poll().state==wanted)return true;QTest::qWait(1);}
 return writer.poll().state==wanted;
}
}
class RecordingWriterTest : public QObject
{
 Q_OBJECT
private slots:
 void refusesInvalidTakeWithoutOpeningSink()
 {
  RecordingRing ring{48000,131072,5760000};auto state=std::make_shared<MemorySinkState>();auto sink=std::make_unique<MemoryWriterSink>(state);
  auto invalid=descriptor();invalid.sampleRate=32000;RecordingWriter writer;
  const auto result=writer.begin(invalid,std::move(sink),ring);
  QVERIFY(!result.ok);QCOMPARE(writer.poll().state,WriterState::NoTakeCreated);
  std::lock_guard guard(state->lock);QVERIFY(state->bytes.empty());
 }
 void opensProvisionalHeaderBeforeReadyAndNeverMakesAnEmptyTake()
 {
  RecordingRing ring{48000,131072,5760000};auto state=std::make_shared<MemorySinkState>();RecordingWriter writer;
  QVERIFY(writer.begin(descriptor(),std::make_unique<MemoryWriterSink>(state),ring).ok);QVERIFY(waitFor(writer,WriterState::Ready));
  {std::lock_guard guard(state->lock);QCOMPARE(state->bytes.size(),std::size_t(44));QVERIFY(!state->wavSealed);QVERIFY(state->checkpoints.empty());}
  writer.requestInputClosed({7,3},completion(0));QVERIFY(waitFor(writer,WriterState::NoTakeCreated));QVERIFY(writer.joinAfterExit().ok);
  const auto result=writer.poll();QVERIFY(!result.audioSealed);QCOMPARE(result.writtenFrames,std::uint64_t(0));
 }
 void pcmBytesAndHeaderMatchTheIndependentReference()
 {
  RecordingRing ring{48000,131072,5760000};const float input[]{-1.f,-.5f,0.f,.5f,1.f,1.1f};
  auto state=std::make_shared<MemorySinkState>();RecordingWriter writer;
  QVERIFY(writer.begin(descriptor(),std::make_unique<MemoryWriterSink>(state),ring).ok);QVERIFY(waitFor(writer,WriterState::Ready));
  QCOMPARE(ring.push(input,6,1,0).acceptedFrames,6u);
  auto final=completion(6);writer.requestInputClosed({7,3},final);QVERIFY(waitFor(writer,WriterState::Sealed));QVERIFY(writer.joinAfterExit().ok);
  const std::array<std::uint8_t,44> expectedHeader{
   'R','I','F','F',48,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,1,0,0x80,0xbb,0,0,0,0x77,1,0,2,0,16,0,'d','a','t','a',12,0,0,0};
  const std::array<std::uint8_t,12> expectedPcm{0,0x80,0,0xc0,0,0,0,0x40,0xff,0x7f,0xff,0x7f};
  std::lock_guard guard(state->lock);QCOMPARE(state->bytes.size(),std::size_t(56));
  QCOMPARE(std::equal(expectedHeader.begin(),expectedHeader.end(),state->bytes.begin()),true);
  QCOMPARE(std::equal(expectedPcm.begin(),expectedPcm.end(),state->bytes.begin()+44),true);
  QVERIFY(state->wavSealed);QVERIFY(state->journalSealed);
  QVERIFY(state->sealIntent);QVERIFY(state->terminalAfterWav);QCOMPARE(state->sealCalls,1u);
  const auto result=writer.poll();QCOMPARE(result.acceptedFrames,std::uint64_t(6));QCOMPARE(result.writtenFrames,std::uint64_t(6));
  QCOMPARE(result.clippedSamples,std::uint64_t(1));QCOMPARE(result.pendingFrames,std::uint64_t(0));
  QCOMPARE(result.sourceSha256.size(),32);QCOMPARE(result.sourceSha256,QCryptographicHash::hash(QByteArray(reinterpret_cast<const char*>(state->bytes.data()),int(state->bytes.size())),QCryptographicHash::Sha256));
 }
 void shortPositiveWritesAreRetriedWithoutReordering()
 {
  RecordingRing ring{48000,131072,5760000};const float input[]{-.25f,0.f,.25f,1.f};
  auto state=std::make_shared<MemorySinkState>();state->maxWriteBytes=3;RecordingWriter writer;
  QVERIFY(writer.begin(descriptor(),std::make_unique<MemoryWriterSink>(state),ring).ok);QVERIFY(waitFor(writer,WriterState::Ready));
  QCOMPARE(ring.push(input,4,1,0).acceptedFrames,4u);
  writer.requestInputClosed({7,3},completion(4));QVERIFY(waitFor(writer,WriterState::Sealed));QVERIFY(writer.joinAfterExit().ok);
  std::lock_guard guard(state->lock);QCOMPARE(state->bytes.size(),std::size_t(52));QCOMPARE(state->wavSealed,true);
 }
 void stallDoesNotBlockTheInputRingAndExactPrefixRemainsDrained()
 {
  RecordingRing ring{48000,131072,5760000};const float first[]{.1f,.2f,.3f};
  auto state=std::make_shared<MemorySinkState>();state->blockNextWrite=true;RecordingWriter writer;
  QVERIFY(writer.begin(descriptor(),std::make_unique<MemoryWriterSink>(state),ring).ok);QVERIFY(waitFor(writer,WriterState::Ready));
  QCOMPARE(ring.push(first,3,1,0).acceptedFrames,3u);
  {std::unique_lock guard(state->lock);QVERIFY(state->changed.wait_for(guard,std::chrono::seconds(3),[&]{return state->writeEntered;}));}
  const float later[]{.4f,.5f,.6f};const auto pushed=ring.push(later,3,1,0);QCOMPARE(pushed.acceptedFrames,3u);
  writer.requestInputClosed({7,3},completion(6));auto pending=writer.poll();QVERIFY(pending.state==WriterState::Writing || pending.state==WriterState::Closing);
  {std::lock_guard guard(state->lock);state->releaseBlockedWrite=true;}state->changed.notify_all();
  QVERIFY(waitFor(writer,WriterState::Sealed));QVERIFY(writer.joinAfterExit().ok);
  const auto result=writer.poll();QCOMPARE(result.acceptedFrames,std::uint64_t(6));QCOMPARE(result.writtenFrames,std::uint64_t(6));
 }
 void diskFullRetainsOnlyAnObservableIncompleteTakeUntilStorageRetry()
 {
  RecordingRing ring{48000,131072,5760000};const float input[]{-.4f,-.2f,0.f,.2f,.4f,.6f};
  auto state=std::make_shared<MemorySinkState>();state->failAfterBytes=3;RecordingWriter writer;
  QVERIFY(writer.begin(descriptor(),std::make_unique<MemoryWriterSink>(state),ring).ok);QVERIFY(waitFor(writer,WriterState::Ready));
  QCOMPARE(ring.push(input,6,1,0).acceptedFrames,6u);
  QVERIFY(waitFor(writer,WriterState::StorageBlocked));writer.requestInputClosed({7,3},completion(6));
  auto blocked=writer.poll();QCOMPARE(blocked.acceptedFrames,std::uint64_t(6));QCOMPARE(blocked.pendingFrames,std::uint64_t(6));
  QCOMPARE(blocked.fault,WriterFault::WriteFailure);QVERIFY(!blocked.audioSealed);
  {std::lock_guard guard(state->lock);state->failAfterBytes=-1;}
  writer.requestStorageRetry();QVERIFY(waitFor(writer,WriterState::Sealed));QVERIFY(writer.joinAfterExit().ok);
  const auto recovered=writer.poll();QCOMPARE(recovered.writtenFrames,std::uint64_t(6));QCOMPARE(recovered.pendingFrames,std::uint64_t(0));
  std::lock_guard guard(state->lock);QCOMPARE(state->bytes.size(),std::size_t(56));
 }
 void finalCounterMismatchOrInputFaultNeverSealsComplete()
 {
  RecordingRing ring{48000,131072,5760000};const float input[]{.1f,.2f,.3f};
  auto state=std::make_shared<MemorySinkState>();RecordingWriter writer;
  QVERIFY(writer.begin(descriptor(),std::make_unique<MemoryWriterSink>(state),ring).ok);QVERIFY(waitFor(writer,WriterState::Ready));
  QCOMPARE(ring.push(input,3,1,0).acceptedFrames,3u);
  auto final=completion(4);writer.requestInputClosed({7,3},final);QVERIFY(waitFor(writer,WriterState::Incomplete));QVERIFY(writer.joinAfterExit().ok);
  QVERIFY(!writer.poll().audioSealed);Q_UNUSED(state);
 }
 void failedInitialHeaderNeverPublishesASealedTake()
 {
  RecordingRing ring{48000,131072,5760000};auto state=std::make_shared<MemorySinkState>();state->beginOk=false;RecordingWriter writer;
  QVERIFY(writer.begin(descriptor(),std::make_unique<MemoryWriterSink>(state),ring).ok);
  QVERIFY(waitFor(writer,WriterState::NoTakeCreated));QVERIFY(writer.joinAfterExit().ok);
  QVERIFY(!writer.poll().audioSealed);QVERIFY(!writer.poll().journalSealed);std::lock_guard guard(state->lock);QVERIFY(state->preserved);
 }
 void failedTerminalJournalAfterAudioSealIsSavedUnattached()
 {
  RecordingRing ring{48000,131072,5760000};auto state=std::make_shared<MemorySinkState>();state->terminalOk=false;RecordingWriter writer;
  QVERIFY(writer.begin(descriptor(),std::make_unique<MemoryWriterSink>(state),ring).ok);QVERIFY(waitFor(writer,WriterState::Ready));
  const float sample[]{.125f};QCOMPARE(ring.push(sample,1,1,0).acceptedFrames,1u);writer.requestInputClosed({7,3},completion(1));
  QVERIFY(waitFor(writer,WriterState::Incomplete));QVERIFY(writer.joinAfterExit().ok);
  const auto result=writer.poll();QVERIFY(result.audioSealed);QVERIFY(!result.journalSealed);QCOMPARE(result.fault,WriterFault::JournalFailure);
 }
};
QTEST_GUILESS_MAIN(RecordingWriterTest)
#include "RecordingWriterTest.moc"
