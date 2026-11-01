#include "RecordingInputRuntime.h"
#include <QtTest>
#include <array>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>

namespace {
thread_local bool countAllocations = false;
thread_local unsigned allocations = 0, frees = 0;
}
void* operator new(std::size_t size)
{
 if (countAllocations) { ++allocations; }
 if (auto* pointer = std::malloc(size ? size : 1)) { return pointer; }
 throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { if (countAllocations && pointer) { ++frees; } std::free(pointer); }
void operator delete[](void* pointer) noexcept { ::operator delete(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { ::operator delete(pointer); }

using namespace lmms::recording;
namespace lmms::recording {
QDebug operator<<(QDebug debug, InputDecision value) { return debug << static_cast<std::uint32_t>(value); }
QDebug operator<<(QDebug debug, InputFault value) { return debug << static_cast<std::uint32_t>(value); }
QDebug operator<<(QDebug debug, InputStopReason value) { return debug << static_cast<std::uint32_t>(value); }
QDebug operator<<(QDebug debug, InputTimestampQuality value) { return debug << static_cast<std::uint32_t>(value); }
}
class RecordingInputRuntimeTest : public QObject
{
 Q_OBJECT
 static InputProfile profile(unsigned rate = 48000, unsigned channels = 1, unsigned selected = 0)
 { return {rate, channels, selected, 4096, std::uint64_t(rate) * 120}; }
 static InputBlock block(const float* samples, unsigned frames, unsigned channels = 1, unsigned flags = 0,
  double adc = 1., double current = 1.01)
 { return {samples, frames, channels, flags, adc, current}; }
 const InputEpoch epoch{1,1};
private slots:
 void beforeStartNeverQueuesTakeFrames()
 {
  RecordingRing ring{48000}; RecordingInputRuntime runtime(profile(), ring); const float sample=.25f;
  QCOMPARE(runtime.process(block(&sample,1)),InputDecision::Continue);
  QCOMPARE(ring.acceptedFrames(),std::uint64_t(0));
  QCOMPARE(runtime.snapshot().receivedFrames,std::uint64_t(0));
 }
 void invalidProfilesCannotBegin()
 {
  const std::array<InputProfile,5> invalid{{{32000,1,0,4096,3840000},{48000,0,0,4096,5760000},
   {48000,2,2,4096,5760000},{48000,1,0,4097,5760000},{48000,1,0,4096,5760001}}};
  for(const auto& p:invalid){RecordingRing ring{48000};RecordingInputRuntime runtime(p,ring);
   QVERIFY(!runtime.enableCapture(epoch));QCOMPARE(runtime.process({}),InputDecision::Stop);
   QCOMPARE(runtime.snapshot().fault,InputFault::InvalidProfile);QCOMPARE(ring.acceptedFrames(),std::uint64_t(0));}
 }
 void selectedChannelOnlyIncludingWideInput()
 {
  RecordingRing ring{48000};RecordingInputRuntime runtime(profile(48000,3,2),ring);QVERIFY(runtime.enableCapture(epoch));
  const float input[]{10.f,20.f,.75f,30.f,40.f,-.25f};
  QCOMPARE(runtime.process(block(input,2,3)),InputDecision::Continue);
  std::array<float,4> output{};QCOMPARE(ring.pop(output.data(),output.size()),std::size_t(2));
  QCOMPARE(output[0],.75f);QCOMPARE(output[1],-.25f);QCOMPARE(runtime.snapshot().acceptedFrames,std::uint64_t(2));
 }
 void invalidBuffersRefuseBeforeReading()
 {
  std::array<float,4097> input{};
  const std::array<InputBlock,4> invalid{{block(nullptr,32),block(input.data(),0),block(input.data(),4097),block(input.data(),1,2)}};
  for(const auto& b:invalid){RecordingRing ring{48000};RecordingInputRuntime runtime(profile(),ring);QVERIFY(runtime.enableCapture(epoch));
   QCOMPARE(runtime.process(b),InputDecision::Stop);QCOMPARE(runtime.snapshot().fault,InputFault::InvalidBuffer);
   QCOMPARE(ring.acceptedFrames(),std::uint64_t(0));QCOMPARE(runtime.snapshot().applicationDroppedFrames,std::uint64_t(b.frames));}
 }
 void configuredCallbackBoundIsEnforced()
 {
  auto p=profile();p.maxCallback=32;RecordingRing ring{48000};RecordingInputRuntime runtime(p,ring);QVERIFY(runtime.enableCapture(epoch));
  std::array<float,64> input{};QCOMPARE(runtime.process(block(input.data(),64)),InputDecision::Stop);
  QCOMPARE(runtime.snapshot().fault,InputFault::InvalidBuffer);QCOMPARE(ring.acceptedFrames(),std::uint64_t(0));
 }
 void invalidSelectedSampleRejectsWholeBlock()
 {
  RecordingRing ring{48000};RecordingInputRuntime runtime(profile(),ring);QVERIFY(runtime.enableCapture(epoch));
  const float input[]{.1f,std::numeric_limits<float>::quiet_NaN(),.3f};
  QCOMPARE(runtime.process(block(input,3)),InputDecision::Stop);QCOMPARE(runtime.snapshot().fault,InputFault::InvalidSample);
  QCOMPARE(runtime.snapshot().applicationDroppedFrames,std::uint64_t(3));QCOMPARE(runtime.snapshot().invalidSamples,std::uint64_t(1));
  QCOMPARE(ring.acceptedFrames(),std::uint64_t(0));
 }
 void invalidUnselectedChannelDoesNotAlterMono()
 {
  RecordingRing ring{48000};RecordingInputRuntime runtime(profile(48000,2,1),ring);QVERIFY(runtime.enableCapture(epoch));
  const float input[]{std::numeric_limits<float>::infinity(),.5f};
  QCOMPARE(runtime.process(block(input,1,2)),InputDecision::Continue);float output=0;QCOMPARE(ring.pop(&output,1),std::size_t(1));QCOMPARE(output,.5f);
 }
 void inputPeakUsesAcceptedSelectedSamples()
 {
  RecordingRing ring{48000};RecordingInputRuntime runtime(profile(48000,3,2),ring);QVERIFY(runtime.enableCapture(epoch));
  const float input[]{std::numeric_limits<float>::infinity(),.25f,.75f,.1f,.2f,-1.25f};
  QCOMPARE(int(runtime.process(block(input,2,3))),int(InputDecision::Continue));QCOMPARE(runtime.snapshot().inputPeak,1.25f);
 }
 void inputPeakExcludesSamplesRejectedByRingCapacity()
 {
  RecordingRing ring{48000};RecordingInputRuntime runtime(profile(),ring);QVERIFY(runtime.enableCapture(epoch));
  std::array<float,4096> full;full.fill(.25f);
  for(unsigned i=0;i<31;++i)QCOMPARE(runtime.process(block(full.data(),4096)),InputDecision::Continue);
  std::array<float,4093> almostFull;almostFull.fill(.3f);
  QCOMPARE(runtime.process(block(almostFull.data(),4093)),InputDecision::Continue);
  const float final[]{.2f,.35f,.4f,1.5f,.9f,.8f,.7f,.6f};
  QCOMPARE(runtime.process(block(final,8)),InputDecision::Stop);
  QCOMPARE(runtime.snapshot().acceptedFrames,std::uint64_t(131072));QCOMPARE(runtime.snapshot().applicationDroppedFrames,std::uint64_t(5));
  QCOMPARE(runtime.snapshot().inputPeak,.4f);
 }
 void maximum120SecondsIncludesPreroll_data()
 {
  QTest::addColumn<unsigned>("rate");QTest::addColumn<qulonglong>("accepted");QTest::addColumn<qulonglong>("received");QTest::addColumn<qulonglong>("outside");
  QTest::newRow("44100")<<44100u<<qulonglong(5292000)<<qulonglong(5292032)<<qulonglong(32);
  QTest::newRow("48000")<<48000u<<qulonglong(5760000)<<qulonglong(5763072)<<qulonglong(3072);
 }
 void maximum120SecondsIncludesPreroll()
 {
  QFETCH(unsigned,rate);QFETCH(qulonglong,accepted);QFETCH(qulonglong,received);QFETCH(qulonglong,outside);
  RecordingRing ring{rate};RecordingInputRuntime runtime(profile(rate),ring);QVERIFY(runtime.enableCapture(epoch));
  std::array<float,4096> input{},output{};InputDecision decision=InputDecision::Continue;unsigned calls=0;
  while(decision==InputDecision::Continue && calls<2000){decision=runtime.process(block(input.data(),4096,1,0,double(calls),double(calls)+.01));
   ring.pop(output.data(),output.size());InputObservation observation;while(runtime.popObservation(observation)){};++calls;}
  QCOMPARE(decision,InputDecision::Stop);const auto status=runtime.snapshot();
  QCOMPARE(status.acceptedFrames,std::uint64_t(accepted));QCOMPARE(ring.acceptedFrames(),std::uint64_t(accepted));
  QCOMPARE(status.receivedFrames,std::uint64_t(received));QCOMPARE(status.intentionallyUnrecordedFrames,std::uint64_t(outside));
  QCOMPARE(status.applicationDroppedFrames,std::uint64_t(0));QCOMPARE(status.fault,InputFault::None);QCOMPARE(status.stopReason,InputStopReason::DurationLimit);
 }
 void outsideDurationBoundaryIsNotValidatedAsRecordedAudio()
 {
  auto p=profile();p.maxTakeFrames=2;RecordingRing ring{48000};RecordingInputRuntime runtime(p,ring);QVERIFY(runtime.enableCapture(epoch));
  const float input[]{.1f,.2f,std::numeric_limits<float>::quiet_NaN()};
  QCOMPARE(runtime.process(block(input,3)),InputDecision::Stop);QCOMPARE(runtime.snapshot().acceptedFrames,std::uint64_t(2));
  QCOMPARE(runtime.snapshot().intentionallyUnrecordedFrames,std::uint64_t(1));QCOMPARE(runtime.snapshot().fault,InputFault::None);
 }
 void overflowPreservesAcceptedPrefix()
 {
  RecordingRing ring{48000};RecordingInputRuntime runtime(profile(),ring);QVERIFY(runtime.enableCapture(epoch));std::array<float,4096> input;input.fill(.25f);
  for(unsigned i=0;i<32;++i){QCOMPARE(runtime.process(block(input.data(),4096,1,0,double(i),double(i)+.01)),InputDecision::Continue);}
  input.fill(.75f);QCOMPARE(runtime.process(block(input.data(),7)),InputDecision::Stop);
  QCOMPARE(runtime.snapshot().acceptedFrames,std::uint64_t(131072));QCOMPARE(runtime.snapshot().applicationDroppedFrames,std::uint64_t(7));
  QCOMPARE(runtime.snapshot().fault,InputFault::CaptureOverrun);std::array<float,4096> output{};
  QCOMPARE(ring.pop(output.data(),output.size()),std::size_t(4096));for(float value:output){QCOMPARE(value,.25f);}
 }
 void hostOverflowPreservesValidCurrentBlockAndUnknownLoss()
 {
  RecordingRing ring{48000};RecordingInputRuntime runtime(profile(),ring);QVERIFY(runtime.enableCapture(epoch));const float input[]{.1f,.2f,.3f};
  QCOMPARE(runtime.process(block(input,3,1,InputHostOverflow)),InputDecision::Stop);QCOMPARE(runtime.snapshot().acceptedFrames,std::uint64_t(3));
  QCOMPARE(runtime.snapshot().fault,InputFault::HardwareDiscontinuity);QVERIFY(!runtime.snapshot().hardwareLossCountKnown);
  std::array<float,3> output{};QCOMPARE(ring.pop(output.data(),3),std::size_t(3));QCOMPARE(output[2],.3f);
 }
 void matchingStopPreventsFurtherWritesAndReuse()
 {
  RecordingRing ring{48000};RecordingInputRuntime runtime(profile(),ring);QVERIFY(runtime.enableCapture(epoch));float sample=.25f;
  QCOMPARE(runtime.process(block(&sample,1)),InputDecision::Continue);runtime.requestStop(epoch);
  QCOMPARE(runtime.process(block(&sample,1)),InputDecision::Stop);QCOMPARE(ring.acceptedFrames(),std::uint64_t(1));
  QCOMPARE(runtime.snapshot().stopReason,InputStopReason::Requested);QVERIFY(!runtime.enableCapture({1,2}));
 }
 void staleStopCannotCancelCurrentEpoch()
 {
  RecordingRing ring{48000};RecordingInputRuntime runtime(profile(),ring);QVERIFY(!runtime.enableCapture({0,1}));QVERIFY(runtime.enableCapture(epoch));
  runtime.requestStop({1,2});float sample=.25f;QCOMPARE(runtime.process(block(&sample,1)),InputDecision::Continue);
  QCOMPARE(runtime.snapshot().acceptedFrames,std::uint64_t(1));
 }
 void missingTimestampDoesNotClaimKnownClock()
 {
  RecordingRing ring{48000};RecordingInputRuntime runtime(profile(),ring);QVERIFY(runtime.enableCapture(epoch));float sample=.25f;
  QCOMPARE(runtime.process(block(&sample,1,1,0,std::numeric_limits<double>::quiet_NaN(),1.)),InputDecision::Continue);
  QCOMPARE(runtime.snapshot().acceptedFrames,std::uint64_t(1));QCOMPARE(runtime.snapshot().timestampQuality,InputTimestampQuality::Unavailable);
  QCOMPARE(runtime.snapshot().fault,InputFault::None);
 }
 void backwardsTimestampDegradesEvidenceWithoutInventingLostPcm()
 {
  RecordingRing ring{48000};RecordingInputRuntime runtime(profile(),ring);QVERIFY(runtime.enableCapture(epoch));float sample=.25f;
  QCOMPARE(runtime.process(block(&sample,1,1,0,10.,10.01)),InputDecision::Continue);
  QCOMPARE(runtime.process(block(&sample,1,1,0,9.,9.01)),InputDecision::Continue);
  QCOMPARE(runtime.snapshot().timestampQuality,InputTimestampQuality::Discontinuous);QCOMPARE(runtime.snapshot().acceptedFrames,std::uint64_t(2));
  QCOMPARE(runtime.snapshot().applicationDroppedFrames,std::uint64_t(0));QCOMPARE(runtime.snapshot().fault,InputFault::None);
 }
 void observationOverflowDropsNoAudioAndNeverOverwritesEvidence()
 {
  RecordingRing ring{48000};RecordingInputRuntime runtime(profile(),ring);QVERIFY(runtime.enableCapture(epoch));float input=.25f,output=0;
  for(unsigned i=0;i<1024;++i){QCOMPARE(runtime.process(block(&input,1,1,0,double(i),double(i)+.01)),InputDecision::Continue);ring.pop(&output,1);}
  QCOMPARE(runtime.process(block(&input,1,1,0,1024.,1024.01)),InputDecision::Stop);
  const auto status=runtime.snapshot();QCOMPARE(status.fault,InputFault::ObservationOverflow);QCOMPARE(status.observationsDropped,std::uint64_t(1));
  QCOMPARE(status.acceptedFrames,std::uint64_t(1025));QCOMPARE(status.applicationDroppedFrames,std::uint64_t(0));
  InputObservation observation;unsigned count=0;while(runtime.popObservation(observation)){QCOMPARE(observation.receivedFirstFrame,std::uint64_t(count));++count;}
  QCOMPARE(count,1024u);
 }
 void prematureRingLimitIsAVisibleContractFault()
 {
  RecordingRing ring{48000,RecordingRing::Capacity,2};auto p=profile();p.maxTakeFrames=4;RecordingInputRuntime runtime(p,ring);QVERIFY(runtime.enableCapture(epoch));
  const float input[]{.1f,.2f,.3f};QCOMPARE(runtime.process(block(input,3)),InputDecision::Stop);
  QCOMPARE(runtime.snapshot().fault,InputFault::RingContractMismatch);QCOMPARE(runtime.snapshot().applicationDroppedFrames,std::uint64_t(1));
 }
 void full120SecondCallbackWorkloadDoesNotAllocateOrFree()
 {
  RecordingRing ring{48000};RecordingInputRuntime runtime(profile(),ring);QVERIFY(runtime.enableCapture(epoch));
  std::array<float,4096> input{},output{};InputDecision decision=InputDecision::Continue;unsigned allocated=0,freed=0;
  for(unsigned i=0;i<1407;++i)
  {
   const auto time=1.+double(i)*4096/48000.;allocations=frees=0;countAllocations=true;
   decision=runtime.process(block(input.data(),4096,1,0,time,time+.01));countAllocations=false;
   allocated+=allocations;freed+=frees;ring.pop(output.data(),output.size());InputObservation observation;runtime.popObservation(observation);
   if(decision==InputDecision::Stop)break;
  }
  QCOMPARE(decision,InputDecision::Stop);QCOMPARE(allocated,0u);QCOMPARE(freed,0u);
  QCOMPARE(runtime.snapshot().acceptedFrames,quint64(5760000));QCOMPARE(runtime.snapshot().fault,InputFault::None);
 }
 void callbackSuccessAndFailureDoNotAllocateOrFree()
 {
  RecordingRing ring{48000};RecordingInputRuntime runtime(profile(),ring);QVERIFY(runtime.enableCapture(epoch));float sample=.25f;
  allocations=frees=0;countAllocations=true;const auto success=runtime.process(block(&sample,1));const auto failure=runtime.process(block(nullptr,1));countAllocations=false;
  QCOMPARE(success,InputDecision::Continue);QCOMPARE(failure,InputDecision::Stop);QCOMPARE(allocations,0u);QCOMPARE(frees,0u);
 }
};
QTEST_GUILESS_MAIN(RecordingInputRuntimeTest)
#include "RecordingInputRuntimeTest.moc"
