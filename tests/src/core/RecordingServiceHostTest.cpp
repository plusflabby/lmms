#include "RecordingServiceHost.h"
#include "RecordingServiceClock.h"
#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QFile>
#include <QCryptographicHash>
#include <mutex>
#include <array>
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
 RecordingDriverResult close() noexcept override{++closes;return closeResult;}
 RecordingDriverResult terminate() noexcept override{++terminates;return terminateResult;}
 int initializes=0,enumerations=0,opens=0,starts=0,stops=0,closes=0,terminates=0;
 bool deliverOnStart=true;
 RecordingDriverResult stopResult{"ok",true};RecordingInputCallback callback=nullptr;void* callbackContext=nullptr;
 RecordingDriverResult closeResult{"ok",true};
 RecordingDriverResult terminateResult{"ok",true};
};
struct ReceiptStorage {QByteArray bytes;bool fail=false;int calls=0;};
class ReceiptSink final : public RecordingReceiptSink
{
public:
 explicit ReceiptSink(ReceiptStorage& value):storage(value){}
 bool appendAndFlush(const QByteArray& bytes) noexcept override
 {++storage.calls;if(storage.fail)return false;storage.bytes+=bytes;return true;}
 ReceiptStorage& storage;
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
 // Removing the controller's observation drain must stop this workload at block 1025.
 void full120SecondWorkloadFinalizesWithoutObservationOverflow_data()
 {QTest::addColumn<unsigned>("blockFrames");QTest::newRow("4096")<<4096u;QTest::newRow("256")<<256u;}
 void full120SecondWorkloadFinalizesWithoutObservationOverflow()
 {
  QFETCH(unsigned,blockFrames);QTemporaryDir temp;QVERIFY(temp.isValid());
  auto library=RecordingLibrary::authorizeLocalRoot(temp.path());QVERIFY(library.library);
  auto d=descriptor();auto files=library.library->createUniqueTake({d.projectId,1},{d.takeId,"synthetic120"},48000,5760000);
  QVERIFY(files.sink);QVERIFY(files.receipt);
  auto driver=std::make_unique<FakeDriver>();auto* observed=driver.get();observed->deliverOnStart=false;
  RecordingServiceHost host(std::move(driver));host.enumerateInputs(0,64);QVERIFY(host.arm(profile(),{1,1},3).ok);
  QVERIFY(host.startTake(d,std::move(files.sink),0,std::move(files.receipt)).ok);
  QElapsedTimer deadline;deadline.start();
  while(deadline.elapsed()<3000 && observed->starts==0){std::ignore=host.poll(0);QTest::qWait(1);}
  std::array<float,4096> samples{};samples.fill(.125f);std::uint64_t received=0;InputDecision decision=InputDecision::Continue;
  while(observed->starts && received<5760000 && decision==InputDecision::Continue)
  {
   for(unsigned burst=0;burst<8 && received<5760000 && decision==InputDecision::Continue;++burst)
   {
    const double time=1.+double(received)/48000.;InputBlock block{samples.data(),blockFrames,1,0,time,time+.01};
    decision=observed->callback(observed->callbackContext,block);received+=blockFrames;
   }
   deadline.restart();
   while(deadline.elapsed()<3000 && host.poll(100).writer.writtenFrames<std::min<std::uint64_t>(received,5760000))QThread::usleep(100);
   if(deadline.elapsed()>=3000)break;
  }
  std::ignore=host.requestStop({1,1});deadline.restart();
  while(deadline.elapsed()<3000 && !host.quiescent()){std::ignore=host.poll(100);QTest::qWait(1);}
  const auto final=host.poll(100);QVERIFY(host.quiescent());
  qInfo("workload: received=%llu accepted=%llu fault=%u observationsDropped=%llu",final.input.input.receivedFrames,final.input.input.acceptedFrames,unsigned(final.input.input.fault),final.input.input.observationsDropped);
  QCOMPARE(final.input.input.fault,InputFault::None);QCOMPARE(final.input.input.acceptedFrames,quint64(5760000));
  QCOMPARE(final.writer.writtenFrames,quint64(5760000));QCOMPARE(final.writer.durableFrames,quint64(5760000));
  QCOMPARE(final.input.input.observationsDropped,quint64(0));QCOMPARE(final.writer.state,WriterState::Sealed);
  QCOMPARE(final.input.input.stopReason,InputStopReason::DurationLimit);
  QCOMPARE(final.input.input.receivedFrames,blockFrames==4096?quint64(5763072):quint64(5760000));
  QCOMPARE(final.input.input.intentionallyUnrecordedFrames,blockFrames==4096?quint64(3072):quint64(0));
  QFile audio(temp.path()+"/"+d.projectId+"/take-"+d.takeId+".wav");QVERIFY(audio.open(QIODevice::ReadOnly));
  const auto wav=audio.readAll();QCOMPARE(wav.size(),11520044);QCOMPARE(wav.mid(0,4),QByteArray("RIFF"));
  const auto le=[&](int at,int bytes){quint32 v=0;for(int i=0;i<bytes;++i)v|=quint32(quint8(wav[at+i]))<<(8*i);return v;};
  QCOMPARE(le(20,2),1u);QCOMPARE(le(22,2),1u);QCOMPARE(le(24,4),48000u);QCOMPARE(le(34,2),16u);QCOMPARE(le(40,4),11520000u);
  QFile receipt(temp.path()+"/"+d.projectId+"/take-"+d.takeId+".diagnostics.jsonl");QVERIFY(receipt.open(QIODevice::ReadOnly));
  const auto record=QJsonDocument::fromJson(receipt.readAll()).object();QVERIFY(record["audio_complete"].toBool());
  const auto timing=record["observations"].toObject();
  QCOMPARE(timing["blocks"].toString(),blockFrames==4096?QString("1407"):QString("22500"));
  QCOMPARE(timing["received_frames"].toString(),blockFrames==4096?QString("5763072"):QString("5760000"));
  QCOMPARE(timing["frame_discontinuities"].toString(),QString("0"));
  QCOMPARE(timing["first"].toObject()["first_frame"].toString(),QString("0"));
  QCOMPARE(timing["first"].toObject()["adc_seconds"].toDouble(),1.);
  QCOMPARE(timing["last"].toObject()["first_frame"].toString(),blockFrames==4096?QString("5758976"):QString("5759744"));
  QCOMPARE(timing["session_revision"].toString(),QString("1"));QVERIFY(!timing["calibrated"].toBool(true));
  QCOMPARE(timing["adc_interval_discontinuities"].toString(),QString("0"));
  QCOMPARE(record["counts"].toObject()["observations_dropped"].toString(),QString("0"));
  QCOMPARE(record["wav_sha256"].toString(),QString::fromLatin1(QCryptographicHash::hash(wav,QCryptographicHash::Sha256).toHex()));
 }
 void observationStarvationRetainsRealPartialInsteadOfSealingAFaultedTake()
 {
  QTemporaryDir temp;QVERIFY(temp.isValid());auto library=RecordingLibrary::authorizeLocalRoot(temp.path());QVERIFY(library.library);
  const auto d=descriptor();auto files=library.library->createUniqueTake({d.projectId,1},{d.takeId,"starved"},48000,5760000);
  QVERIFY(files.sink);QVERIFY(files.receipt);auto driver=std::make_unique<FakeDriver>();auto* observed=driver.get();observed->deliverOnStart=false;
  RecordingServiceHost host(std::move(driver));host.enumerateInputs(0,64);QVERIFY(host.arm(profile(),{1,1},3).ok);
  QVERIFY(host.startTake(d,std::move(files.sink),0,std::move(files.receipt)).ok);QElapsedTimer timer;timer.start();
  while(timer.elapsed()<3000 && observed->starts==0){std::ignore=host.poll(0);QTest::qWait(1);}
  const float sample=.25f;if(observed->callback)for(unsigned i=0;i<1025;++i)
   std::ignore=observed->callback(observed->callbackContext,{&sample,1,1,0,1.+i/48000.,1.01+i/48000.});
  std::ignore=host.requestStop({1,1});timer.restart();
  while(timer.elapsed()<3000 && !host.quiescent()){std::ignore=host.poll(100);QTest::qWait(1);}
  QVERIFY(host.quiescent());const auto final=host.poll(100);QCOMPARE(final.writer.state,WriterState::Incomplete);
  QCOMPARE(final.input.input.fault,InputFault::ObservationOverflow);QCOMPARE(final.writer.writtenFrames,quint64(1025));
  const auto stem=temp.path()+"/"+d.projectId+"/take-"+d.takeId;QVERIFY(!QFile::exists(stem+".wav"));
  QFile partial(stem+".partial");QVERIFY(partial.open(QIODevice::ReadOnly));QCOMPARE(partial.size(),qint64(2094));
  QFile receipt(stem+".diagnostics.jsonl");QVERIFY(receipt.open(QIODevice::ReadOnly));const auto record=QJsonDocument::fromJson(receipt.readAll()).object();
  QVERIFY(!record["audio_complete"].toBool());QCOMPARE(record["counts"].toObject()["observations_dropped"].toString(),QString("1"));
  QCOMPARE(record["observations"].toObject()["blocks"].toString(),QString("1024"));
 }
 void rearmingResetsLiveCountersWithoutChangingPriorReceipt()
 {
  ReceiptStorage receipt;auto driver=std::make_unique<FakeDriver>();auto* observed=driver.get();RecordingServiceHost host(std::move(driver));
  host.enumerateInputs(0,64);QVERIFY(host.arm(profile(),{1,1},3).ok);
  QVERIFY(host.startTake(descriptor(),std::make_unique<MemorySink>(),0,std::make_unique<ReceiptSink>(receipt)).ok);
  QElapsedTimer timer;timer.start();while(timer.elapsed()<3000 && observed->starts==0){std::ignore=host.poll(std::uint64_t(timer.elapsed()));QTest::qWait(1);}
  QVERIFY(host.requestStop({1,1}).ok);timer.restart();
  while(timer.elapsed()<3000 && !host.quiescent()){std::ignore=host.poll(std::uint64_t(timer.elapsed()));QTest::qWait(1);}
  QVERIFY(host.quiescent());const auto prior=receipt.bytes;QVERIFY(!prior.isEmpty());
  host.enumerateInputs(0,64);QVERIFY(host.arm(profile(),{1,2},3).ok);const auto armed=host.poll(400);QVERIFY(host.disarm().ok);
  QCOMPARE(armed.writer.writtenFrames,quint64(0));QCOMPARE(armed.input.input.receivedFrames,quint64(0));
  QCOMPARE(armed.receiptRecords,0);QCOMPARE(receipt.bytes,prior);QCOMPARE(observed->starts,1);QCOMPARE(observed->terminates,2);
 }
 void realLibraryReceiptMatchesImmutableWavAndTerminalCounts()
 {
  QTemporaryDir temp;QVERIFY(temp.isValid());auto library=RecordingLibrary::authorizeLocalRoot(temp.path());QVERIFY(library.library);
  const auto d=descriptor();const ProjectIdentity project{d.projectId,1};const TakeIdentity take{d.takeId,"receipt integration"};
  auto files=library.library->createUniqueTake(project,take,48000,5760000);QVERIFY(files.sink);QVERIFY(files.receipt);
  auto driver=std::make_unique<FakeDriver>();auto* observed=driver.get();RecordingServiceHost host(std::move(driver));
  host.enumerateInputs(0,64);QVERIFY(host.arm(profile(),{1,1},3).ok);
  QVERIFY(host.startTake(d,std::move(files.sink),0,std::move(files.receipt)).ok);
  QElapsedTimer timer;timer.start();while(timer.elapsed()<3000 && observed->starts==0){std::ignore=host.poll(std::uint64_t(timer.elapsed()));QTest::qWait(1);}
  QVERIFY(host.requestStop({1,1}).ok);timer.restart();
  while(timer.elapsed()<3000 && !host.quiescent()){std::ignore=host.poll(std::uint64_t(timer.elapsed()));QTest::qWait(1);}
  QVERIFY(host.quiescent());const auto stem=temp.path()+"/"+d.projectId+"/take-"+d.takeId;
  QFile audio(stem+".wav");QVERIFY(audio.open(QIODevice::ReadOnly));const auto pcm=audio.readAll();QCOMPARE(pcm.size(),46);
  QFile receipt(stem+".diagnostics.jsonl");QVERIFY(receipt.open(QIODevice::ReadOnly));const auto document=QJsonDocument::fromJson(receipt.readAll()).object();
  QCOMPARE(document["wav_sha256"].toString(),QString::fromLatin1(QCryptographicHash::hash(pcm,QCryptographicHash::Sha256).toHex()));
  QCOMPARE(document["counts"].toObject()["written"].toString(),QString("1"));QVERIFY(document["audio_complete"].toBool());
  QVERIFY(document["lifecycle_closed"].toBool());QCOMPARE(host.poll(100).receiptCode,QString("receipt_saved"));
 }
 void completedHostRetainsWriterCountsAndPersistsAfterCloseAndTerminate()
 {
  ReceiptStorage receipt;auto driver=std::make_unique<FakeDriver>();auto* observed=driver.get();RecordingServiceHost host(std::move(driver));
  host.enumerateInputs(0,64);QVERIFY(host.arm(profile(),{1,1},3).ok);
  QVERIFY(host.startTake(descriptor(),std::make_unique<MemorySink>(),0,std::make_unique<ReceiptSink>(receipt)).ok);
  QElapsedTimer timer;timer.start();while(timer.elapsed()<3000 && observed->starts==0){std::ignore=host.poll(std::uint64_t(timer.elapsed()));QTest::qWait(1);}
  QVERIFY(host.requestStop({1,1}).ok);timer.restart();
  while(timer.elapsed()<3000 && !host.quiescent()){std::ignore=host.poll(std::uint64_t(timer.elapsed()));QTest::qWait(1);}
  const auto final=host.poll(100);QVERIFY(host.quiescent());QCOMPARE(final.writer.writtenFrames,quint64(1));
  QCOMPARE(final.receiptCode,QString("receipt_saved"));QCOMPARE(receipt.calls,1);
  const auto obj=QJsonDocument::fromJson(receipt.bytes).object();QCOMPARE(obj["event"].toString(),QString("closed"));
  QVERIFY(obj["lifecycle_closed"].toBool());QCOMPARE(obj["counts"].toObject()["written"].toString(),QString("1"));
  QCOMPARE(observed->closes,1);QCOMPARE(observed->terminates,1);
 }
 void closeFailureIsRetainedOnceAndSuccessfulRetryAppendsAClosure_data()
 {QTest::addColumn<bool>("terminateFailure");QTest::newRow("close")<<false;QTest::newRow("terminate")<<true;}
 void closeFailureIsRetainedOnceAndSuccessfulRetryAppendsAClosure()
 {
  QFETCH(bool,terminateFailure);ReceiptStorage receipt;auto driver=std::make_unique<FakeDriver>();auto* observed=driver.get();
  if(terminateFailure)observed->terminateResult={"terminate_failed",true};else observed->closeResult={"close_failed",true};
  RecordingServiceHost host(std::move(driver));host.enumerateInputs(0,64);QVERIFY(host.arm(profile(),{1,1},3).ok);
  QVERIFY(host.startTake(descriptor(),std::make_unique<MemorySink>(),0,std::make_unique<ReceiptSink>(receipt)).ok);
  QElapsedTimer timer;timer.start();while(timer.elapsed()<3000 && observed->starts==0){std::ignore=host.poll(std::uint64_t(timer.elapsed()));QTest::qWait(1);}
  QVERIFY(host.requestStop({1,1}).ok);timer.restart();
  while(timer.elapsed()<3000 && observed->closes==0){std::ignore=host.poll(std::uint64_t(timer.elapsed()));QTest::qWait(1);}
  for(int i=0;i<4;++i)std::ignore=host.poll(100+i);
  const auto failedBytes=receipt.bytes;const bool retained=!host.quiescent();
  const auto initialTerminates=observed->terminates;observed->closeResult={"ok",true};observed->terminateResult={"ok",true};timer.restart();
  while(timer.elapsed()<3000 && !host.quiescent()){std::ignore=host.poll(200+std::uint64_t(timer.elapsed()));QTest::qWait(1);}
  QVERIFY(host.quiescent());QVERIFY(retained);QVERIFY(!failedBytes.isEmpty());
  QCOMPARE(failedBytes.count('\n'),1);QCOMPARE(receipt.bytes.count('\n'),2);
  QCOMPARE(QJsonDocument::fromJson(failedBytes).object()["event"].toString(),QString("retained"));
  QCOMPARE(observed->terminates,initialTerminates+1);
 }
 void activeProjectOrBackendChangeStopsAndRetainsItsTakeEvidence_data()
 {QTest::addColumn<int>("action");QTest::newRow("backend")<<0;QTest::newRow("project-authorization")<<1;QTest::newRow("local-disarm")<<2;}
 void activeProjectOrBackendChangeStopsAndRetainsItsTakeEvidence()
 {
  QFETCH(int,action);ReceiptStorage receipt;auto driver=std::make_unique<FakeDriver>();auto* observed=driver.get();RecordingServiceHost host(std::move(driver));
  host.enumerateInputs(0,64);QVERIFY(host.arm(profile(),{1,1},3).ok);
  QVERIFY(host.startTake(descriptor(),std::make_unique<MemorySink>(),0,std::make_unique<ReceiptSink>(receipt)).ok);
  QElapsedTimer timer;timer.start();while(timer.elapsed()<3000 && observed->starts==0){std::ignore=host.poll(std::uint64_t(timer.elapsed()));QTest::qWait(1);}
  const auto result=action==0?host.requestBackendChange():action==1?host.notifyAuthorizationChanged(4):host.disarm();QVERIFY(result.ok);timer.restart();
  while(timer.elapsed()<3000 && !host.quiescent()){std::ignore=host.poll(std::uint64_t(timer.elapsed()));QTest::qWait(1);}
  QVERIFY(host.quiescent());QCOMPARE(host.poll(200).writer.writtenFrames,quint64(1));QCOMPARE(receipt.calls,1);
  QCOMPARE(observed->stops,1);QCOMPARE(observed->closes,1);QCOMPARE(observed->terminates,1);
  const auto record=QJsonDocument::fromJson(receipt.bytes).object();
  QCOMPARE(record["stop_action"].toString(),action==0?QString("backend_change"):action==1?QString("project_change"):QString("disarm"));
 }
 void diagnosticWriteFailureDoesNotRetainMicrophoneOrEraseSuccessfulAudioCounts()
 {
  ReceiptStorage receipt;receipt.fail=true;auto driver=std::make_unique<FakeDriver>();auto* observed=driver.get();RecordingServiceHost host(std::move(driver));
  host.enumerateInputs(0,64);QVERIFY(host.arm(profile(),{1,1},3).ok);
  QVERIFY(host.startTake(descriptor(),std::make_unique<MemorySink>(),0,std::make_unique<ReceiptSink>(receipt)).ok);
  QElapsedTimer timer;timer.start();while(timer.elapsed()<3000 && observed->starts==0){std::ignore=host.poll(std::uint64_t(timer.elapsed()));QTest::qWait(1);}
  QVERIFY(host.requestStop({1,1}).ok);timer.restart();
  while(timer.elapsed()<3000 && !host.quiescent()){std::ignore=host.poll(std::uint64_t(timer.elapsed()));QTest::qWait(1);}
  QVERIFY(host.quiescent());const auto final=host.poll(100);QCOMPARE(final.receiptCode,QString("receipt_write_failed"));
  QCOMPARE(final.writer.writtenFrames,quint64(1));QCOMPARE(observed->closes,1);QCOMPARE(observed->terminates,1);
 }
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
