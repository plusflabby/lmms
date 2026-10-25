#include "RecordingInputController.h"
#include <QtTest>
#include <algorithm>
#include <array>
#include <tuple>
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
