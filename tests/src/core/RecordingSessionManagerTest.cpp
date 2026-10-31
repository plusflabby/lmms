#include "RecordingSessionManager.h"
#include "RecordingAttachment.h"
#include "AudioImportPolicy.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "AudioEngine.h"
#include "AudioDevice.h"
#include "Song.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QDomDocument>
#include <QDir>
#include <QFile>
#include <QUuid>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <windows.h>
#include <functional>
#include <tuple>
using namespace lmms::recording;
using namespace lmms;
namespace lmms::recording {
class RecordingRecorderView
{
public:
	static ManualPlacementApproval approve(const QString& take, quint64 generation, quint64 revision, qint64 sourceIn, qint64 target)
	{
		ManualPlacementApproval value;
		value.takeId = take;
		value.expectedProjectGeneration = generation;
		value.placementRevision = revision;
		value.sourceInTicks = sourceIn;
		value.targetTick = target;
		return value;
	}
};
}
namespace {
QString uuid(){return QUuid::createUuid().toString(QUuid::WithoutBraces);}
class SyntheticOutput final : public AudioDevice
{
public:
	explicit SyntheticOutput(int rate):AudioDevice(2,Engine::audioEngine()){setSampleRate(rate);}
private:
	void startProcessingImpl() override{}
	void stopProcessingImpl() override{}
};
class Input final : public RecordingInputDriver
{
public:
 int initializes=0,enumerates=0,opens=0,starts=0,stops=0,closes=0,terminates=0;
 unsigned startupFrames=4;bool deliver=true;
 RecordingInputCallback callback=nullptr;void* context=nullptr;
 std::function<void()> onOpen,onStop;
 RecordingDriverResult stopResult{"ok",true},closeResult{"ok",true};
 std::vector<QString>* events=nullptr;
 double clock=1.;
 RecordingDriverResult initialize() noexcept override{++initializes;return {"ok",true};}
 RecordingInputDevicePage enumerateInputs(std::uint32_t,std::uint32_t) override{++enumerates;return {"ok",{{"mic-1","Synthetic input","WASAPI",1}},1,false};}
 RecordingDriverResult openInputOnly(const SelectedInputProfile&,RecordingInputCallback cb,void* value) noexcept override
 {++opens;callback=cb;context=value;if(events)events->push_back("open");if(onOpen)onOpen();return {"ok",true};}
 RecordingDriverResult start() noexcept override{++starts;if(events)events->push_back("input_start");if(deliver)send(startupFrames);return {"ok",false};}
 RecordingDriverResult stop() noexcept override{++stops;if(onStop)onStop();return stopResult;}
 RecordingDriverResult close() noexcept override{++closes;return closeResult;}
 RecordingDriverResult terminate() noexcept override{++terminates;return {"ok",true};}
 void send(unsigned frames=4)
 {std::array<float,4096> pcm{};pcm.fill(.25f);if(callback){std::ignore=callback(context,{pcm.data(),frames,1,0,clock,clock+.01});clock+=double(frames)/48000.;if(events)events->push_back("input_frames");}}
};
std::unique_ptr<Input> make(Input*& out){auto p=std::make_unique<Input>();out=p.get();return p;}
class Transport final : public RecordingTransportControl
{
public:
 RecordingTransportSnapshot world;
 int seeks=0,plays=0,stops=0;quint64 serial=0;
 bool failPlay=false,inCommand=false;
 std::function<void()> onSeek,onPlay;
 std::vector<QString>* events=nullptr;
 Transport(){world.available=true;world.projectGeneration=1;}
 RecordingTransportSnapshot snapshot() const override{return world;}
 RecordingTransportResult seekSong(qint64 tick) override
 {++seeks;inCommand=true;if(events)events->push_back("seek");world.songTick=tick;++world.seekRevision;if(onSeek)onSeek();inCommand=false;return {true,"ok"};}
 RecordingTransportResult playSong() override
 {++plays;inCommand=true;if(events)events->push_back("play");if(failPlay){inCommand=false;return {false,"song_start_failed"};}
  world.playing=true;world.songMode=true;++world.stateRevision;RecordingPlaybackOwnership owner{++serial,world.stateRevision};if(onPlay)onPlay();inCommand=false;return {true,"ok",owner};}
 RecordingTransportResult stopOwned(RecordingPlaybackOwnership owner) override
 {if(!world.playing && !world.paused)return {true,"already_stopped"};
  if(!world.songMode || (world.stateRevision!=owner.stateRevision && !(world.paused && world.stateRevision==owner.stateRevision+1)))return {true,"ownership_lost"};
  ++stops;world.playing=world.paused=false;world.songMode=false;++world.stateRevision;return {true,"stopped"};}
};
struct Fixture
{
 QTemporaryDir directory;
 RecordingProjectIdentity identity{lmms::Engine::getSong()};
 Input* input=nullptr;
 RecordingServiceHost host{make(input)};
 Transport transport;
 RecordingSessionManager manager{host,transport};
 std::vector<QString> events;
 Fixture(){const auto root=host.selectRecordingRoot(directory.path(),identity);transport.world.projectId=root.project.id;input->events=&events;transport.events=&events;}
 ~Fixture(){input->stopResult={"ok",true};input->closeResult={"ok",true};std::ignore=manager.requestShutdown();std::ignore=host.requestShutdown(0);
  QElapsedTimer timer;timer.start();while(timer.elapsed()<3000 && (!host.quiescent() || !manager.quiescent())){std::ignore=manager.poll(100);std::ignore=host.poll(100);QTest::qWait(1);}
  if(!host.quiescent())qFatal("Synthetic fixture failed to quiesce; owners must not be freed live");}
 bool arm(quint64 maximum=5760000)
 {if(host.enumerateInputs(0,64).code!="ok")return false;SelectedInputProfile profile;profile.deviceId="mic-1";profile.hostApiName="WASAPI";profile.wasapiShared=true;profile.input={48000,1,0,4096,maximum};
  auto ticket=host.localArmTicket(profile,transport.world.projectGeneration);if(!ticket.ok){qInfo()<<"local ticket failed"<<ticket.code;return false;}
  const auto result=manager.arm(std::move(ticket.ticket));if(!result.ok)qInfo()<<"arm failed"<<result.code<<"terminated selection refs"<<input->terminates;return result.ok;}
 PrepareTakeRequest request(int bars=1) const{return {768,"Synthetic vocal",bars,false,transport.world.projectGeneration};}
 RecordingReply prepare(int bars=1){return manager.prepare(request(bars));}
 RecordingReply start(const RecordingReply& p,QString id={}){return manager.start({p.status.preparedId,transport.world.projectGeneration,id.isEmpty()?uuid():id});}
 void until(const std::function<bool()>& done,quint64 now=100)
 {QElapsedTimer timer;timer.start();while(timer.elapsed()<3000 && !done()){std::ignore=manager.poll(now);QTest::qWait(1);}}
 QString folder() const{return directory.path()+"/"+host.projectIdentity().id;}
 QStringList files() const{return QDir(folder()).entryList(QDir::Files);}
};
}
class RecordingSessionManagerTest : public QObject
{
 Q_OBJECT
private slots:
 void initTestCase(){lmms::Engine::init(true);lmms::Engine::audioEngine()->audioDev()->stopProcessing();}
 void cleanupTestCase(){lmms::Engine::destroy();}
 void prepareDisarmedNoDriverOrFile()
 {
  Fixture f;const auto before=f.files();const auto caps=f.manager.capabilities();QVERIFY(caps.requiresLocalArm);QVERIFY(!caps.sampleAccuracyVerified);
  std::ignore=f.manager.status();QCOMPARE(f.manager.takes().code,QString("ok"));const auto p=f.prepare();QVERIFY(p.ok);QVERIFY(!p.status.armed);
  QCOMPARE(f.start(p).code,QString("not_armed"));QCOMPARE(f.input->initializes,0);QCOMPARE(f.input->opens,0);QCOMPARE(f.files(),before);QCOMPARE(f.transport.plays,0);
 }
 void localArmTicketOnlyTransfersWithinItsOwner()
 {
  Fixture a,b;QVERIFY(a.host.enumerateInputs(0,64).code=="ok");SelectedInputProfile p;p.deviceId="mic-1";p.hostApiName="WASAPI";p.wasapiShared=true;p.input={48000,1,0,4096,5760000};
  auto ticket=a.host.localArmTicket(p,1);QVERIFY(ticket.ok);QVERIFY(!b.manager.arm(std::move(ticket.ticket)).ok);QCOMPARE(a.input->opens,0);QCOMPARE(b.input->opens,0);
  QVERIFY(a.arm());QCOMPARE(a.input->opens,0);QCOMPARE(a.input->starts,0);
 }
 void safeQueriesRetainLabelWithoutAdvancingOrOpeningInput()
 {
  Fixture f;auto p=f.prepare();QVERIFY(p.ok);QCOMPARE(p.status.takeLabel,QString("Synthetic vocal"));
  QCOMPARE(f.manager.status().takeLabel,p.status.takeLabel);for(int i=0;i<10;++i){std::ignore=f.manager.status();std::ignore=f.manager.takes();std::ignore=f.manager.capabilities();}
  QCOMPARE(f.input->initializes,0);QCOMPARE(f.input->opens,0);QCOMPARE(f.manager.takes({-1,25}).code,QString("invalid_page"));QCOMPARE(f.manager.takes({0,101}).code,QString("invalid_page"));
  auto invalid=f.request();invalid.takeLabel=QString(97,'a');QCOMPARE(f.manager.prepare(invalid).code,QString("label_resource_limit"));
 }
 void outputWithLegacyCaptureIsRefusedBeforeOpening()
 {
  Fixture f;f.transport.world.outputCaptureSafe=false;QVERIFY(f.arm());const auto before=f.files();
  QCOMPARE(f.prepare().code,QString("output_capture_unsupported"));QCOMPARE(f.input->opens,0);QCOMPARE(f.files(),before);
 }
 void sessionFactsPersistWithoutClaimingAlignment()
 {
  Fixture f;QVERIFY(f.arm());auto p=f.prepare(1);QVERIFY(p.ok);QVERIFY(f.start(p).ok);f.until([&]{return f.transport.plays==1;});
  f.transport.world.songTick=768;std::ignore=f.manager.poll(200);QVERIFY(f.manager.stop(f.manager.status().takeId).ok);f.until([&]{return f.manager.quiescent();});
  const auto files=QDir(f.folder()).entryList({"*.diagnostics.jsonl"},QDir::Files);QCOMPARE(files.size(),1);QFile file(f.folder()+"/"+files[0]);QVERIFY(file.open(QIODevice::ReadOnly));
  const auto bytes=file.readAll();const auto record=QJsonDocument::fromJson(bytes.trimmed()).object();const auto facts=record["session"].toObject();QVERIFY(!facts.isEmpty());
  QCOMPARE(facts["target_tick"].toString(),QString("768"));QCOMPARE(facts["preroll_origin_tick"].toString(),QString("576"));
  QCOMPARE(facts["native_tempo"].toInt(),120);QCOMPARE(facts["native_numerator"].toInt(),4);QCOMPARE(facts["native_denominator"].toInt(),4);
  QCOMPARE(facts["raw_ready_observed_ms"].toString(),QString("100"));QCOMPARE(facts["frames_at_readiness"].toString(),QString("4"));
  QVERIFY(facts["performance_reached"].toBool());QCOMPARE(facts["placement_quality"].toString(),QString("uncalibrated"));QVERIFY(!facts["sample_accuracy_verified"].toBool(true));
 }
 void stopDuringInputCommandIsExplicitlyDeferred()
 {
  Fixture f;QVERIFY(f.arm());auto p=f.prepare();QVERIFY(p.ok);QString code;
  f.input->onOpen=[&]{code=f.manager.stop(f.manager.status().takeId).code;};
  QVERIFY(f.start(p).ok);f.until([&]{return f.manager.quiescent();});QCOMPARE(code,QString("stop_deferred"));
  QCOMPARE(f.manager.status().acceptedFrames,quint64(4));QCOMPARE(f.manager.status().outcome,TakeOutcome::SavedUnattached);QCOMPARE(f.transport.plays,0);
 }
 void reentrantDisarmDuringInputOpenIsDeferred()
 {
  Fixture f;QVERIFY(f.arm());auto p=f.prepare();QVERIFY(p.ok);bool deferred=false;
  f.input->onOpen=[&]{std::ignore=f.manager.disarm();deferred=f.input->closes==0 && f.input->terminates==0 && f.host.snapshot().armed;};
  QVERIFY(f.start(p).ok);f.until([&]{return f.manager.quiescent();});QVERIFY(deferred);QCOMPARE(f.transport.plays,0);QVERIFY(!f.manager.status().armed);
 }
 void cancelBeforeReadinessDoesNotStartSong()
 {
  Fixture f;f.input->deliver=false;QVERIFY(f.arm());auto p=f.prepare();QVERIFY(p.ok);QVERIFY(f.start(p).ok);
  f.until([&]{return f.input->starts==1;});QVERIFY(f.manager.stop(f.manager.status().takeId).ok);f.until([&]{return f.manager.quiescent();});
  QCOMPARE(f.transport.plays,0);QCOMPARE(f.manager.status().acceptedFrames,quint64(0));QCOMPARE(f.manager.status().captureIntegrity,CaptureIntegrity::Failed);QCOMPARE(f.input->closes,1);
 }
 void shutdownBeforeFirstPollNeverOpensInput()
 {
  Fixture f;QVERIFY(f.arm());auto p=f.prepare();QVERIFY(p.ok);QVERIFY(f.start(p).ok);
  QElapsedTimer timer;timer.start();while(timer.elapsed()<3000 && f.host.snapshot().writer.state!=WriterState::Ready)QTest::qWait(1);
  QCOMPARE(f.host.snapshot().writer.state,WriterState::Ready);std::ignore=f.manager.requestShutdown();f.until([&]{return f.manager.quiescent();});
  QCOMPARE(f.input->opens,0);QCOMPARE(f.input->starts,0);QVERIFY(f.manager.quiescent());
 }
 void shutdownWhileCapturingWaitsForWriterAndRelease()
 {
  Fixture f;QVERIFY(f.arm());auto p=f.prepare(0);QVERIFY(p.ok);QVERIFY(f.start(p).ok);f.until([&]{return f.transport.plays==1;});
  std::ignore=f.manager.requestShutdown();f.until([&]{return f.manager.quiescent();});QVERIFY(f.manager.quiescent());QCOMPARE(f.input->closes,1);QCOMPARE(f.input->terminates,1);
  QCOMPARE(f.manager.status().acceptedFrames,f.manager.status().writtenFrames);QCOMPARE(f.prepare().code,QString("shutting_down"));
 }
 void oldStopCannotCancelANewTakeAndOriginalIsUnchanged()
 {
  Fixture f;QVERIFY(f.arm());auto p=f.prepare(0);QVERIFY(p.ok);QVERIFY(f.start(p).ok);f.until([&]{return f.transport.plays==1;});const auto old=f.manager.status().takeId;
  QVERIFY(f.manager.stop(old).ok);f.until([&]{return f.manager.quiescent();});QFile original(f.folder()+"/take-"+old+".wav");QVERIFY(original.open(QIODevice::ReadOnly));const auto bytes=original.readAll();original.close();
  QVERIFY(f.arm());p=f.prepare(0);QVERIFY(p.ok);QVERIFY(f.start(p).ok);f.until([&]{return f.transport.plays==2;});const auto current=f.manager.status().takeId;QVERIFY(current!=old);
  QVERIFY(f.manager.stop(old).ok);QVERIFY(!f.manager.quiescent());QVERIFY(f.transport.world.playing);QVERIFY(f.manager.stop(current).ok);f.until([&]{return f.manager.quiescent();});
  QVERIFY(original.open(QIODevice::ReadOnly));QCOMPARE(original.readAll(),bytes);QCOMPARE(QDir(f.folder()).entryList({"*.wav"},QDir::Files).size(),2);
 }
 void activeProjectChangeRetainsRawAndReleasesInput()
 {
  Fixture f;QVERIFY(f.arm());auto p=f.prepare();QVERIFY(p.ok);QVERIFY(f.start(p).ok);f.until([&]{return f.transport.plays==1;});
  ++f.transport.world.projectGeneration;++f.transport.world.contextRevision;f.until([&]{return f.manager.quiescent();});
  QCOMPARE(f.manager.status().acceptedFrames,quint64(4));QCOMPARE(f.manager.status().writtenFrames,quint64(4));QCOMPARE(f.input->closes,1);
  QVERIFY(!f.manager.status().armed);QCOMPARE(f.manager.status().placementState,PlacementState::SavedUnattached);
 }
 void rawCapIncludesStartupAndStopsOwnedPlayback()
 {
  Fixture f;QVERIFY(f.arm(12));auto p=f.prepare(0);QVERIFY(p.ok);QVERIFY(f.start(p).ok);f.until([&]{return f.transport.plays==1;});
  std::ignore=f.manager.poll(200);f.input->send(12);f.until([&]{return f.manager.quiescent();});const auto s=f.manager.status();
  QCOMPARE(s.receivedFrames,quint64(16));QCOMPARE(s.acceptedFrames,quint64(12));QCOMPARE(s.writtenFrames,quint64(12));QCOMPARE(s.intentionallyUnrecordedFrames,quint64(4));
  QCOMPARE(s.captureIntegrity,CaptureIntegrity::Complete);QCOMPARE(f.transport.stops,1);QVERIFY(!s.armed);
 }
 void captureBeginsBeforeNativePreroll()
 {
  Fixture f;QVERIFY(f.arm());const auto p=f.prepare();QVERIFY(p.ok);bool headerReady=false;
  f.input->onOpen=[&]{const auto names=QDir(f.folder()).entryList({"*.partial"},QDir::Files);if(names.size()!=1)return;const auto path=f.folder()+"/"+names[0];HANDLE h=CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);char bytes[4]{};DWORD n=0;headerReady=h!=INVALID_HANDLE_VALUE && ReadFile(h,bytes,4,&n,nullptr) && n==4 && QByteArray(bytes,4)=="RIFF";if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);};
  QVERIFY(f.start(p).ok);f.until([&]{return f.transport.plays==1;});QVERIFY(headerReady);QCOMPARE(f.transport.plays,1);
  const auto frame=std::find(f.events.begin(),f.events.end(),QString("input_frames")),play=std::find(f.events.begin(),f.events.end(),QString("play"));QVERIFY(frame<play);
  QCOMPARE(f.transport.world.songTick,qint64(576));QCOMPARE(f.manager.status().phase,Phase::Preroll);
 }
 void rawFramesExistDuringPreroll()
 {
  Fixture f;QVERIFY(f.arm());const auto p=f.prepare();QVERIFY(p.ok);QVERIFY(f.start(p).ok);f.until([&]{return f.transport.plays==1;});f.input->send(8);
  f.until([&]{return f.manager.status().writtenFrames>=12;});const auto status=f.manager.status();QCOMPARE(status.phase,Phase::Preroll);QCOMPARE(status.acceptedFrames,quint64(12));QCOMPARE(status.writtenFrames,quint64(12));
  QVERIFY(f.manager.stop(status.takeId).ok);f.until([&]{return f.manager.quiescent();});QCOMPARE(f.manager.status().captureIntegrity,CaptureIntegrity::Complete);
  QCOMPARE(f.manager.status().placementState,PlacementState::SavedUnattached);QVERIFY(f.manager.status().cancelled);
 }
 void prerollUsesNativeMeter()
 {
  Fixture f;f.transport.world.numerator=3;f.transport.world.denominator=4;f.transport.world.ticksPerBar=144;QVERIFY(f.arm());auto p=f.prepare(2);QVERIFY(p.ok);QVERIFY(f.start(p).ok);
  f.until([&]{return f.transport.plays==1;});QCOMPARE(f.transport.world.songTick,qint64(480));
  f.transport.world.songTick=600;std::ignore=f.manager.poll(100000);QCOMPARE(f.manager.status().phase,Phase::Preroll);QCOMPARE(f.manager.status().countInTicksRemaining,qint64(168));
  f.transport.world.songTick=768;std::ignore=f.manager.poll(100001);QCOMPARE(f.manager.status().phase,Phase::Recording);QVERIFY(f.manager.status().performanceReached);
 }
 void insufficientPrerollRefused()
 {
  Fixture f;auto request=f.request(2);request.startTick=100;const auto before=f.files();QCOMPARE(f.manager.prepare(request).code,QString("insufficient_preroll"));QCOMPARE(f.files(),before);QCOMPARE(f.input->initializes,0);
 }
 void countinExhausting120SecondBudgetRefused()
 {
  Fixture f;f.transport.world.tempo=10;f.transport.world.numerator=32;f.transport.world.denominator=4;f.transport.world.ticksPerBar=1536;
  auto request=f.request(2);request.startTick=6000;QCOMPARE(f.manager.prepare(request).code,QString("countin_exceeds_take_budget"));QCOMPARE(f.input->opens,0);
 }
 void captureEndedDuringStartupNeverStartsLatePlayback_data()
 {QTest::addColumn<bool>("duringSeek");QTest::newRow("start-callback")<<false;QTest::newRow("seek-callback")<<true;}
 void captureEndedDuringStartupNeverStartsLatePlayback()
 {
  QFETCH(bool,duringSeek);Fixture f;f.input->startupFrames=duringSeek?4:8;QVERIFY(f.arm(8));auto p=f.prepare(0);QVERIFY(p.ok);
  if(duringSeek)f.transport.onSeek=[&]{f.input->send(4);};
  QVERIFY(f.start(p).ok);f.until([&]{return f.manager.quiescent();});
  QCOMPARE(f.transport.plays,0);QCOMPARE(f.manager.status().acceptedFrames,quint64(8));QCOMPARE(f.manager.status().placementState,PlacementState::SavedUnattached);
 }
 void metronomeTrueRefused()
 {
  Fixture f;auto request=f.request();request.metronomeEnabled=true;QCOMPARE(f.manager.prepare(request).code,QString("unsupported_metronome"));
  request.metronomeEnabled=false;f.transport.world.metronomeActive=true;QCOMPARE(f.manager.prepare(request).code,QString("unsupported_metronome"));QCOMPARE(f.input->opens,0);
 }
 void ambiguousStartNeverReplayed()
 {
  Fixture f;QVERIFY(f.arm());auto p=f.prepare();QVERIFY(p.ok);const auto request=uuid();QVERIFY(f.start(p,request).ok);QVERIFY(f.start(p,request).ok);
  f.until([&]{return f.transport.plays==1;});QCOMPARE(f.input->opens,1);QCOMPARE(f.transport.plays,1);const auto take=f.manager.status().takeId;
  QVERIFY(f.manager.stop(take).ok);f.until([&]{return f.manager.quiescent();});const auto files=f.files();QVERIFY(f.start(p,request).ok);QCOMPARE(f.input->opens,1);QCOMPARE(f.files(),files);
  QCOMPARE(f.manager.start({uuid(),1,request}).code,QString("conflicting_start"));
 }
 void localDisarmWins()
 {
  Fixture f;QVERIFY(f.arm());auto p=f.prepare();QVERIFY(p.ok);bool deferred=false;
  f.transport.onPlay=[&]{std::ignore=f.manager.disarm();deferred=f.input->stops==0;};f.input->onStop=[&]{QVERIFY(!f.transport.inCommand);};
  QVERIFY(f.start(p).ok);f.until([&]{return f.manager.quiescent();});QVERIFY(deferred);QVERIFY(!f.manager.status().armed);QCOMPARE(f.transport.stops,1);
  QCOMPARE(f.input->starts,1);QCOMPARE(f.input->closes,1);QVERIFY(f.manager.status().cancelled);
 }
 void repeatedStopIsIdempotent()
 {
  Fixture f;QVERIFY(f.arm());auto p=f.prepare();QVERIFY(p.ok);QVERIFY(f.start(p).ok);f.until([&]{return f.transport.plays==1;});const auto take=f.manager.status().takeId;
  QVERIFY(f.manager.stop(take).ok);QVERIFY(f.manager.stop(take).ok);f.until([&]{return f.manager.quiescent();});QVERIFY(f.manager.stop(take).ok);
  QCOMPARE(f.input->stops,1);QCOMPARE(f.input->closes,1);QCOMPARE(f.transport.stops,1);
 }
 void quiescingRetainsExclusiveCharge()
 {
  Fixture f;QVERIFY(f.arm());auto p=f.prepare();QVERIFY(p.ok);QVERIFY(f.start(p).ok);f.until([&]{return f.transport.plays==1;});
  f.input->stopResult={"synthetic_stop_failed",false};QVERIFY(!f.manager.stop(f.manager.status().takeId).ok);QVERIFY(!f.manager.quiescent());
  QCOMPARE(f.manager.status().phase,Phase::Quiescing);QCOMPARE(f.prepare().code,QString("recording_busy"));QCOMPARE(f.input->closes,0);
  f.input->stopResult={"ok",true};std::ignore=f.manager.stop(f.manager.status().takeId);f.until([&]{return f.manager.quiescent();});QCOMPARE(f.input->closes,1);
 }
 void projectRootDeviceChangeRevokesArm_data()
 {QTest::addColumn<int>("change");QTest::newRow("project")<<0;QTest::newRow("root")<<1;QTest::newRow("backend")<<2;}
 void projectRootDeviceChangeRevokesArm()
 {
  QFETCH(int,change);Fixture f;QVERIFY(f.arm());auto p=f.prepare();QVERIFY(p.ok);
  if(change==0){++f.transport.world.projectGeneration;++f.transport.world.contextRevision;}
  if(change==1)std::ignore=f.host.notifyAuthorizationChanged(f.host.rootRevision()+1);
  if(change==2)std::ignore=f.host.requestBackendChange();
  std::ignore=f.manager.poll(100);QVERIFY(!f.manager.status().armed);QVERIFY(!f.start(p).ok);QCOMPARE(f.input->opens,0);
 }
 void songStartFailurePreservesRaw()
 {
  Fixture f;QVERIFY(f.arm());auto p=f.prepare();QVERIFY(p.ok);f.transport.failPlay=true;QVERIFY(f.start(p).ok);f.until([&]{return f.manager.quiescent();});
  const auto status=f.manager.status();QCOMPARE(status.captureIntegrity,CaptureIntegrity::Complete);QCOMPARE(status.placementState,PlacementState::SavedUnattached);
  QCOMPARE(status.acceptedFrames,quint64(4));QCOMPARE(QDir(f.folder()).entryList({"*.wav"},QDir::Files).size(),1);QCOMPARE(f.input->closes,1);
 }
 void knownSeekPauseTempoChangeStopsOrInvalidates_data()
 {QTest::addColumn<int>("change");QTest::newRow("seek")<<0;QTest::newRow("pause")<<1;QTest::newRow("tempo")<<2;}
 void knownSeekPauseTempoChangeStopsOrInvalidates()
 {
  QFETCH(int,change);Fixture f;QVERIFY(f.arm());auto p=f.prepare();QVERIFY(p.ok);QVERIFY(f.start(p).ok);f.until([&]{return f.transport.plays==1;});
  if(change==0){++f.transport.world.seekRevision;f.transport.world.songTick=400;}
  if(change==1){++f.transport.world.stateRevision;f.transport.world.paused=true;f.transport.world.playing=false;}
  if(change==2){++f.transport.world.contextRevision;++f.transport.world.tempo;}
  std::ignore=f.manager.poll(200);f.until([&]{return f.manager.quiescent();});QCOMPARE(f.input->closes,1);QCOMPARE(f.manager.status().placementState,PlacementState::SavedUnattached);
  QVERIFY(!f.manager.status().armed);
 }
 void cancelDuringCountinPreservesNoNormalClip()
 {
  Fixture f;QVERIFY(f.arm());auto p=f.prepare(2);QVERIFY(p.ok);QVERIFY(f.start(p).ok);f.until([&]{return f.transport.plays==1;});
  QVERIFY(f.manager.stop(f.manager.status().takeId).ok);f.until([&]{return f.manager.quiescent();});const auto status=f.manager.status();
  QCOMPARE(status.acceptedFrames,quint64(4));QCOMPARE(status.writtenFrames,quint64(4));QCOMPARE(status.placementState,PlacementState::SavedUnattached);
  QVERIFY(status.cancelled);QVERIFY(!status.placementConfirmed);QCOMPARE(lmms::Engine::getSong()->tracks().size(),std::size_t(0));
 }
 void captureCompleteDoesNotImplyPlacementVerified()
 {
  Fixture f;QVERIFY(f.arm());auto p=f.prepare(0);QVERIFY(p.ok);QVERIFY(f.start(p).ok);f.until([&]{return f.transport.plays==1;});
  std::ignore=f.manager.poll(200);QVERIFY(f.manager.stop(f.manager.status().takeId).ok);f.until([&]{return f.manager.quiescent();});const auto status=f.manager.status();
  QCOMPARE(status.captureIntegrity,CaptureIntegrity::Complete);QCOMPARE(status.placementState,PlacementState::AwaitingLocalPlacement);
  QCOMPARE(status.placementQuality,QString("uncalibrated"));QCOMPARE(status.driftQuality,QString("unmeasured"));QCOMPARE(status.outputContinuity,QString("unobserved"));
  QVERIFY(!status.sampleAccuracyVerified);QVERIFY(!status.placementConfirmed);QCOMPARE(f.manager.confirmPlacement({}).code,QString("manual_approval_required"));
  QCOMPARE(f.manager.takes().takes.size(),std::size_t(1));
 }
 void localConfirmationAttachesOneTakeWithoutCalibrating()
 {
  Engine::audioEngine()->setAudioDevice(new SyntheticOutput(48000),false);Engine::getSong()->setTempo(120);
  Fixture f;QVERIFY(f.arm());auto p=f.prepare(0);QVERIFY(p.ok);QVERIFY(f.start(p).ok);f.until([&]{return f.transport.plays==1;});
  f.transport.world.songTick=768;std::ignore=f.manager.poll(200);const auto takeId=f.manager.status().takeId;
  QVERIFY(!takeId.isEmpty());QVERIFY(f.manager.stop(takeId).ok);f.until([&]{return f.manager.quiescent();});
  const auto beforeStatus=f.manager.status();QCOMPARE(beforeStatus.captureIntegrity,CaptureIntegrity::Complete);
  QCOMPARE(beforeStatus.placementState,PlacementState::AwaitingLocalPlacement);

  QDomDocument document;auto head=document.createElement(QStringLiteral("head"));document.appendChild(head);
  auto identity=head.ownerDocument().createElement(QStringLiteral("recording_project"));
  identity.setAttribute(QStringLiteral("schema"),1);identity.setAttribute(QStringLiteral("id"),f.host.projectIdentity().id);identity.setAttribute(QStringLiteral("nonce"),uuid());head.appendChild(identity);
  Engine::getSong()->recordingProjectIdentity().acceptLoad(head,QString{});
  auto* config=ConfigManager::inst();const auto oldFile=config->value("aibridge","audio_import_max_file_bytes","task7-absent");
  const auto oldDecoded=config->value("aibridge","audio_import_max_decoded_bytes","task7-absent");
  auto restore= qScopeGuard([&]{if(oldFile=="task7-absent")config->deleteValue("aibridge","audio_import_max_file_bytes");else config->setValue("aibridge","audio_import_max_file_bytes",oldFile);
   if(oldDecoded=="task7-absent")config->deleteValue("aibridge","audio_import_max_decoded_bytes");else config->setValue("aibridge","audio_import_max_decoded_bytes",oldDecoded);AudioImportPolicy::configured().refresh();});
  config->setValue("aibridge","audio_import_max_file_bytes","2000000");config->setValue("aibridge","audio_import_max_decoded_bytes","2000000");
  const auto folder=f.folder();const auto takePath=folder+"/take-"+takeId+".wav";QFile audio(takePath);QVERIFY(audio.open(QIODevice::ReadOnly));const auto before=QCryptographicHash::hash(audio.readAll(),QCryptographicHash::Sha256);audio.close();
  const auto tracksBefore=Engine::getSong()->tracks().size();
  const auto approval=RecordingRecorderView::approve(takeId,1,1,0,768);
  const auto attached=f.manager.confirmPlacement(approval);QVERIFY(attached.ok);QCOMPARE(attached.code,QStringLiteral("take_attached"));
  const auto status=f.manager.status();QCOMPARE(status.placementState,PlacementState::Attached);QVERIFY(status.placementConfirmed);
  QCOMPARE(status.placementQuality,QStringLiteral("uncalibrated"));QCOMPARE(status.driftQuality,QStringLiteral("unmeasured"));QVERIFY(!status.sampleAccuracyVerified);
  QCOMPARE(Engine::getSong()->tracks().size(),tracksBefore+1);const auto clipCount=Engine::getSong()->tracks().back()->numOfClips();QCOMPARE(clipCount,1);
  const auto repeated=f.manager.confirmPlacement(approval);QVERIFY(repeated.ok);QCOMPARE(repeated.code,QStringLiteral("already_attached"));
  QCOMPARE(Engine::getSong()->tracks().size(),tracksBefore+1);
  QFile afterFile(takePath);QVERIFY(afterFile.open(QIODevice::ReadOnly));QCOMPARE(QCryptographicHash::hash(afterFile.readAll(),QCryptographicHash::Sha256),before);
  QVERIFY(f.manager.disarm().ok);
 }
};
QTEST_GUILESS_MAIN(RecordingSessionManagerTest)
#include "RecordingSessionManagerTest.moc"
