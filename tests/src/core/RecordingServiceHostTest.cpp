#include "RecordingServiceHost.h"
#include "RecordingServiceClock.h"
#include <QtTest>
#include <mutex>
#include <tuple>
using namespace lmms::recording;
namespace {
class FakeDriver final : public RecordingInputDriver
{
public:
 RecordingDriverResult initialize() noexcept override{++initializes;return {"ok",true};}
 RecordingInputDevicePage enumerateInputs(std::uint32_t,std::uint32_t) override
 {++enumerations;return {"ok",{{"mic-1","Local microphone","WASAPI",1}},1,false};}
 RecordingDriverResult openInputOnly(const SelectedInputProfile&,RecordingInputCallback cb,void* context) noexcept override
 {++opens;callback=cb;callbackContext=context;return {"ok",true};}
 RecordingDriverResult start() noexcept override
 {++starts;if(callback && deliverOnStart){const float sample[]{.2f};InputBlock block{sample,1,1,0,1.,1.};std::ignore=callback(callbackContext,block);}return {"ok",false};}
 RecordingDriverResult stop() noexcept override{++stops;return stopResult;}
 RecordingDriverResult close() noexcept override{++closes;return {"ok",true};}
 RecordingDriverResult terminate() noexcept override{++terminates;return {"ok",true};}
 int initializes=0,enumerations=0,opens=0,starts=0,stops=0,closes=0,terminates=0;
 bool deliverOnStart=true;
 RecordingDriverResult stopResult{"ok",true};RecordingInputCallback callback=nullptr;void* callbackContext=nullptr;
};
class MemorySink final : public RecordingWriterSink
{
public:
 WriterIoCode beginTake(const TakeDescriptor&) noexcept override{return WriterIoCode::Ok;}
 WriterIoResult appendPayload(std::span<const std::uint8_t> value) noexcept override
 {bytes.insert(bytes.end(),value.begin(),value.end());return {WriterIoCode::Ok,std::uint32_t(value.size())};}
 WriterIoCode appendCheckpoint(const TakeProgress&) noexcept override{return WriterIoCode::Ok;}
 WriterIoCode appendSealIntent(const TakeProgress&) noexcept override{return WriterIoCode::Ok;}
 WriterIoCode sealPcm16(std::uint32_t,std::uint64_t) noexcept override{return WriterIoCode::Ok;}
 WriterIoCode appendTerminal(TakeTerminal,const TakeProgress&) noexcept override{return WriterIoCode::Ok;}
 QByteArray sealedAudioSha256() const noexcept override{return QByteArray(32,'d');}
 void preservePartial() noexcept override{}
 std::vector<std::uint8_t> bytes;
};
SelectedInputProfile profile(){SelectedInputProfile p;p.deviceId="mic-1";p.hostApiName="WASAPI";p.wasapiShared=true;p.input={48000,1,0,4096,5760000};return p;}
TakeDescriptor descriptor(){TakeDescriptor d;d.projectId="00000000-0000-4000-8000-000000000001";d.takeId="00000000-0000-4000-8000-000000000002";d.label="Qualification";d.epoch={1,1};d.projectGeneration=1;d.maxRawFrames=5760000;d.sampleRate=48000;return d;}
}
class RecordingServiceHostTest : public QObject
{
 Q_OBJECT
private slots:
 void laterPanelClockCannotCauseImmediateApplicationWatchdogTimeout()
 {
  QElapsedTimer applicationClock;applicationClock.start();QTest::qWait(2100);
  QElapsedTimer panelClock;panelClock.start();
  auto driver=std::make_unique<FakeDriver>();auto* observed=driver.get();observed->deliverOnStart=false;
  RecordingServiceHost host(std::move(driver));QCOMPARE(host.enumerateInputs(0,64).code,QString("ok"));
  QVERIFY(host.arm(profile(),{1,1},3).ok);
  QVERIFY(host.startTake(descriptor(),std::make_unique<MemorySink>(),recordingServiceMilliseconds(panelClock)).ok);
  QElapsedTimer deadline;deadline.start();
  while(deadline.elapsed()<3000 && observed->starts==0)
  {std::ignore=host.poll(recordingServiceMilliseconds(panelClock));QTest::qWait(1);}
  const auto beforeCallback=host.poll(recordingServiceMilliseconds(applicationClock));
  if(beforeCallback.input.state==RecordingControllerState::Capturing)
  {const float sample[]{.2f};InputBlock block{sample,1,1,0,1.,1.};std::ignore=observed->callback(observed->callbackContext,block);}
  const auto afterCallback=host.poll(recordingServiceMilliseconds(panelClock));
  std::ignore=host.requestShutdown(recordingServiceMilliseconds(applicationClock));deadline.restart();
  while(deadline.elapsed()<3000 && !host.quiescent())
  {std::ignore=host.poll(recordingServiceMilliseconds(applicationClock));QTest::qWait(1);}
  QVERIFY(host.quiescent());QCOMPARE(observed->starts,1);
  QCOMPARE(beforeCallback.input.state,RecordingControllerState::Capturing);
  QVERIFY(afterCallback.input.firstCallbackSeen);QCOMPARE(afterCallback.input.input.acceptedFrames,quint64(1));
  QCOMPARE(observed->closes,1);QCOMPARE(observed->terminates,1);
 }
 void hostConstructionAndUnarmedStartMakeNoDriverCalls()
 {
 auto driver=std::make_unique<FakeDriver>();auto* observed=driver.get();RecordingServiceHost host(std::move(driver));
 QCOMPARE(observed->initializes,0);QCOMPARE(observed->enumerations,0);QCOMPARE(observed->opens,0);
 const auto result=host.startTake(descriptor(),std::make_unique<MemorySink>(),0);QVERIFY(!result.ok);
 QCOMPARE(result.code,QString("local_arm_required"));QCOMPARE(observed->initializes,0);QCOMPARE(observed->opens,0);
 }
 void explicitArmAndWriterReadyPrecedeInputOnlyStart()
 {
 auto driver=std::make_unique<FakeDriver>();auto* observed=driver.get();RecordingServiceHost host(std::move(driver));
 QCOMPARE(host.enumerateInputs(0,64).code,QString("ok"));QCOMPARE(observed->initializes,1);
 QVERIFY(host.arm(profile(),{1,1},3).ok);QCOMPARE(observed->opens,0);
 const auto start=host.startTake(descriptor(),std::make_unique<MemorySink>(),10);QVERIFY(start.ok);QCOMPARE(observed->opens,0);
 QElapsedTimer elapsed;elapsed.start();RecordingHostSnapshot status;
 while(elapsed.elapsed()<3000 && observed->starts==0){status=host.poll(10+std::uint64_t(elapsed.elapsed()));QTest::qWait(1);}
 QCOMPARE(observed->opens,1);QCOMPARE(observed->starts,1);QCOMPARE(status.input.state,RecordingControllerState::Capturing);
 QVERIFY(host.requestStop({1,1}).ok);elapsed.restart();
 while(elapsed.elapsed()<3000 && !host.quiescent()){status=host.poll(20+std::uint64_t(elapsed.elapsed()));QTest::qWait(1);}
 QVERIFY(host.quiescent());QCOMPARE(observed->closes,1);QCOMPARE(observed->terminates,1);
 }
 void backendChangeRevokesArmAndClosesInputReference()
 {
  auto driver=std::make_unique<FakeDriver>();auto* observed=driver.get();RecordingServiceHost host(std::move(driver));
  QCOMPARE(host.enumerateInputs(0,64).code,QString("ok"));QVERIFY(host.arm(profile(),{2,1},7).ok);
  QVERIFY(host.poll(0).armed);QVERIFY(host.requestBackendChange().ok);
  QVERIFY(!host.poll(1).armed);QCOMPARE(observed->opens,0);QCOMPARE(observed->closes,0);QCOMPARE(observed->terminates,1);
  QCOMPARE(host.startTake(descriptor(),std::make_unique<MemorySink>(),2).code,QString("local_arm_required"));
 }
 void hungStopRetainsHostAndLateCallbackUntilSafeRetry()
 {
  auto driver=std::make_unique<FakeDriver>();auto* observed=driver.get();observed->stopResult={"stop_failed",false};
  RecordingServiceHost host(std::move(driver));QCOMPARE(host.enumerateInputs(0,64).code,QString("ok"));
  QVERIFY(host.arm(profile(),{4,1},3).ok);auto take=descriptor();take.epoch={4,1};take.projectGeneration=4;
  QVERIFY(host.startTake(take,std::make_unique<MemorySink>(),0).ok);
  QElapsedTimer timer;timer.start();while(timer.elapsed()<3000 && observed->starts==0){std::ignore=host.poll(std::uint64_t(timer.elapsed()));QTest::qWait(1);}
  QCOMPARE(observed->starts,1);QVERIFY(!host.requestStop({4,1}).ok);QVERIFY(!host.quiescent());QCOMPARE(observed->closes,0);
  const float sample[]{.75f};InputBlock late{sample,1,1,0,2.,2.};QCOMPARE(observed->callback(observed->callbackContext,late),InputDecision::Stop);
  observed->stopResult={"ok",true};QVERIFY(host.requestStop({4,1}).ok);timer.restart();
  while(timer.elapsed()<3000 && !host.quiescent()){std::ignore=host.poll(std::uint64_t(timer.elapsed()));QTest::qWait(1);}
  QVERIFY(host.quiescent());QCOMPARE(observed->closes,1);QCOMPARE(observed->terminates,1);
 }
 void shutdownBarrierDoesNotRequireAProjectModelLock()
 {
  auto driver=std::make_unique<FakeDriver>();RecordingServiceHost host(std::move(driver));std::mutex modelLock;
  modelLock.lock();const auto shutdown=host.requestShutdown();modelLock.unlock();QVERIFY(shutdown.ok);QVERIFY(host.quiescent());
 }
};
QTEST_GUILESS_MAIN(RecordingServiceHostTest)
#include "RecordingServiceHostTest.moc"
