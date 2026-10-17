#include <QtTest>
#include <QtEndian>
#include <QCryptographicHash>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <cstring>
#include "AudioImportPolicy.h"
#include "AudioDevice.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "SampleClip.h"
#include "SamplePlayHandle.h"
#include "SampleTrack.h"
#include "Song.h"
#include "ProjectJournal.h"
#include "AiBridgeAudioContext.h"
#include "AiBridgeAudioRangeActions.h"
#include "AudioWindowPlayback.h"
#include <stdexcept>
#include <QLocalSocket>
#include <QElapsedTimer>
#include <QScopeGuard>
#include "AiBridgeIpcServer.h"
#include "AiBridgeProtocol.h"
#include "Timeline.h"

class RangeProbeDevice : public lmms::AudioDevice
{
public:
	explicit RangeProbeDevice(int rate):AudioDevice(2,lmms::Engine::audioEngine()){setSampleRate(rate);}
	void startProcessingImpl() override {}
	void stopProcessingImpl() override {}
};

class AiBridgeAudioRangeProtocolTest : public QObject
{
	Q_OBJECT
	QString directory, source;
	QByteArray digest;
	QStringList old;
	QString oldDeviceBuffer;
	const QStringList keys{"audio_import_roots","audio_import_max_file_bytes","audio_import_max_decoded_bytes"};
	QDomElement save(lmms::SampleClip& clip)
	{
		QDomDocument doc; auto element=doc.createElement("sampleclip"); doc.appendChild(element);
		clip.saveSettings(doc,element); return element;
	}
	QDomElement window()
	{
		QDomDocument doc;
		doc.setContent(QStringLiteral("<audio_source_window version='2' phase='linear_equal_ref256_v2' reference_end='88200' in='4410' out='39690' origin='4410' source_frames='88200' source_rate='44100' reference_bpm='125' reference_output_rate='44100' ticks_per_beat='48' frames_per_tick='441' length_ticks='80' offset_ticks='-10'/>"));
		return doc.documentElement();
	}
	QDomElement state()
	{
		lmms::SampleTrack track(lmms::Engine::getSong()); lmms::SampleClip clip(&track);
		auto prepared=lmms::AudioImportPolicy::configured().prepare(source);
		if(prepared.code!="ok" || clip.adoptAuthorizedAudio(std::move(prepared.audio))!="ok") return {};
		clip.setAutoResize(false); clip.movePosition(110); clip.changeLength(80); clip.setStartTimeOffset(-10);
		auto node=save(clip); node.appendChild(node.ownerDocument().importNode(window(),true)); return node;
	}
	QByteArray fileHash()
	{
		QFile f(source); if(!f.open(QIODevice::ReadOnly)) return {};
		return QCryptographicHash::hash(f.readAll(),QCryptographicHash::Sha256);
	}
private slots:
	void initTestCase()
	{
		using namespace lmms;oldDeviceBuffer=ConfigManager::inst()->value("audioengine","framesperaudiobuffer","range-absent");
		bool ok=false;const int period=qEnvironmentVariableIntValue("LMMS_V2_TEST_BUFFER",&ok);if(ok)ConfigManager::inst()->setValue("audioengine","framesperaudiobuffer",QString::number(period));
		Engine::init(true);Engine::audioEngine()->audioDev()->stopProcessing();qInfo()<<"RANGE_PERIOD"<<Engine::audioEngine()->framesPerPeriod();
	}
	void cleanupTestCase()
	{
		using namespace lmms;if(oldDeviceBuffer=="range-absent")ConfigManager::inst()->deleteValue("audioengine","framesperaudiobuffer");else ConfigManager::inst()->setValue("audioengine","framesperaudiobuffer",oldDeviceBuffer);Engine::destroy();
	}
	void init()
	{
		using namespace lmms; Engine::getSong()->setTempo(125);Engine::audioEngine()->setAudioDevice(new RangeProbeDevice(44100),false);
		for(const auto& key:keys) old<<ConfigManager::inst()->value("aibridge",key,"range-test-absent");
		directory=QCoreApplication::applicationDirPath()+"/window-persistence-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
		QVERIFY(QDir().mkpath(directory)); source=directory+"/original.wav";
		QByteArray bytes(352844,0); std::memcpy(bytes.data(),"RIFF",4); std::memcpy(bytes.data()+8,"WAVEfmt ",8); std::memcpy(bytes.data()+36,"data",4);
		qToLittleEndian<quint32>(352836,bytes.data()+4); qToLittleEndian<quint32>(16,bytes.data()+16);
		qToLittleEndian<quint16>(1,bytes.data()+20); qToLittleEndian<quint16>(2,bytes.data()+22);
		qToLittleEndian<quint32>(44100,bytes.data()+24); qToLittleEndian<quint32>(176400,bytes.data()+28);
		qToLittleEndian<quint16>(4,bytes.data()+32); qToLittleEndian<quint16>(16,bytes.data()+34); qToLittleEndian<quint32>(352800,bytes.data()+40);
		for(int i=0;i<88200;++i) { qToLittleEndian<qint16>((i*7919u)%30001u-15000,bytes.data()+44+i*4); qToLittleEndian<qint16>((i*3571u)%29009u-14504,bytes.data()+46+i*4); }
		{QFile f(source); QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::NewOnly)); QCOMPARE(f.write(bytes),qint64(bytes.size()));}
		digest=fileHash();
		ConfigManager::inst()->setValue("aibridge",keys[0],QString::fromUtf8(QJsonDocument(QJsonArray{directory}).toJson(QJsonDocument::Compact)));
		ConfigManager::inst()->setValue("aibridge",keys[1],"1000000"); ConfigManager::inst()->setValue("aibridge",keys[2],"2000000");
	}
	void cleanup()
	{
		using namespace lmms;
		QCOMPARE(fileHash(),digest);
		for(int i=0;i<keys.size();++i) {if(old[i]=="range-test-absent") ConfigManager::inst()->deleteValue("aibridge",keys[i]); else ConfigManager::inst()->setValue("aibridge",keys[i],old[i]);}
		old.clear(); AudioImportPolicy::configured().refresh();setAudioRangeTestHook(nullptr);
	}
	void strictRequests_data()
	{
		QTest::addColumn<QString>("action");QTest::addColumn<QJsonObject>("parameters");QTest::addColumn<bool>("accepted");
		const QJsonObject base{{"clip_id","clip"},{"expected_audio_state_hash",QString(64,'a')}};
		for(const QString action:{"trim_audio_clip","split_audio_clip"})
		{
			auto p=base;p.insert(action=="trim_audio_clip"?"new_edge_tick":"split_tick",140);if(action=="trim_audio_clip")p.insert("edge","left");
			QTest::newRow(qPrintable(action+"-valid"))<<action<<p<<true;auto dry=p;dry.insert("dry_run",true);QTest::newRow(qPrintable(action+"-dry"))<<action<<dry<<true;
			for(const QString key:{"clip_id","expected_audio_state_hash",action=="trim_audio_clip"?"new_edge_tick":"split_tick"}){auto bad=p;bad.remove(key);QTest::newRow(qPrintable(action+"-missing-"+key))<<action<<bad<<false;}
			for(const QString key:{"path","frames","source_window","reference_end","operations","unknown"}){auto bad=p;bad.insert(key,1);QTest::newRow(qPrintable(action+"-extra-"+key))<<action<<bad<<false;}
			for(const QJsonValue tick:{QJsonValue(-1),QJsonValue(.5),QJsonValue(2147483648.0),QJsonValue("140"),QJsonValue(QJsonValue::Null)}){auto bad=p;bad.insert(action=="trim_audio_clip"?"new_edge_tick":"split_tick",tick);QTest::newRow(qPrintable(action+"-tick-"+QString::number(tick.type())+"-"+tick.toVariant().toString()))<<action<<bad<<false;}
			auto bad=p;bad.insert("dry_run","true");QTest::newRow(qPrintable(action+"-bool"))<<action<<bad<<false;bad=p;bad.insert("expected_audio_state_hash",QString(64,'A'));QTest::newRow(qPrintable(action+"-hash"))<<action<<bad<<false;
		}
		for(const QString edge:{"both","Left","","split"}){auto p=base;p.insert("edge",edge);p.insert("new_edge_tick",140);QTest::newRow(qPrintable("edge-"+edge))<<"trim_audio_clip"<<p<<false;}
	}
	void strictRequests()
	{
		QFETCH(QString,action);QFETCH(QJsonObject,parameters);QFETCH(bool,accepted);const auto result=lmms::AiBridgeProtocol::parseRequest(QJsonDocument(QJsonObject{{"version",2},{"request_id","test"},{"action",action},{"parameters",parameters}}).toJson(QJsonDocument::Compact)+'\n');QCOMPARE(result.success,accepted);
		if(!accepted)QCOMPARE(result.code,QString("invalid_parameter"));
	}
	QJsonObject call(QLocalSocket& socket,const QString& action,const QJsonObject& parameters={})
	{
		socket.write(QJsonDocument(QJsonObject{{"version",2},{"request_id",QUuid::createUuid().toString()},{"action",action},{"parameters",parameters}}).toJson(QJsonDocument::Compact)+'\n');socket.flush();QElapsedTimer timer;timer.start();QByteArray data;while(!data.endsWith('\n') && timer.elapsed()<5000){QTest::qWait(1);data+=socket.readAll();}if(data.size()>65536 || !data.endsWith('\n'))return {};return QJsonDocument::fromJson(data).object();
	}
	void socketDryRunExecutionMetadataAndStaleState()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);clip.loadSettings(state());AiBridgeProjectContext ids;const auto id=ids.idForClip(&clip);
		AiBridgeIpcServer server(nullptr,"audio-range-"+QUuid::createUuid().toString());QVERIFY(server.start());QLocalSocket socket;socket.connectToServer(server.endpointName());QVERIFY(socket.waitForConnected(1000));
		auto read=call(socket,"get_audio_clip",{{"clip_id",id}}).value("data").toObject();QCOMPARE(read.value("range_status").toString(),QString("ok"));QCOMPARE(read.value("source_window").toObject().value("reference_end_frame").toInt(),88200);
		auto p=QJsonObject{{"clip_id",id},{"expected_audio_state_hash",read.value("audio_state_hash")},{"split_tick",140},{"dry_run",true}};Engine::projectJournal()->clearJournal();
		const auto dry=call(socket,"split_audio_clip",p);QCOMPARE(dry.value("code").toString(),QString("ok"));QCOMPARE(track.numOfClips(),1);QVERIFY(!Engine::projectJournal()->canUndo());QVERIFY(!dry.value("data").toObject().value("preview").toArray()[1].toObject().contains("clip_id"));
		p.insert("dry_run",false);const auto executed=call(socket,"split_audio_clip",p);QCOMPARE(executed.value("code").toString(),QString("ok"));const auto data=executed.value("data").toObject();QCOMPARE(data.value("preview"),dry.value("data").toObject().value("preview"));QCOMPARE(data.value("applied").toArray(),(QJsonArray{0,1}));QCOMPARE(track.numOfClips(),2);QCOMPARE(data.value("created_clip_ids").toArray().size(),1);
		QCOMPARE(call(socket,"split_audio_clip",p).value("code").toString(),QString("clip_state_mismatch"));QCOMPARE(track.numOfClips(),2);
		const auto raw=QJsonDocument(data).toJson();QVERIFY(raw.size()<65536);for(const auto& secret:{source.toUtf8(),clip.sourceFileIdentity().toHex(),clip.expectedSourceDigest().toHex(),QByteArray("<audio_source_window")})QVERIFY(!raw.contains(secret));
		const auto caps=call(socket,"get_audio_capabilities").value("data").toObject();QVERIFY(caps.value("trim_supported").toBool());QVERIFY(caps.value("split_supported").toBool());QCOMPARE(caps.value("range_editing").toObject().value("reference_tempo_bpm").toInt(),125);
		const auto evidence=qEnvironmentVariable("LMMS_AUDIO_RANGE_WIRE_EVIDENCE");if(!evidence.isEmpty())
		{
			QFile output(evidence);QVERIFY(output.open(QIODevice::WriteOnly|QIODevice::NewOnly));
			const QJsonObject rows{{"get_audio_clip",read},{"get_audio_capabilities",caps},{"split_preview",dry.value("data")},{"split_executed",data}};
			const auto bytes=QJsonDocument(rows).toJson();QCOMPARE(output.write(bytes),qint64(bytes.size()));
		}
	}
	void socketSemanticErrors_data()
	{QTest::addColumn<int>("mode");QTest::addColumn<QString>("code");const QStringList codes{"unsupported_tempo_map","unsupported_sample_rate_mapping","audio_window_geometry_mismatch","unsupported_audio_window_version","invalid_audio_window","destination_conflict","audio_source_missing","unrepresentable_audio_boundary"};for(int i=0;i<codes.size();++i)QTest::newRow(qPrintable(codes[i]))<<i<<codes[i];}
	void socketSemanticErrors()
	{
		QFETCH(int,mode);QFETCH(QString,code);using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);auto n=state();
		if(mode==2)n.setAttribute("len",79);if(mode==3)n.firstChildElement("audio_source_window").setAttribute("version",9);if(mode==4)n.firstChildElement("audio_source_window").setAttribute("reference_end",1);clip.loadSettings(n);
		if(mode==0)Engine::getSong()->setTempo(123);if(mode==1)Engine::audioEngine()->setAudioDevice(new RangeProbeDevice(48000),false);if(mode==5){auto* neighbor=new SampleClip(&track);neighbor->setAutoResize(false);neighbor->movePosition(150);neighbor->changeLength(10);}if(mode==6)QVERIFY(QFile::rename(source,source+".retained"));
		const auto restore=qScopeGuard([&]{if(mode==6)QFile::rename(source+".retained",source);});
		AiBridgeProjectContext ids;const auto id=ids.idForClip(&clip);AiBridgeIpcServer server(nullptr,"range-errors-"+QUuid::createUuid().toString());QVERIFY(server.start());QLocalSocket socket;socket.connectToServer(server.endpointName());QVERIFY(socket.waitForConnected(1000));const auto read=call(socket,"get_audio_clip",{{"clip_id",id}}).value("data").toObject();Engine::projectJournal()->clearJournal();
		const auto response=call(socket,"split_audio_clip",{{"clip_id",id},{"expected_audio_state_hash",read.value("audio_state_hash")},{"split_tick",mode==7?110:140}});QCOMPARE(response.value("code").toString(),code);QVERIFY(!response.value("success").toBool());QVERIFY(!Engine::projectJournal()->canUndo());
	}
	void socketTrimEdgesAndNoEffect()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);clip.loadSettings(state());AiBridgeProjectContext ids;const auto id=ids.idForClip(&clip);AiBridgeIpcServer server(nullptr,"range-trim-"+QUuid::createUuid().toString());QVERIFY(server.start());QLocalSocket socket;socket.connectToServer(server.endpointName());QVERIFY(socket.waitForConnected(1000));
		for(const auto& edge:QStringList{"left","right"})
		{
			const auto before=call(socket,"get_audio_clip",{{"clip_id",id}}).value("data").toObject();const int tick=edge=="left"?120:170;
			const auto response=call(socket,"trim_audio_clip",{{"clip_id",id},{"expected_audio_state_hash",before.value("audio_state_hash")},{"edge",edge},{"new_edge_tick",tick}});QCOMPARE(response.value("code").toString(),QString("ok"));QCOMPARE(track.numOfClips(),1);
			const auto after=call(socket,"get_audio_clip",{{"clip_id",id}}).value("data").toObject();QCOMPARE(after.value("position_ticks").toInt(),120);QCOMPARE(after.value("source_window").toObject().value("reference_end_frame").toInt(),88200);
			const auto preview=response.value("data").toObject().value("preview").toArray()[0].toObject();QCOMPARE(preview.value("boundary_tick").toInt(),tick);QCOMPARE(preview.value("boundary_frame").toInt(),edge=="left"?8820:30870);
		}
		const auto before=call(socket,"get_audio_clip",{{"clip_id",id}}).value("data").toObject();QCOMPARE(call(socket,"trim_audio_clip",{{"clip_id",id},{"expected_audio_state_hash",before.value("audio_state_hash")},{"edge","left"},{"new_edge_tick",120}}).value("code").toString(),QString("no_effect"));
	}
	void socketPartialFailureHasRealIdsAndNoReplay_data(){QTest::addColumn<bool>("post");QTest::newRow("constructor-failure")<<false;QTest::newRow("post-verification")<<true;}
	void socketPartialFailureHasRealIdsAndNoReplay()
	{
		QFETCH(bool,post);using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);clip.loadSettings(state());AiBridgeProjectContext ids;const auto id=ids.idForClip(&clip);AiBridgeIpcServer server(nullptr,"range-partial-"+QUuid::createUuid().toString());QVERIFY(server.start());QLocalSocket socket;socket.connectToServer(server.endpointName());QVERIFY(socket.waitForConnected(1000));const auto read=call(socket,"get_audio_clip",{{"clip_id",id}}).value("data").toObject();
		static int attempts;static bool corrupt;attempts=0;corrupt=post;setAudioRangeTestHook([](AudioRangeTestStage stage,int index,SampleClip* target){if(stage==AudioRangeTestStage::AfterConstruction){++attempts;if(!corrupt)throw std::runtime_error("test-only failure");}if(corrupt && stage==AudioRangeTestStage::BeforeVerification && index==1)target->movePosition(500);});
		const QJsonObject p{{"clip_id",id},{"expected_audio_state_hash",read.value("audio_state_hash")},{"split_tick",140}};const auto response=call(socket,"split_audio_clip",p);setAudioRangeTestHook(nullptr);QCOMPARE(response.value("code").toString(),post?QString("post_verification_failed"):QString("partial_failure"));const auto data=response.value("data").toObject();QCOMPARE(data.value("applied").toArray(),QJsonArray{0});QCOMPARE(data.value("failed").toArray(),QJsonArray{1});QCOMPARE(data.value("created_clip_ids").toArray().size(),1);QCOMPARE(attempts,1);QCOMPARE(track.numOfClips(),2);QTest::qWait(25);QCOMPARE(attempts,1);QCOMPARE(track.numOfClips(),2);
	}
};
QTEST_MAIN(AiBridgeAudioRangeProtocolTest)
#include "AiBridgeAudioRangeProtocolTest.moc"
