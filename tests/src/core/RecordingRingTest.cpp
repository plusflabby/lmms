#include "RecordingRing.h"
#include <QtTest>
#include <array>
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>
namespace {
thread_local bool trackAllocation=false;
thread_local unsigned allocations=0,frees=0;
}
void* operator new(std::size_t size){if(trackAllocation)++allocations;if(auto* p=std::malloc(size?size:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t size){return ::operator new(size);}
void operator delete(void* ptr) noexcept{if(trackAllocation && ptr)++frees;std::free(ptr);}
void operator delete[](void* ptr) noexcept{::operator delete(ptr);}
void operator delete(void* ptr,std::size_t) noexcept{::operator delete(ptr);}
void operator delete[](void* ptr,std::size_t) noexcept{::operator delete(ptr);}
using namespace lmms::recording;
class RecordingRingTest : public QObject
{
 Q_OBJECT
 std::unique_ptr<RecordingRing> ring;
 static std::vector<float> interleaved(std::size_t frames,int channels,int selected,float start=0)
 {
  std::vector<float> input(frames*std::size_t(channels));
  for(std::size_t i=0;i<frames;++i)for(int channel=0;channel<channels;++channel)
   input[i*std::size_t(channels)+std::size_t(channel)]=channel==selected?start+float(i):100000.f+float(i*std::size_t(channels)+std::size_t(channel));
  return input;
 }
private slots:
 void init(){ring=std::make_unique<RecordingRing>(48000);}
 void cleanup(){ring.reset();}
 void capacityProvidesTwoSecondsAndTwoMaximumCallbacks()
 {
  QCOMPARE(ring->capacityFrames(),131072u);QVERIFY(ring->capacityFrames()>=2u*48000u+2u*4096u);
  RecordingRing rate44(44100);QVERIFY(rate44.capacityFrames()>=2u*44100u+2u*4096u);
 }
 void wraparoundPreservesMonoFrameOrder()
 {
  std::size_t expected=0;
  for(int block=0;block<31;++block){auto first=interleaved(4096,2,1,float(expected));QVERIFY(ring->push(first.data(),4096,2,1).valid);expected+=4096;}
  std::vector<float> out(70000);std::size_t offset=0;
  while(offset<out.size()){const auto count=ring->pop(out.data()+offset,std::min<std::size_t>(4096,out.size()-offset));QVERIFY(count);offset+=count;}
  for(std::size_t i=0;i<out.size();++i)QCOMPARE(out[i],float(i));
  std::array<float,4096> oldTail{};std::size_t oldRemaining=expected-out.size();
  while(oldRemaining){const auto count=ring->pop(oldTail.data(),std::min(oldTail.size(),oldRemaining));QVERIFY(count);for(std::size_t i=0;i<count;++i)QCOMPARE(oldTail[i],float(expected-oldRemaining+i));oldRemaining-=count;}
  auto second=interleaved(4096,2,1,float(expected));QVERIFY(ring->push(second.data(),4096,2,1).valid);
  std::vector<float> tail(4096);QCOMPARE(ring->pop(tail.data(),tail.size()),tail.size());
  for(std::size_t i=0;i<tail.size();++i)QCOMPARE(tail[i],float(expected+i));
 }
 void splitPushesAndPopsPreserveInterleavedChannelSelection()
 {
  auto block=interleaved(1000,2,1,200);QVERIFY(ring->push(block.data(),333,2,1).valid);QVERIFY(ring->push(block.data()+333*2,667,2,1).valid);
  std::array<float,512> first{};QCOMPARE(ring->pop(first.data(),first.size()),first.size());
  for(std::size_t i=0;i<first.size();++i)QCOMPARE(first[i],200.f+float(i));
  std::array<float,512> second{};QCOMPARE(ring->pop(second.data(),second.size()),size_t(488));
  for(std::size_t i=0;i<488;++i)QCOMPARE(second[i],712.f+float(i));
  QCOMPARE(ring->pop(second.data(),second.size()),size_t(0));
 }
 void onlyRequestedMonoChannelIsCaptured()
 {
  auto stereo=interleaved(32,2,1,10);QVERIFY(ring->push(stereo.data(),32,2,1).valid);std::array<float,32> out{};
  QCOMPARE(ring->pop(out.data(),out.size()),out.size());for(std::size_t i=0;i<out.size();++i)QCOMPARE(out[i],10.f+float(i));
 }
 void invalidInputShapeDoesNotMoveQueue()
 {
  auto block=interleaved(32,2,0);
  QVERIFY(!ring->push(nullptr,32,2,0).valid);QVERIFY(!ring->push(block.data(),0,2,0).valid);
  QVERIFY(!ring->push(block.data(),32,0,0).valid);QVERIFY(!ring->push(block.data(),32,3,0).valid);
  QVERIFY(!ring->push(block.data(),32,2,2).valid);QVERIFY(!ring->push(block.data(),4097,2,0).valid);
  std::array<float,64> out{};QCOMPARE(ring->pop(out.data(),out.size()),size_t(0));
 }
 void fullRingCountsEveryDroppedFrameWithoutSilentInsertion()
 {
  std::array<float,4096> input{};input.fill(3.5f);
  for(int i=0;i<32;++i){const auto pushed=ring->push(input.data(),4096,1,0);QVERIFY(pushed.valid);QCOMPARE(pushed.acceptedFrames,4096u);QCOMPARE(pushed.droppedFrames,0u);}
  const auto overflow=ring->push(input.data(),777,1,0);QVERIFY(overflow.valid);QCOMPARE(overflow.acceptedFrames,0u);QCOMPARE(overflow.droppedFrames,777u);
  QCOMPARE(ring->droppedFrames(),777u);std::array<float,4096> output{};QCOMPARE(ring->pop(output.data(),output.size()),output.size());
  for(const auto sample:output)QCOMPARE(sample,3.5f);
  QCOMPARE(ring->droppedFrames(),777u);
 }
 void partialSpaceAcceptsPrefixAndCountsOnlyTail()
 {
  std::array<float,4096> full{};full.fill(1.f);for(int i=0;i<31;++i)QVERIFY(ring->push(full.data(),4096,1,0).valid);
  QVERIFY(ring->push(full.data(),3072,1,0).valid);QVERIFY(ring->push(full.data(),1024,1,0).valid); // 131,072 frames occupied
  std::array<float,512> pop{};QCOMPARE(ring->pop(pop.data(),512),size_t(512));
  std::array<float,1024> next{};for(std::size_t i=0;i<next.size();++i)next[i]=float(i+2);
  const auto result=ring->push(next.data(),1024,1,0);QCOMPARE(result.acceptedFrames,512u);QCOMPARE(result.droppedFrames,512u);
  std::array<float,4096> older{};std::size_t oldRemaining=ring->capacityFrames()-512;
  while(oldRemaining){const auto count=ring->pop(older.data(),std::min(older.size(),oldRemaining));QVERIFY(count);for(std::size_t i=0;i<count;++i)QCOMPARE(older[i],1.f);oldRemaining-=count;}
  std::array<float,512> accepted{};QCOMPARE(ring->pop(accepted.data(),accepted.size()),accepted.size());
  for(std::size_t i=0;i<accepted.size();++i)QCOMPARE(accepted[i],float(i+2));
 }
 void configuredTakeLimitStopsAtExactFrameBoundary()
 {
  RecordingRing limited(48000,131072,17);std::array<float,32> input{};input.fill(5.f);
  const auto first=limited.push(input.data(),16,1,0);QCOMPARE(first.acceptedFrames,16u);QVERIFY(!first.limitReached);
  const auto last=limited.push(input.data()+16,4,1,0);QCOMPARE(last.acceptedFrames,1u);QVERIFY(last.limitReached);QCOMPARE(last.droppedFrames,0u);
  QCOMPARE(limited.acceptedFrames(),17u);const auto exhausted=limited.push(input.data(),1,1,0);QVERIFY(exhausted.valid);QVERIFY(exhausted.limitReached);QCOMPARE(exhausted.acceptedFrames,0u);
 }
 void oversizedRequestedCapacityIsRejectedBeforeAllocation()
 {
  RecordingRing tooSmall(48000,65536);QVERIFY(!tooSmall.isReady());
  RecordingRing notPowerOfTwo(48000,131071);QVERIFY(!notPowerOfTwo.isReady());
  RecordingRing tooLarge(48000,262144);QVERIFY(!tooLarge.isReady());
  RecordingRing invalidRate(96000);QVERIFY(!invalidRate.isReady());
 }
 void checkedFrameCounterDoesNotWrap()
 {
  std::uint64_t next=99;const auto max=std::numeric_limits<std::uint64_t>::max();
  QVERIFY(RecordingRing::checkedCounterAdvance(max-2,2,next));QCOMPARE(next,max);
  next=77;QVERIFY(!RecordingRing::checkedCounterAdvance(max,1,next));QCOMPARE(next,77u);
  QVERIFY(RecordingRing::checkedCounterAdvance(0,0,next));QCOMPARE(next,0u);
 }
 void pushAndPopDoNotAllocateOrFree()
 {
  std::array<float,256> input{};std::array<float,256> output{};allocations=0;frees=0;trackAllocation=true;
  bool valid=true;for(int i=0;i<256;++i){if(!ring->push(input.data(),input.size(),1,0).valid || ring->pop(output.data(),output.size())!=output.size())valid=false;}
  trackAllocation=false;QVERIFY(valid);QCOMPARE(allocations,0u);QCOMPARE(frees,0u);
 }
 void producerAndWriterPreservePublishedMonoOrder()
 {
  constexpr int blocks=64000/256;std::array<float,256> input{},output{};std::atomic_bool valid{true},done{false};
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
  std::thread producer([&]{for(int block=0;block<blocks;++block){for(int i=0;i<256;++i)input[std::size_t(i)]=float(block*256+i);const auto result=ring->push(input.data(),input.size(),1,0);if(!result.valid || result.acceptedFrames!=256 || result.droppedFrames)valid=false;}done=true;});
  for(int block=0;block<blocks;++block){std::size_t count=0;while(count<output.size()){count+=ring->pop(output.data()+count,output.size()-count);if(count<output.size()){if(done || std::chrono::steady_clock::now()>deadline){valid=false;break;}std::this_thread::yield();}}if(count!=output.size())break;for(int i=0;i<256;++i)if(output[std::size_t(i)]!=float(block*256+i))valid=false;}
  producer.join();QVERIFY(valid.load());QCOMPARE(ring->acceptedFrames(),std::uint64_t(blocks*256));QCOMPARE(ring->droppedFrames(),0u);
 }
 void resetIsSafeOnlyAfterEndpointsQuiesce()
 {
  std::array<float,16> input{};input.fill(7.f);QVERIFY(ring->push(input.data(),16,1,0).valid);
  QVERIFY(!ring->resetAfterQuiescence());std::array<float,16> output{};QCOMPARE(ring->pop(output.data(),output.size()),output.size());
  for(const auto value:output)QCOMPARE(value,7.f);
  QVERIFY(ring->resetAfterQuiescence());QCOMPARE(ring->acceptedFrames(),0u);QCOMPARE(ring->droppedFrames(),0u);
  QCOMPARE(ring->pop(output.data(),output.size()),size_t(0));
 }
};
QTEST_APPLESS_MAIN(RecordingRingTest)
#include "RecordingRingTest.moc"
