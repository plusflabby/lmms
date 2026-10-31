#include <QtTest>
#include <QtEndian>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QDomDocument>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
#include <cstring>
#include <cmath>
#include "AudioDevice.h"
#include "AudioImportPolicy.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "SampleClip.h"
#include "SamplePlayHandle.h"
#include "SampleTrack.h"
#include "Song.h"
using namespace lmms;
namespace {
class Output final : public AudioDevice
{
public: explicit Output(int rate):AudioDevice(2,Engine::audioEngine()){setSampleRate(rate);}
private: void startProcessingImpl() override{} void stopProcessingImpl() override{}
};
QByteArray hash(const QString& path){QFile f(path);if(!f.open(QIODevice::ReadOnly))return {};return QCryptographicHash::hash(f.readAll(),QCryptographicHash::Sha256);}
QDomElement save(SampleClip& clip){QDomDocument d;auto n=d.createElement("sampleclip");d.appendChild(n);clip.saveSettings(d,n);return n;}
}
class RecordingClipOffsetPlaybackTest : public QObject
{
 Q_OBJECT
 QStringList previous;
 const QStringList keys{"audio_import_roots","audio_import_max_file_bytes","audio_import_max_decoded_bytes"};
 void flush(){auto* s=Engine::getSong();auto* e=Engine::audioEngine();s->stop();e->clear();e->renderNextPeriod();e->renderNextPeriod();}
 std::vector<SampleFrame> reference(SampleClip& clip,int first,int end)
 {
  Sample copy=clip.sample();copy.setStartFrame(first);copy.setEndFrame(end);SamplePlayHandle handle(&copy,false);
  const auto period=Engine::audioEngine()->framesPerPeriod();std::vector<SampleFrame> result,block(period);
  for(int frame=0;frame<end-first;frame+=period){handle.play(block.data());result.insert(result.end(),block.begin(),block.end());}
  result.resize(end-first);return result;
 }
 void check(SampleClip& clip,int seek,int rawFrames)
 {
  auto* song=Engine::getSong();auto* engine=Engine::audioEngine();const auto period=engine->framesPerPeriod();const auto fpt=Engine::framesPerTick(clip.sample().sampleRate());
  const int first=f_cnt_t(fpt*(seek-clip.startTimeOffset().getTicks()));const auto expected=reference(clip,first,rawFrames);
  flush();song->getTimeline(Song::PlayMode::Song).setLoopEnabled(false);song->playSong();song->setPlayPos(clip.startPosition().getTicks()+seek,Song::PlayMode::Song);
  qint64 mismatches=0,prefixSamples=0,tailNonzero=0;float maximumError=0;
  const int count=int((expected.size()+period*3+period-1)/period);
  for(int part=0;part<count;++part){const auto block=engine->renderNextPeriod();
   for(size_t j=0;j<block.size();++j){const qint64 relative=qint64(part)*period+j-period;const auto value=relative>=0 && relative<qint64(expected.size())?expected[size_t(relative)]:SampleFrame{};
    for(int ch=0;ch<2;++ch){const float error=std::abs(block[j][ch]-value[ch]);if(error>0.000001f)++mismatches;maximumError=std::max(maximumError,error);
     if(block[j][ch]>.5f)++prefixSamples;
     if(relative>=qint64(expected.size())-64 && relative<qint64(expected.size()) && std::abs(block[j][ch])>.001f)++tailNonzero;}
   }
  }
  flush();QCOMPARE(clip.sample().startFrame(),first);QCOMPARE(clip.sample().endFrame(),rawFrames);QVERIFY(!clip.hasAudioSourceWindow());
  qInfo()<<"LEGACY_OFFSET"<<song->getTempo()<<clip.sample().sampleRate()<<"seek"<<seek<<"first"<<first<<"mismatches"<<mismatches<<"max_error"<<maximumError<<"tail_nonzero"<<tailNonzero;
  QCOMPARE(prefixSamples,qint64(0));QVERIFY(tailNonzero>0);QCOMPARE(mismatches,qint64(0));
 }
private slots:
 void initTestCase(){Engine::init(true);Engine::audioEngine()->audioDev()->stopProcessing();}
 void cleanupTestCase(){flush();Engine::destroy();}
 void nativeOffsetPersistenceAndPlayback_data()
 {QTest::addColumn<int>("rate");QTest::addColumn<int>("tempo");QTest::addColumn<QString>("boundary");for(int rate:{44100,48000})for(int tempo:{100,125,137})for(const auto& boundary:QStringList{"ordinary-tail","one-frame-range","one-frame-last-period"})QTest::newRow(qPrintable(QString::number(rate)+"-"+QString::number(tempo)+"-"+boundary))<<rate<<tempo<<boundary;}
 void nativeOffsetPersistenceAndPlayback()
 {
  QFETCH(int,rate);QFETCH(int,tempo);QFETCH(QString,boundary);flush();Engine::audioEngine()->setAudioDevice(new Output(rate),false);Engine::getSong()->setTempo(tempo);
  QCOMPARE(Engine::audioEngine()->outputSampleRate(),sample_rate_t(rate));QCOMPARE(Engine::framesPerTick(),Engine::framesPerTick(rate));
  QTemporaryDir directory(QCoreApplication::applicationDirPath()+"/recording-offset-fixture-XXXXXX");directory.setAutoRemove(false);QVERIFY(directory.isValid());const auto path=directory.filePath("immutable-original.wav");const int skip=13,position=384;
  const auto fpt=Engine::framesPerTick(rate);const int first=f_cnt_t(fpt*skip);const int frames=boundary=="one-frame-range"?first+1:boundary=="one-frame-last-period"?first+int(Engine::audioEngine()->framesPerPeriod())*8+1:rate*2+137;
  if(boundary=="one-frame-range")QCOMPARE(frames-first,1);if(boundary=="one-frame-last-period")QCOMPARE((frames-first)%int(Engine::audioEngine()->framesPerPeriod()),1);
  int length=1;while(f_cnt_t(fpt*(skip+length))<f_cnt_t(frames))++length;
  QVERIFY(f_cnt_t(fpt*(skip+length-1))<f_cnt_t(frames));QByteArray bytes(44+frames*2,0);std::memcpy(bytes.data(),"RIFF",4);std::memcpy(bytes.data()+8,"WAVEfmt ",8);std::memcpy(bytes.data()+36,"data",4);
  qToLittleEndian<quint32>(36+frames*2,bytes.data()+4);qToLittleEndian<quint32>(16,bytes.data()+16);qToLittleEndian<quint16>(1,bytes.data()+20);qToLittleEndian<quint16>(1,bytes.data()+22);
  qToLittleEndian<quint32>(rate,bytes.data()+24);qToLittleEndian<quint32>(rate*2,bytes.data()+28);qToLittleEndian<quint16>(2,bytes.data()+32);qToLittleEndian<quint16>(16,bytes.data()+34);qToLittleEndian<quint32>(frames*2,bytes.data()+40);
  for(int i=0;i<frames;++i){const qint16 value=i<first?24576:i>=frames-128?-8192:qint16((i*7919u)%10001u-5000);qToLittleEndian<qint16>(value,bytes.data()+44+i*2);}
  {QFile f(path);QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::NewOnly));QCOMPARE(f.write(bytes),qint64(bytes.size()));}const auto before=hash(path);qInfo()<<"RETAINED_SYNTHETIC_ORIGINAL"<<path<<before.toHex()<<"frames"<<frames;
  auto* config=ConfigManager::inst();for(const auto& key:keys)previous<<config->value("aibridge",key,"task6-absent");
  config->setValue("aibridge",keys[0],QString::fromUtf8(QJsonDocument(QJsonArray{directory.path()}).toJson(QJsonDocument::Compact)));config->setValue("aibridge",keys[1],"1000000");config->setValue("aibridge",keys[2],"2000000");
  {
   SampleTrack track(Engine::getSong());SampleClip clip(&track);QCOMPARE(track.numOfClips(),1);
   auto prepared=AudioImportPolicy::configured().prepare(path);QCOMPARE(prepared.code,QString("ok"));QCOMPARE(clip.adoptAuthorizedAudio(std::move(prepared.audio)),QString("ok"));
   clip.setAutoResize(false);clip.movePosition(position);clip.setStartTimeOffset(-skip);clip.changeLength(length);
   QCOMPARE(clip.sample().sampleSize(),size_t(frames));QCOMPARE(clip.startTimeOffset().getTicks(),-skip);QVERIFY(!clip.hasAudioSourceWindow());
   check(clip,0,frames);if(QTest::currentTestFailed())return;
   if(f_cnt_t(fpt*(skip+7))<f_cnt_t(frames)){check(clip,7,frames);if(QTest::currentTestFailed())return;}
   check(clip,0,frames);if(QTest::currentTestFailed())return;
   const auto saved=save(clip);QCOMPARE(saved.attribute("off").toInt(),-skip);QCOMPARE(saved.attribute("pos").toInt(),position);QCOMPARE(saved.attribute("len").toInt(),length);QCOMPARE(saved.attribute("autoresize"),QString("0"));QVERIFY(saved.firstChildElement("audio_source_window").isNull());
   clip.loadSettings(saved);QCOMPARE(clip.sourceLoadError(),QString());QCOMPARE(clip.startTimeOffset().getTicks(),-skip);QCOMPARE(clip.length().getTicks(),length);QVERIFY(!clip.getAutoResize());
   check(clip,0,frames);if(QTest::currentTestFailed())return;if(f_cnt_t(fpt*(skip+7))<f_cnt_t(frames))check(clip,7,frames);flush();
   clip.setMuted(true);
   SampleTrack reopenedTrack(Engine::getSong());SampleClip reopened(&reopenedTrack);reopened.loadSettings(saved);
   QCOMPARE(reopened.sourceLoadError(),QString());QCOMPARE(reopened.sample().sampleSize(),size_t(frames));
   QCOMPARE(reopened.startTimeOffset().getTicks(),-skip);QCOMPARE(reopened.startPosition().getTicks(),position);QCOMPARE(reopened.length().getTicks(),length);QVERIFY(!reopened.getAutoResize());
   check(reopened,0,frames);if(QTest::currentTestFailed())return;if(f_cnt_t(fpt*(skip+7))<f_cnt_t(frames))check(reopened,7,frames);check(reopened,0,frames);flush();
  }
  QCOMPARE(hash(path),before);
 }
 void cleanup(){flush();auto* c=ConfigManager::inst();if(previous.size()==keys.size())for(int i=0;i<keys.size();++i){if(previous[i]=="task6-absent")c->deleteValue("aibridge",keys[i]);else c->setValue("aibridge",keys[i],previous[i]);}previous.clear();AudioImportPolicy::configured().refresh();}
};
QTEST_MAIN(RecordingClipOffsetPlaybackTest)
#include "RecordingClipOffsetPlaybackTest.moc"
