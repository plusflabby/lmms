#include "RecordingInputController.h"
#include <QtTest>
#include <algorithm>
#include <array>
#include <tuple>
#include <thread>
#include <semaphore>
#include <limits>
#include <vector>
using namespace lmms::recording;
namespace {
class FakeInputDriver final : public RecordingInputDriver
{
public:
 RecordingDriverResult initialize() noexcept override{++initializeCalls;events.push_back("initialize");return initializeResult;}
 RecordingInputDevicePage enumerateInputs(std::uint32_t,std::uint32_t) override
 {++enumerateCalls;events.push_back("enumerate");return page;}
 RecordingDriverResult openInputOnly(const SelectedInputProfile& p,RecordingInputCallback cb,void* context) noexcept override
 {++openCalls;events.push_back("open_input_only");opened=p;callback=cb;callbackContext=context;return openResult;}
 RecordingDriverResult start() noexcept override
 {++startCalls;events.push_back("start");if(deliverOnStart && callback){const float samples[]{.1f,.2f,.3f,.4f};InputBlock b{samples,4,2,0,1.0,1.0};std::ignore=callback(callbackContext,b);}return startResult;}
 RecordingDriverResult stop() noexcept override{++stopCalls;events.push_back("stop");return stopResult;}
 RecordingDriverResult close() noexcept override{++closeCalls;events.push_back("close");return closeResult;}
 RecordingDriverResult terminate() noexcept override{++terminateCalls;events.push_back("terminate");return terminateResult;}
 int initializeCalls=0,enumerateCalls=0,openCalls=0,startCalls=0,stopCalls=0,closeCalls=0,terminateCalls=0;
 SelectedInputProfile opened;RecordingInputCallback callback=nullptr;void* callbackContext=nullptr;
 RecordingDriverResult initializeResult{"ok",true},openResult{"ok",true},startResult{"ok",false},stopResult{"ok",true},closeResult{"ok",true},terminateResult{"ok",true};
 RecordingInputDevicePage page{"ok",{{"device-1","Selected microphone","WASAPI",2}},1,false};bool deliverOnStart=false;std::vector<QString> events;
};
SelectedInputProfile profile(std::uint32_t rate=48000)
{SelectedInputProfile p;p.deviceId="device-1";p.hostApiName="WASAPI";p.wasapiShared=true;p.input={rate,2,1,4096,std::uint64_t(rate)*120};return p;}
}
class RecordingInputControllerTest : public QObject
{
 Q_OBJECT
private slots:
 // Without a production consumer, the second delayed burst faults at observation 1025.
 void concurrentProducerAndDelayedConsumerKeepOrderedSessionEvidence()
 {
  FakeInputDriver driver;RecordingInputController controller(driver);controller.enumerateInputs(0,64);
  auto ticket=controller.arm(profile(),{30,1},1);auto lease=controller.createRuntimeLease(profile());
  QVERIFY(controller.open(std::move(ticket.ticket),std::move(lease)).ok);QVERIFY(controller.beginCapture({30,1},0).ok);
  std::binary_semaphore ready{0},resume{0};InputDecision decision=InputDecision::Continue;
  std::thread producer([&]{
   const float sample[]{.25f,.5f};
   for(unsigned batch=0;batch<4;++batch)
   {
    for(unsigned i=0;i<1024;++i){const auto n=batch*1024+i;const auto time=1.+double(n)/48000.;
     decision=driver.callback(driver.callbackContext,{sample,1,2,0,time,time+.01});if(decision==InputDecision::Stop)break;}
    ready.release();resume.acquire();
   }
  });
  for(unsigned batch=0;batch<4;++batch){ready.acquire();std::ignore=controller.poll(100);resume.release();}
  producer.join();std::ignore=controller.requestStop({30,1});std::ignore=controller.closeAfterQuiescence();
  const auto final=controller.snapshot();QCOMPARE(decision,InputDecision::Continue);QCOMPARE(final.input.fault,InputFault::None);
  QCOMPARE(final.observations.observations,quint64(4096));QCOMPARE(final.observations.receivedFrames,quint64(4096));
  QCOMPARE(final.observations.first.receivedFirstFrame,quint64(0));QCOMPARE(final.observations.last.receivedFirstFrame,quint64(4095));
  QCOMPARE(final.observations.frameDiscontinuities,quint64(0));QCOMPARE(final.observations.adcIntervalDiscontinuities,quint64(0));
  QCOMPARE(final.observations.epoch.projectGeneration,quint64(30));QCOMPARE(final.observations.quality,InputTimestampQuality::Observed);
 }
 void stoppedQueueIsDrainedBeforeRuntimeReleaseAndNextEpochStartsEmpty()
 {
  FakeInputDriver driver;RecordingInputController controller(driver);
  for(unsigned session=1;session<=2;++session)
  {
   controller.enumerateInputs(0,64);auto ticket=controller.arm(profile(),{31,session},1);auto lease=controller.createRuntimeLease(profile());
   QVERIFY(controller.open(std::move(ticket.ticket),std::move(lease)).ok);QVERIFY(controller.beginCapture({31,session},0).ok);
   const float sample[]{.25f,.5f};for(unsigned i=0;i<3;++i)std::ignore=driver.callback(driver.callbackContext,{sample,1,2,0,1.+i/48000.,1.01+i/48000.});
   std::ignore=controller.requestStop({31,session});std::ignore=controller.closeAfterQuiescence();
   const auto summary=controller.snapshot().observations;
   QCOMPARE(summary.observations,quint64(3));QCOMPARE(summary.epoch.sessionRevision,quint64(session));
   QCOMPARE(summary.last.receivedFirstFrame,quint64(2));
  }
 }
 void summaryRetainsClockAndHardwareDegradationWithoutClaimingCalibration()
 {
  FakeInputDriver driver;RecordingInputController controller(driver);controller.enumerateInputs(0,64);
  auto ticket=controller.arm(profile(),{32,1},1);auto lease=controller.createRuntimeLease(profile());
  QVERIFY(controller.open(std::move(ticket.ticket),std::move(lease)).ok);QVERIFY(controller.beginCapture({32,1},0).ok);
  const float sample[]{.1f,.2f};
  std::ignore=driver.callback(driver.callbackContext,{sample,1,2,0,10.,10.01});
  std::ignore=driver.callback(driver.callbackContext,{sample,1,2,0,11.,11.01});
  std::ignore=driver.callback(driver.callbackContext,{sample,1,2,0,9.,9.01});
  std::ignore=driver.callback(driver.callbackContext,{sample,1,2,0,std::numeric_limits<double>::quiet_NaN(),12.});
  std::ignore=driver.callback(driver.callbackContext,{sample,1,2,InputHostOverflow,13.,13.01});
  std::ignore=controller.requestStop({32,1});std::ignore=controller.closeAfterQuiescence();const auto s=controller.snapshot();
  QCOMPARE(s.observations.observations,quint64(5));QCOMPARE(s.observations.unavailableTimestamps,quint64(1));
  QCOMPARE(s.observations.clockRegressions,quint64(1));QCOMPARE(s.observations.adcIntervalDiscontinuities,quint64(2));
  QCOMPARE(s.observations.hostStatusBlocks,quint64(1));QCOMPARE(s.observations.hostFlags,InputHostOverflow);
  QCOMPARE(s.observations.first.adcFirstSeconds,10.);QCOMPARE(s.observations.last.adcFirstSeconds,13.);
  QCOMPARE(s.observations.quality,InputTimestampQuality::Discontinuous);QCOMPARE(s.input.fault,InputFault::HardwareDiscontinuity);
 }
 void consumerStarvationStillLatchesExplicitOverflowAndPreservesQueuedPrefix()
 {
  FakeInputDriver driver;RecordingInputController controller(driver);controller.enumerateInputs(0,64);
  auto ticket=controller.arm(profile(),{33,1},1);auto lease=controller.createRuntimeLease(profile());
  QVERIFY(controller.open(std::move(ticket.ticket),std::move(lease)).ok);QVERIFY(controller.beginCapture({33,1},0).ok);
  const float sample[]{.1f,.2f};InputDecision decision=InputDecision::Continue;
  for(unsigned i=0;i<1025;++i)decision=driver.callback(driver.callbackContext,{sample,1,2,0,1.+i/48000.,1.01+i/48000.});
  const auto afterPoll=controller.poll(100);std::ignore=controller.requestStop({33,1});std::ignore=controller.closeAfterQuiescence();
  QCOMPARE(decision,InputDecision::Stop);QCOMPARE(afterPoll.input.fault,InputFault::ObservationOverflow);
  QCOMPARE(afterPoll.input.observationsDropped,quint64(1));QCOMPARE(afterPoll.input.applicationDroppedFrames,quint64(0));
  QCOMPARE(afterPoll.observations.observations,quint64(1024));QCOMPARE(afterPoll.observations.last.receivedFirstFrame,quint64(1023));
 }
 void producerOverlapsOwnerPollWithoutRacingRuntimeTeardown()
 {
  FakeInputDriver driver;RecordingInputController controller(driver);controller.enumerateInputs(0,64);
  auto ticket=controller.arm(profile(),{34,1},1);auto lease=controller.createRuntimeLease(profile());
  QVERIFY(controller.open(std::move(ticket.ticket),std::move(lease)).ok);QVERIFY(controller.beginCapture({34,1},0).ok);
  std::atomic<std::uint64_t> consumed{0};std::atomic_bool done{false},abort{false};std::atomic<unsigned> stopped{0};
  std::thread producer([&]{const float samples[]{.1f,.2f};for(unsigned i=0;i<20000 && !abort.load();++i){
   while(i>=consumed.load(std::memory_order_acquire)+512 && !abort.load())std::this_thread::yield();
   if(abort.load())break;const auto time=1.+i/48000.;
   if(driver.callback(driver.callbackContext,{samples,1,2,0,time,time+.01})==InputDecision::Stop){++stopped;break;}}
   done.store(true,std::memory_order_release);});
  QElapsedTimer deadline;deadline.start();while(!done.load(std::memory_order_acquire) && deadline.elapsed()<3000){
   consumed.store(controller.poll(100).observations.observations,std::memory_order_release);std::this_thread::yield();}
  abort.store(true);producer.join();std::ignore=controller.requestStop({34,1});std::ignore=controller.closeAfterQuiescence();
  const auto final=controller.snapshot();QCOMPARE(stopped.load(),0u);QCOMPARE(final.input.fault,InputFault::None);
  QCOMPARE(final.observations.observations,quint64(20000));QCOMPARE(final.observations.last.receivedFirstFrame,quint64(19999));
  QCOMPARE(final.observations.frameDiscontinuities,quint64(0));QCOMPARE(final.input.observationsDropped,quint64(0));
 }
 void observationIdentityFollowsTheAdmittedTicketRatherThanLaterPreparation()
 {
  FakeInputDriver driver;RecordingInputController controller(driver);controller.enumerateInputs(0,64);
  auto admitted=controller.arm(profile(),{35,1},1);auto later=controller.arm(profile(),{35,2},1);QVERIFY(later.ok);
  auto lease=controller.createRuntimeLease(profile());QVERIFY(controller.open(std::move(admitted.ticket),std::move(lease)).ok);
  driver.deliverOnStart=true;QVERIFY(controller.beginCapture({35,1},0).ok);
  std::ignore=controller.requestStop({35,1});std::ignore=controller.closeAfterQuiescence();const auto s=controller.snapshot();
  QCOMPARE(s.observations.epoch.sessionRevision,s.input.epoch.sessionRevision);QCOMPARE(s.observations.epoch.sessionRevision,quint64(1));
 }
 void constructionDoesNotInitializeOrEnumerateDevices()
 {
  FakeInputDriver driver;RecordingInputController controller(driver);
  QCOMPARE(driver.initializeCalls,0);QCOMPARE(driver.enumerateCalls,0);QCOMPARE(driver.openCalls,0);
  QCOMPARE(controller.snapshot().state,RecordingControllerState::Closed);
 }
 void noTicketCannotOpenOrFallBackToADefaultDevice()
 {
  FakeInputDriver driver;RecordingInputController controller(driver);const auto opened=controller.open(LocalInputTicket{},InputRuntimeLease{});
  QVERIFY(!opened.ok);QCOMPARE(opened.code,QString("no_local_ticket"));QCOMPARE(driver.openCalls,0);
  QCOMPARE(driver.enumerateCalls,0);QCOMPARE(driver.startCalls,0);
 }
 void enumerationIsExplicitAndPagedToAtMostSixtyFour()
 {
  FakeInputDriver driver;RecordingInputController controller(driver);
  const auto page=controller.enumerateInputs(0,64);QCOMPARE(page.code,QString("ok"));QCOMPARE(page.devices.size(),std::size_t(1));
  QCOMPARE(driver.initializeCalls,1);QCOMPARE(driver.enumerateCalls,1);QCOMPARE(driver.openCalls,0);
  const auto invalid=controller.enumerateInputs(0,65);QCOMPARE(invalid.code,QString("invalid_page"));QCOMPARE(driver.enumerateCalls,1);
 }
 void unsupportedRateAndNonSharedProfileAreRejectedBeforeOpen()
 {
  FakeInputDriver driver;RecordingInputController controller(driver);controller.enumerateInputs(0,64);
  QVERIFY(!controller.arm(profile(32000),{1,1},1).ok);QCOMPARE(driver.openCalls,0);
  auto exclusive=profile();exclusive.wasapiShared=false;QVERIFY(!controller.arm(exclusive,{1,2},1).ok);QCOMPARE(driver.openCalls,0);
 }
 void selectedInputOpensOnlyAfterLocalArmAndBalancedClose()
 {
  FakeInputDriver driver;RecordingInputController controller(driver);QVERIFY(controller.enumerateInputs(0,64).code=="ok");
  auto ticket=controller.arm(profile(),{7,1},4);QVERIFY(ticket.ok);QCOMPARE(driver.openCalls,0);
  auto lease=controller.createRuntimeLease(profile());QVERIFY(lease);
  auto opened=controller.open(std::move(ticket.ticket),std::move(lease));QVERIFY(opened.ok);QCOMPARE(opened.state,RecordingControllerState::Ready);
  QCOMPARE(driver.openCalls,1);QVERIFY(driver.opened.wasapiShared);QCOMPARE(driver.opened.deviceId,QString("device-1"));
  driver.deliverOnStart=true;const auto started=controller.beginCapture({7,1},100);QVERIFY(started.ok);
  QVERIFY(controller.snapshot().firstCallbackSeen);QCOMPARE(controller.snapshot().input.acceptedFrames,quint64(4));
  QVERIFY(controller.requestStop({7,1}).ok);QCOMPARE(controller.snapshot().state,RecordingControllerState::Quiescent);
  QVERIFY(controller.closeAfterQuiescence().ok);QCOMPARE(driver.closeCalls,1);QCOMPARE(driver.terminateCalls,1);
  const auto closeAt=std::find(driver.events.begin(),driver.events.end(),QString("close"));
  const auto terminateAt=std::find(driver.events.begin(),driver.events.end(),QString("terminate"));QVERIFY(closeAt<terminateAt);
 }
 void firstCallbackWatchdogStopsWithoutInventingReadiness()
 {
  FakeInputDriver driver;RecordingInputController controller(driver);controller.enumerateInputs(0,64);
  auto ticket=controller.arm(profile(),{8,1},2);auto lease=controller.createRuntimeLease(profile());
  QVERIFY(controller.open(std::move(ticket.ticket),std::move(lease)).ok);QVERIFY(controller.beginCapture({8,1},500).ok);
  const auto timedOut=controller.poll(2499);QCOMPARE(timedOut.state,RecordingControllerState::Capturing);
  const auto stopped=controller.poll(2500);QCOMPARE(stopped.state,RecordingControllerState::Quiescent);
  QVERIFY(!stopped.firstCallbackSeen);QCOMPARE(stopped.errorCode,QString("first_callback_timeout"));QCOMPARE(driver.stopCalls,1);
  QVERIFY(controller.closeAfterQuiescence().ok);
 }
 void lateCallbackAndStopFailureRetainRuntimeAndBlockRestart()
 {
  FakeInputDriver driver;driver.stopResult={"stop_failed",false};RecordingInputController controller(driver);controller.enumerateInputs(0,64);
  auto ticket=controller.arm(profile(),{9,1},2);auto lease=controller.createRuntimeLease(profile());
  QVERIFY(controller.open(std::move(ticket.ticket),std::move(lease)).ok);QVERIFY(controller.beginCapture({9,1},0).ok);
  const auto stop=controller.requestStop({9,1});QVERIFY(!stop.ok);QCOMPARE(stop.state,RecordingControllerState::FailedRetained);
  QVERIFY(!controller.arm(profile(),{9,2},2).ok);QCOMPARE(driver.openCalls,1);QCOMPARE(driver.closeCalls,0);QCOMPARE(driver.terminateCalls,0);
  QVERIFY(!controller.closeAfterQuiescence().ok);
  const float lateSample[]{.5f};InputBlock late{lateSample,1,1,0,2.,2.};
  QCOMPARE(driver.callback(driver.callbackContext,late),InputDecision::Stop);QCOMPARE(controller.snapshot().callbacksInFlight,0u);
  driver.stopResult={"ok",true};QVERIFY(controller.requestStop({9,1}).ok);QVERIFY(controller.closeAfterQuiescence().ok);
 }
 void initializationFailureIsVisibleAndNeverOpensInput()
 {
  FakeInputDriver driver;driver.initializeResult={"wasapi_unavailable",true};RecordingInputController controller(driver);
  const auto page=controller.enumerateInputs(0,64);QCOMPARE(page.code,QString("wasapi_unavailable"));
  QVERIFY(!controller.arm(profile(),{10,1},1).ok);QCOMPARE(driver.openCalls,0);QCOMPARE(driver.terminateCalls,0);
 }
 void closeAndTerminateFailuresRetainTheReferenceUntilSafeRetry()
 {
  FakeInputDriver driver;RecordingInputController controller(driver);controller.enumerateInputs(0,64);
  auto ticket=controller.arm(profile(),{11,1},2);auto lease=controller.createRuntimeLease(profile());
  QVERIFY(controller.open(std::move(ticket.ticket),std::move(lease)).ok);driver.deliverOnStart=true;
  QVERIFY(controller.beginCapture({11,1},0).ok);QVERIFY(controller.requestStop({11,1}).ok);
  driver.closeResult={"close_failed",true};QVERIFY(!controller.closeAfterQuiescence().ok);QCOMPARE(driver.terminateCalls,0);
  driver.closeResult={"ok",true};driver.terminateResult={"terminate_failed",true};
  QVERIFY(!controller.closeAfterQuiescence().ok);QCOMPARE(driver.closeCalls,2);QCOMPARE(driver.terminateCalls,1);
  driver.terminateResult={"ok",true};QVERIFY(controller.closeAfterQuiescence().ok);
  QCOMPARE(controller.snapshot().state,RecordingControllerState::Closed);QCOMPARE(driver.terminateCalls,2);
 }
};
QTEST_GUILESS_MAIN(RecordingInputControllerTest)
#include "RecordingInputControllerTest.moc"
