#include "RecordingPortAudioInput.h"
#include <QtTest>
#include <pa_win_wasapi.h>
#include <vector>
using namespace lmms::recording;
namespace {
// Every entry is replaced: no test can initialize or enumerate a real audio device.
struct FakeApi
{
 static inline FakeApi* current=nullptr;
 QByteArray name="EarPods",message="Invalid sample rate",hostMessage="original host failure";
 PaDeviceInfo device{};PaHostApiInfo host{};PaHostErrorInfo hostError{paWASAPI,1234,nullptr};
 PaError initError=paNoError,formatError=paNoError,openError=paNoError,startError=paNoError,stopError=paNoError;
 int initializeCalls=0,formatCalls=0,openCalls=0,startCalls=0,hostErrorReads=0;
 bool initialized=false,deliver=false;double requestedRate=0;int requestedChannels=0;unsigned long wasapiFlags=0;
 PaStreamCallback* callback=nullptr;void* context=nullptr;
 FakeApi()
 {current=this;device.structVersion=2;device.name=name.constData();device.hostApi=0;device.maxInputChannels=2;
  device.defaultSampleRate=16000;device.defaultLowInputLatency=.01;host.type=paWASAPI;host.name="WASAPI";}
 RecordingPortAudioApi api()
 {
  RecordingPortAudioApi a;
  a.initialize=[]()->PaError{++current->initializeCalls;current->initialized=current->initError==0;return current->initError;};
  a.terminate=[]()->PaError{current->initialized=false;return paNoError;};
  a.deviceCount=[]()->PaDeviceIndex{return 1;};
  a.deviceInfo=[](PaDeviceIndex i)->const PaDeviceInfo*{return i==0?&current->device:nullptr;};
  a.hostApiInfo=[](PaHostApiIndex)->const PaHostApiInfo*{return &current->host;};
  a.errorText=[](PaError)->const char*{current->hostError.errorCode=9999;return current->message.constData();};
  a.lastHostError=[]()->const PaHostErrorInfo*{++current->hostErrorReads;current->hostError.errorText=current->hostMessage.constData();return &current->hostError;};
  a.formatSupported=[](const PaStreamParameters* in,const PaStreamParameters* out,double rate)->PaError
  {++current->formatCalls;current->record(in,out,rate);return current->formatError;};
  a.open=[](PaStream** stream,const PaStreamParameters* in,const PaStreamParameters* out,double rate,
   unsigned long,PaStreamFlags,PaStreamCallback* cb,void* context)->PaError
  {++current->openCalls;current->record(in,out,rate);if(current->openError)return current->openError;
   *stream=reinterpret_cast<PaStream*>(quintptr(1));current->callback=cb;current->context=context;return paNoError;};
  a.start=[](PaStream*)->PaError
  {++current->startCalls;if(current->deliver){const float samples[]{.1f,.2f,.3f,.4f};PaStreamCallbackTimeInfo times{1.,1.,0.};
    current->callback(samples,nullptr,2,&times,0,current->context);}return current->startError;};
  a.isStopped=[](PaStream*)->PaError{current->hostError.errorCode=8888;current->hostMessage="overwritten by later call";return 1;};
  a.stop=[](PaStream*)->PaError{return current->stopError;};a.close=[](PaStream*)->PaError{return paNoError;};return a;
 }
 void record(const PaStreamParameters* in,const PaStreamParameters* out,double rate)
 {Q_ASSERT(in && !out);requestedRate=rate;requestedChannels=in->channelCount;
  wasapiFlags=static_cast<const PaWasapiStreamInfo*>(in->hostApiSpecificStreamInfo)->flags;}
 SelectedInputProfile profile() const
 {SelectedInputProfile p;p.deviceId="0|"+QString::fromUtf8(device.name);p.hostApiName="WASAPI";p.wasapiShared=true;
  p.input={48000,2,1,4096,5760000};return p;}
};
InputDecision ignoreInput(void*,const InputBlock&) noexcept{return InputDecision::Continue;}
}
class RecordingPortAudioDiagnosticTest : public QObject
{
 Q_OBJECT
private slots:
 void constructionDoesNotTouchPortAudio()
 {FakeApi f;RecordingPortAudioInputDriver driver(f.api());QCOMPARE(f.initializeCalls,0);QCOMPARE(f.openCalls,0);}
 void initializationFailureHasItsOwnStage()
 {FakeApi f;f.initError=paUnanticipatedHostError;RecordingPortAudioInputDriver driver(f.api());const auto r=driver.initialize();
  QVERIFY(!r.ok());QCOMPARE(r.diagnostic.failedStage,InputDiagnosticStage::Initialize);QCOMPARE(r.diagnostic.portAudioError,-9999);
  QCOMPARE(r.diagnostic.hostErrorCode,1234L);QCOMPARE(f.openCalls,0);}
 void samePortAudioErrorIsDistinguishedAtEachStage_data()
 {QTest::addColumn<int>("stage");QTest::newRow("format")<<int(InputDiagnosticStage::FormatSupport);
  QTest::newRow("open")<<int(InputDiagnosticStage::OpenStream);QTest::newRow("start")<<int(InputDiagnosticStage::StartStream);}
 void samePortAudioErrorIsDistinguishedAtEachStage()
 {
  QFETCH(int,stage);FakeApi f;const auto expected=InputDiagnosticStage(stage);
  if(expected==InputDiagnosticStage::FormatSupport)f.formatError=paInvalidSampleRate;
  if(expected==InputDiagnosticStage::OpenStream)f.openError=paInvalidSampleRate;
  if(expected==InputDiagnosticStage::StartStream)f.startError=paInvalidSampleRate;
  RecordingPortAudioInputDriver driver(f.api());QVERIFY(driver.initialize().ok());auto r=driver.openInputOnly(f.profile(),ignoreInput,nullptr);
  if(expected==InputDiagnosticStage::StartStream){QVERIFY(r.ok());r=driver.start();}
  QVERIFY(!r.ok());QCOMPARE(r.diagnostic.failedStage,expected);QCOMPARE(r.diagnostic.portAudioError,-9997);
  QCOMPARE(r.diagnostic.description,QString("Invalid sample rate"));QCOMPARE(r.diagnostic.requestedRate,48000u);
  QCOMPARE(r.diagnostic.inputChannels,2u);QCOMPARE(r.diagnostic.selectedChannel,1u);
  QCOMPARE(r.diagnostic.maxInputChannels,2u);QCOMPARE(r.diagnostic.defaultSampleRate,16000.);
  QCOMPARE(r.diagnostic.deviceDescription,QString("EarPods"));QCOMPARE(r.diagnostic.hostApiName,QString("WASAPI"));
  QVERIFY(r.diagnostic.wasapiShared);QCOMPARE(r.diagnostic.outcome(expected),InputDiagnosticOutcome::Failed);
  QCOMPARE(r.diagnostic.outcome(InputDiagnosticStage::ResolveEndpoint),InputDiagnosticOutcome::Succeeded);
  QCOMPARE(r.diagnostic.outcome(InputDiagnosticStage::FirstCallback),InputDiagnosticOutcome::NotAttempted);
  QVERIFY(!r.diagnostic.hasHostError);QCOMPARE(f.hostErrorReads,0);
  QCOMPARE(f.requestedRate,48000.);QCOMPARE(f.requestedChannels,2);QCOMPARE(f.wasapiFlags,0ul);
  QCOMPARE(f.openCalls,expected==InputDiagnosticStage::FormatSupport?0:1);
  QCOMPARE(f.startCalls,expected==InputDiagnosticStage::StartStream?1:0);
  if(expected==InputDiagnosticStage::StartStream)QVERIFY(driver.close().ok());
  QVERIFY(driver.terminate().ok());
 }
 void localValidationNeverMislabelsPortAudioErrors_data()
 {QTest::addColumn<int>("kind");QTest::addColumn<int>("stage");
  QTest::newRow("stale endpoint")<<0<<int(InputDiagnosticStage::ResolveEndpoint);
  QTest::newRow("wrong host")<<1<<int(InputDiagnosticStage::ResolveEndpoint);
  QTest::newRow("channel count")<<2<<int(InputDiagnosticStage::ValidateChannels);
  QTest::newRow("channel index")<<3<<int(InputDiagnosticStage::ValidateChannels);
  QTest::newRow("rate policy")<<4<<int(InputDiagnosticStage::ValidateFormat);}
 void localValidationNeverMislabelsPortAudioErrors()
 {QFETCH(int,kind);QFETCH(int,stage);FakeApi f;auto p=f.profile();
  if(kind==0)p.deviceId="42|private-endpoint";
  if(kind==1)f.host.type=paDirectSound;
  if(kind==2)p.input.inputChannels=3;
  if(kind==3)p.input.selectedChannel=2;
  if(kind==4)p.input.sampleRate=32000;
  RecordingPortAudioInputDriver driver(f.api());QVERIFY(driver.initialize().ok());auto r=driver.openInputOnly(p,ignoreInput,nullptr);
  QVERIFY(!r.ok());QCOMPARE(r.diagnostic.failedStage,InputDiagnosticStage(stage));QVERIFY(!r.diagnostic.hasPortAudioError);
  QCOMPARE(f.formatCalls,0);QCOMPARE(f.openCalls,0);QVERIFY(driver.terminate().ok());}
 void hostErrorIsCapturedBeforeDescriptionAndStoppedQueriesOverwriteIt()
 {FakeApi f;f.startError=paUnanticipatedHostError;RecordingPortAudioInputDriver driver(f.api());QVERIFY(driver.initialize().ok());
  QVERIFY(driver.openInputOnly(f.profile(),ignoreInput,nullptr).ok());const auto r=driver.start();
  QCOMPARE(r.diagnostic.failedStage,InputDiagnosticStage::StartStream);QVERIFY(r.diagnostic.hasHostError);
  QCOMPARE(r.diagnostic.hostErrorCode,1234L);QCOMPARE(r.diagnostic.hostErrorDescription,QString("original host failure"));
  QCOMPARE(f.hostErrorReads,1);QVERIFY(driver.close().ok());QVERIFY(driver.terminate().ok());}
 void messagesAreBoundedAndDoNotEchoOpaqueDeviceIds()
 {FakeApi f;f.name=QByteArray(4096,'d');f.device.name=f.name.constData();f.message=QByteArray(4096,'e');
  f.formatError=paInvalidSampleRate;RecordingPortAudioInputDriver driver(f.api());QVERIFY(driver.initialize().ok());
  const auto r=driver.openInputOnly(f.profile(),ignoreInput,nullptr);QVERIFY(!r.ok());
  QVERIFY(r.diagnostic.description.size()<=160);QVERIFY(r.diagnostic.deviceDescription.size()<=96);
  const auto text=r.diagnostic.localSummary();QVERIFY(!text.isEmpty());QVERIFY(text.size()<=2048);
  QVERIFY(!text.contains(f.profile().deviceId));QVERIFY(text.contains("48000"));QVERIFY(text.contains("format_support"));
  QVERIFY(driver.terminate().ok());}
 void controllerRetainsFailureDetailsAcrossCleanup()
 {FakeApi f;f.startError=paUnanticipatedHostError;RecordingPortAudioInputDriver driver(f.api());RecordingInputController c(driver);
  QCOMPARE(c.enumerateInputs(0,64).code,QString("ok"));auto ticket=c.arm(f.profile(),{2,3},1);QVERIFY(ticket.ok);
  QVERIFY(c.open(std::move(ticket.ticket),c.createRuntimeLease(f.profile())).ok);QVERIFY(!c.beginCapture({2,3},0).ok);
  QVERIFY(c.closeAfterQuiescence().ok);const auto d=c.snapshot().diagnostic;
  QCOMPARE(d.failedStage,InputDiagnosticStage::StartStream);QCOMPARE(d.hostErrorCode,1234L);
  QCOMPARE(d.outcome(InputDiagnosticStage::OpenStream),InputDiagnosticOutcome::Succeeded);}
 void callbackTimeoutIsSeparateFromAStartedStream()
 {FakeApi f;RecordingPortAudioInputDriver driver(f.api());RecordingInputController c(driver);c.enumerateInputs(0,64);
  auto ticket=c.arm(f.profile(),{3,4},1);QVERIFY(c.open(std::move(ticket.ticket),c.createRuntimeLease(f.profile())).ok);
  QVERIFY(c.beginCapture({3,4},0).ok);const auto s=c.poll(2000);
  QCOMPARE(s.diagnostic.failedStage,InputDiagnosticStage::FirstCallback);
  QCOMPARE(s.diagnostic.outcome(InputDiagnosticStage::StartStream),InputDiagnosticOutcome::Succeeded);
  QVERIFY(!s.diagnostic.hasPortAudioError);QVERIFY(c.closeAfterQuiescence().ok);}
 void callbackAndFrameDeliveryRemainObservableAfterClose()
 {FakeApi f;f.deliver=true;RecordingPortAudioInputDriver driver(f.api());RecordingInputController c(driver);c.enumerateInputs(0,64);
  auto ticket=c.arm(f.profile(),{4,5},1);QVERIFY(c.open(std::move(ticket.ticket),c.createRuntimeLease(f.profile())).ok);
  QVERIFY(c.beginCapture({4,5},0).ok);QVERIFY(c.requestStop({4,5}).ok);QVERIFY(c.closeAfterQuiescence().ok);
  const auto s=c.snapshot();QCOMPARE(s.diagnostic.outcome(InputDiagnosticStage::FirstCallback),InputDiagnosticOutcome::Succeeded);
  QCOMPARE(s.diagnostic.receivedFrames,quint64(2));QCOMPARE(s.diagnostic.acceptedFrames,quint64(2));}
 void alreadyStoppedIsNotReportedAsAnInputFailure()
 {FakeApi f;RecordingPortAudioInputDriver driver(f.api());QVERIFY(driver.initialize().ok());
  QVERIFY(driver.openInputOnly(f.profile(),ignoreInput,nullptr).ok());QVERIFY(driver.start().ok());f.stopError=paStreamIsStopped;
  const auto result=driver.stop();QVERIFY(result.ok());QCOMPARE(result.diagnostic.failedStage,InputDiagnosticStage::None);
  QCOMPARE(result.diagnostic.outcome(InputDiagnosticStage::StopStream),InputDiagnosticOutcome::Succeeded);
  QVERIFY(driver.close().ok());QVERIFY(driver.terminate().ok());}
};
QTEST_GUILESS_MAIN(RecordingPortAudioDiagnosticTest)
#include "RecordingPortAudioDiagnosticTest.moc"
