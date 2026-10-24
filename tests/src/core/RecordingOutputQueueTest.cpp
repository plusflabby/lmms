#include "RecordingOutputQueue.h"
#include <QtTest>
#include <array>
#include <limits>
#include <memory>
#include <thread>
#include <cstdlib>
#include <new>
#include <chrono>
namespace {
thread_local bool trackMemory=false;
thread_local unsigned allocations=0, frees=0;
}
void* operator new(std::size_t size){if(trackMemory)++allocations;if(auto* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* p) noexcept {if(trackMemory && p)++frees;std::free(p);}
void operator delete[](void* p) noexcept {::operator delete(p);}
void operator delete(void* p,std::size_t) noexcept {::operator delete(p);}
void operator delete[](void* p,std::size_t) noexcept {::operator delete(p);}
using namespace lmms;
using namespace lmms::recording;

class RecordingOutputQueueTest : public QObject
{
 Q_OBJECT
 const RenderEpoch epoch{7, 1};
 static std::array<SampleFrame, 256> signal(float base)
 {
  std::array<SampleFrame, 256> result;
  for (unsigned i=0;i<result.size();++i) result[i]=SampleFrame(base+i, -base-i);
  return result;
 }
 bool put(RecordingOutputQueue& q, std::int64_t first, bool target=false, unsigned offset=0)
 {
  auto reservation=q.reservePeriod(epoch,256);
  const auto pcm=signal(float(first));
  return q.publish(std::move(reservation),pcm,{epoch,first,256,true,target,offset});
 }
private slots:
 void wraparoundPreservesStereoAndStamp()
 {
  auto q=std::make_unique<RecordingOutputQueue>(epoch);
  std::array<float,512> output{};
  for(int block=0;block<150;++block){
   QVERIFY(put(*q,block*256));OutputTransfer transfer;
   q->consume(output.data(),256,transfer);
   QCOMPARE(transfer.copiedFrames,256u);QCOMPARE(transfer.missingFrames,0u);
   for(int i=0;i<256;++i){QCOMPARE(output[2*i],float(block*256+i));QCOMPARE(output[2*i+1],-float(block*256+i));}
  }
 }
 void splitAndCombinePeriodsPreserveTargetOffset()
 {
  auto q=std::make_unique<RecordingOutputQueue>(epoch);
  QVERIFY(put(*q,0));QVERIFY(put(*q,256,true,17));QVERIFY(put(*q,512));
  std::array<float,1024> output{};OutputTransfer transfer;
  q->consume(output.data(),128,transfer);QVERIFY(!transfer.hasTarget);
  q->consume(output.data(),512,transfer);
  QCOMPARE(transfer.copiedFrames,512u);QVERIFY(transfer.hasTarget);QCOMPARE(transfer.targetOffset,145u);
  QCOMPARE(transfer.epoch,epoch);QCOMPARE(output[0],128.f);QCOMPARE(output[1022],639.f);
  q->consume(output.data(),128,transfer);QVERIFY(!transfer.hasTarget);QCOMPARE(output[0],640.f);
 }
 void fullQueueDoesNotOverwriteOrAdvance()
 {
  auto q=std::make_unique<RecordingOutputQueue>(epoch);
  for(int b=0;b<64;++b)QVERIFY(put(*q,b*256));
  QVERIFY(!q->reservePeriod(epoch,256));
  std::array<float,8192> output{};OutputTransfer transfer;
  for(int b=0;b<4;++b){q->consume(output.data(),4096,transfer);QCOMPARE(transfer.copiedFrames,4096u);QCOMPARE(output[0],float(b*4096));}
  QVERIFY(put(*q,16384));
 }
 void uncommittedDescriptorIsInvisible()
 {
  auto q=std::make_unique<RecordingOutputQueue>(epoch);auto reservation=q->reservePeriod(epoch,256);QVERIFY(reservation);
  std::array<float,512> output;output.fill(99);OutputTransfer transfer;
  q->consume(output.data(),256,transfer);QCOMPARE(transfer.copiedFrames,0u);QCOMPARE(transfer.missingFrames,256u);QCOMPARE(output[0],0.f);
  QVERIFY(q->publish(std::move(reservation),signal(1),{epoch,0,256,true,false,0}));
  q->consume(output.data(),256,transfer);QCOMPARE(output[0],1.f);
 }
 void staleEpochNeverReturnsMusicalAudio()
 {
  auto q=std::make_unique<RecordingOutputQueue>(epoch);
  QVERIFY(!q->reservePeriod({8,1},256));
  auto reservation=q->reservePeriod(epoch,256);
  QVERIFY(!q->publish(std::move(reservation),signal(1),{{8,1},0,256,true,true,0}));
  std::array<float,512> output{};OutputTransfer transfer;q->consume(output.data(),256,transfer);
  QCOMPARE(transfer.copiedFrames,0u);QVERIFY(!transfer.hasTarget);QVERIFY(put(*q,0));
 }
 void partialReadCountsExactMissingFrames()
 {
  auto q=std::make_unique<RecordingOutputQueue>(epoch);QVERIFY(put(*q,0));
  std::array<float,1024> output;output.fill(99);OutputTransfer transfer;q->consume(output.data(),512,transfer);
  QCOMPARE(transfer.copiedFrames,256u);QCOMPARE(transfer.missingFrames,256u);QCOMPARE(output[510],255.f);QCOMPARE(output[512],0.f);QCOMPARE(output[1023],0.f);
 }
 void reserveBeforeNativeRender()
 {
  auto q=std::make_unique<RecordingOutputQueue>(epoch);
  {auto abandoned=q->reservePeriod(epoch,256);QVERIFY(abandoned);QVERIFY(!q->reservePeriod(epoch,256));}
  auto r=q->reservePeriod(epoch,256);QVERIFY(r);
  auto moved=std::move(r);QVERIFY(!r);QVERIFY(moved);
  QVERIFY(q->publish(std::move(moved),signal(1),{epoch,0,256,true,false,0}));
 }
 void counterOverflowFailsClosed()
 {
  auto q=std::make_unique<RecordingOutputQueue>(epoch);auto r=q->reservePeriod(epoch,256);
  QVERIFY(!q->publish(std::move(r),signal(1),{epoch,std::numeric_limits<std::int64_t>::max()-127,256,true,false,0}));
  QVERIFY(!q->reservePeriod(epoch,0));QVERIFY(!q->reservePeriod(epoch,257));QVERIFY(put(*q,0));
 }
 void malformedStampAndDuplicateOnsetRejectWithoutPublication()
 {
  auto q=std::make_unique<RecordingOutputQueue>(epoch);
  auto r=q->reservePeriod(epoch,256);QVERIFY(!q->publish(std::move(r),signal(1),{epoch,0,256,true,true,256}));
  QVERIFY(put(*q,0,true,5));
  auto duplicate=q->reservePeriod(epoch,256);QVERIFY(!q->publish(std::move(duplicate),signal(1),{epoch,256,256,true,true,0}));
  QVERIFY(put(*q,256));
 }
 void boundedStorageAndLockFreePublication()
 {
  QVERIFY(sizeof(RenderStamp)<=64);QVERIFY(sizeof(RecordingOutputQueue)<220000);
  QVERIFY(std::atomic<std::uint64_t>::is_always_lock_free);
  QVERIFY(std::atomic<std::uint32_t>::is_always_lock_free);
 }
 void descriptorCapacityAndInvalidPayloadCannotLeakAudio()
 {
  auto q=std::make_unique<RecordingOutputQueue>(epoch);
  const std::array<SampleFrame,1> pcm{SampleFrame(3,-4)};
  for(int i=0;i<1024;++i){auto r=q->reservePeriod(epoch,1);QVERIFY(r);QVERIFY(q->publish(std::move(r),pcm,{epoch,i,1,true,false,0}));}
  QVERIFY(!q->reservePeriod(epoch,1));
  std::array<float,2048> out{};OutputTransfer transfer;q->consume(out.data(),1024,transfer);QCOMPARE(transfer.copiedFrames,1024u);
  auto r=q->reservePeriod(epoch,2);QVERIFY(r);QVERIFY(!q->publish(std::move(r),pcm,{epoch,1024,2,true,false,0}));
  q->consume(out.data(),32,transfer);QCOMPARE(transfer.copiedFrames,0u);QCOMPARE(out[0],0.f);
 }
 void primingCannotPublishMusicalTarget()
 {
  auto q=std::make_unique<RecordingOutputQueue>(epoch);
  auto bad=q->reservePeriod(epoch,256);QVERIFY(!q->publish(std::move(bad),signal(1),{epoch,0,256,false,true,0}));
  std::array<SampleFrame,256> silence{};
  auto r=q->reservePeriod(epoch,256);QVERIFY(q->publish(std::move(r),silence,{epoch,0,256,false,false,0}));
  QVERIFY(put(*q,0,true,0));std::array<float,1024> output{};OutputTransfer t;q->consume(output.data(),512,t);
  QVERIFY(t.hasTarget);QCOMPARE(t.targetOffset,256u);QCOMPARE(output[0],0.f);QCOMPARE(output[514],1.f);
 }
 void transferDoesNotAllocateOrFree()
 {
  auto q=std::make_unique<RecordingOutputQueue>(epoch);const auto pcm=signal(10);std::array<float,512> out{};OutputTransfer t;
  allocations=0;frees=0;trackMemory=true;
  auto r=q->reservePeriod(epoch,256);const bool published=q->publish(std::move(r),pcm,{epoch,0,256,true,true,7});
  q->consume(out.data(),256,t);q->consume(out.data(),256,t);
  trackMemory=false;
  QVERIFY(published);QCOMPARE(allocations,0u);QCOMPARE(frees,0u);
 }
 void producerConsumerVisibility()
 {
  auto q=std::make_unique<RecordingOutputQueue>(epoch);std::atomic_bool valid{true};
  QVERIFY(put(*q,0));std::array<float,512> initial{};OutputTransfer first;q->consume(initial.data(),256,first);QCOMPARE(first.copiedFrames,256u);
  std::atomic_bool done{false};const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
  std::thread producer([&]{for(int block=0;block<20000;++block){ProducerReservation r;while(!(r=q->reservePeriod(epoch,256))){if(std::chrono::steady_clock::now()>deadline){valid=false;done=true;return;}std::this_thread::yield();}if(!q->publish(std::move(r),signal(float(block*256)),{epoch,block*256,256,true,false,0})){valid=false;done=true;return;}}done=true;});
  std::array<float,512> output{};OutputTransfer transfer;
  for(int block=0;block<20000;++block){do{q->consume(output.data(),256,transfer);if(!transfer.copiedFrames){if(done || std::chrono::steady_clock::now()>deadline){valid=false;break;}std::this_thread::yield();}}while(!transfer.copiedFrames);if(!transfer.copiedFrames)break;if(transfer.copiedFrames!=256 || output[0]!=float(block*256) || output[511]!=-float(block*256+255))valid=false;}
  producer.join();QVERIFY(valid.load());
 }
};
QTEST_APPLESS_MAIN(RecordingOutputQueueTest)
#include "RecordingOutputQueueTest.moc"
