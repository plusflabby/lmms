#include "RecordingQualificationView.h"
#include <QComboBox>
#include <QElapsedTimer>
#include <QPushButton>
#include <QtTest>
#include <vector>
using namespace lmms::recording;
namespace {
class FakeDriver final : public RecordingInputDriver
{
public:
 RecordingDriverResult initialize() noexcept override{++initializes;return {"ok",true};}
 RecordingInputDevicePage enumerateInputs(std::uint32_t,std::uint32_t) override
 {++enumerations;return {"ok",{{"mic-1","Local microphone","WASAPI",2}},1,false};}
 RecordingDriverResult openInputOnly(const SelectedInputProfile&,RecordingInputCallback,void*) noexcept override{++opens;return {"ok",true};}
 RecordingDriverResult start() noexcept override{++starts;return {"ok",false};}
 RecordingDriverResult stop() noexcept override{++stops;return {"ok",true};}
 RecordingDriverResult close() noexcept override{++closes;return {"ok",true};}
 RecordingDriverResult terminate() noexcept override{++terminates;return {"ok",true};}
 int initializes=0,enumerations=0,opens=0,starts=0,stops=0,closes=0,terminates=0;
};
SelectedInputProfile profile(){SelectedInputProfile p;p.deviceId="mic-1";p.hostApiName="WASAPI";p.wasapiShared=true;p.input={48000,2,0,4096,5760000};return p;}
class MemorySink final : public RecordingWriterSink
{
public:
 WriterIoCode beginTake(const TakeDescriptor&) noexcept override{return WriterIoCode::Ok;}
 WriterIoResult appendPayload(std::span<const std::uint8_t> samples) noexcept override{bytes.insert(bytes.end(),samples.begin(),samples.end());return {WriterIoCode::Ok,std::uint32_t(samples.size())};}
 WriterIoCode appendCheckpoint(const TakeProgress&) noexcept override{return WriterIoCode::Ok;}
 WriterIoCode appendSealIntent(const TakeProgress&) noexcept override{return WriterIoCode::Ok;}
 WriterIoCode sealPcm16(std::uint32_t,std::uint64_t) noexcept override{return WriterIoCode::Ok;}
 WriterIoCode appendTerminal(TakeTerminal,const TakeProgress&) noexcept override{return WriterIoCode::Ok;}
 QByteArray sealedAudioSha256() const noexcept override{return QByteArray(32,'d');}
 void preservePartial() noexcept override{}
 std::vector<std::uint8_t> bytes;
};
}
class RecordingQualificationViewTest : public QObject
{
 Q_OBJECT
private slots:
 void constructionAndPanelCloseDoNotEnumerateOpenOrRearm()
 {
  auto driver=std::make_unique<FakeDriver>();auto* observed=driver.get();RecordingServiceHost host(std::move(driver));
  RecordingProjectIdentity project(nullptr);auto* view=new RecordingQualificationView(host,project);
  QCOMPARE(observed->initializes,0);QCOMPARE(observed->enumerations,0);QCOMPARE(observed->opens,0);
  QCOMPARE(view->selectedInputIndex(),-1);QCOMPARE(view->selectedChannelIndex(),-1);QCOMPARE(view->selectedSampleRate(),0);
  delete view;QCOMPARE(observed->opens,0);QCOMPARE(observed->closes,0);QCOMPARE(observed->terminates,0);
  QVERIFY(host.requestShutdown(0).ok);QCOMPARE(observed->terminates,0);
 }
 void onlyTheExplicitPickerActionEnumeratesLocalInputs()
 {
  auto driver=std::make_unique<FakeDriver>();auto* observed=driver.get();RecordingServiceHost host(std::move(driver));
  RecordingProjectIdentity project(nullptr);RecordingQualificationView view(host,project);
  QCOMPARE(observed->initializes,0);QCOMPARE(observed->enumerations,0);
  auto* picker=view.findChild<QPushButton*>("enumerateRecordingInputs");QVERIFY(picker);QTest::mouseClick(picker,Qt::LeftButton);
  QCOMPARE(observed->initializes,1);QCOMPARE(observed->enumerations,1);QCOMPARE(observed->opens,0);
  QCOMPARE(view.selectedInputIndex(),-1);QCOMPARE(view.selectedChannelIndex(),-1);QCOMPARE(view.selectedSampleRate(),0);
  QVERIFY(host.requestShutdown(0).ok);QCOMPARE(observed->closes,0);QCOMPARE(observed->terminates,1);
 }
 void closingPanelDoesNotDisarmApplicationOwnedHost()
 {
  auto driver=std::make_unique<FakeDriver>();auto* observed=driver.get();RecordingServiceHost host(std::move(driver));
  QCOMPARE(host.enumerateInputs(0,64).code,QString("ok"));QVERIFY(host.arm(profile(),{3,1},6).ok);
  RecordingProjectIdentity project(nullptr);auto* view=new RecordingQualificationView(host,project);delete view;
  QVERIFY(host.poll(0).armed);QCOMPARE(observed->opens,0);QCOMPARE(observed->closes,0);
  QVERIFY(host.requestShutdown(1).ok);QCOMPARE(observed->terminates,1);
 }
 void closingPanelDoesNotCloseAnActiveApplicationOwnedTake()
 {
  auto driver=std::make_unique<FakeDriver>();auto* observed=driver.get();RecordingServiceHost host(std::move(driver));
  QCOMPARE(host.enumerateInputs(0,64).code,QString("ok"));const auto selected=profile();QVERIFY(host.arm(selected,{4,1},7).ok);
  TakeDescriptor take;take.projectId="00000000-0000-4000-8000-000000000001";take.takeId="00000000-0000-4000-8000-000000000002";
  take.label="Qualification";take.epoch={4,1};take.projectGeneration=4;take.maxRawFrames=5760000;take.sampleRate=48000;
  QVERIFY(host.startTake(take,std::make_unique<MemorySink>(),0).ok);
  RecordingProjectIdentity project(nullptr);auto* view=new RecordingQualificationView(host,project);
  QElapsedTimer elapsed;elapsed.start();
  while(elapsed.elapsed()<3000 && observed->starts==0){std::ignore=host.poll(std::uint64_t(elapsed.elapsed()));QTest::qWait(1);}
  QCOMPARE(observed->starts,1);delete view;
  QCOMPARE(observed->closes,0);QCOMPARE(host.poll(std::uint64_t(elapsed.elapsed())).input.state,RecordingControllerState::Capturing);
  QVERIFY(!host.quiescent());
  std::ignore=host.requestShutdown(std::uint64_t(elapsed.elapsed()));elapsed.restart();
  while(elapsed.elapsed()<3000 && !host.quiescent()){std::ignore=host.poll(std::uint64_t(elapsed.elapsed()));QTest::qWait(1);}
  QVERIFY(host.quiescent());QCOMPARE(observed->closes,1);
 }
};
QTEST_MAIN(RecordingQualificationViewTest)
#include "RecordingQualificationViewTest.moc"
