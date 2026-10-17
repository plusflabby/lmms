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
#include "Timeline.h"

class RangeProbeDevice : public lmms::AudioDevice
{
public:
	explicit RangeProbeDevice(int rate):AudioDevice(2,lmms::Engine::audioEngine()){setSampleRate(rate);}
	void startProcessingImpl() override {}
	void stopProcessingImpl() override {}
};

class AiBridgeAudioRangeActionsTest : public QObject
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
	void compoundAdapterPreservesSourceAndRejectsBadWindows()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);auto n=state();n.removeChild(n.firstChildElement("audio_source_window"));clip.loadSettings(n);
		const auto identity=clip.sourceFileIdentity();const auto buffer=clip.sample().buffer();const auto before=save(clip);
		AudioSourceWindowV2 w{{4410,17640,4410,88200,44100,125,44100,48,441,30,-10},39690};
		AudioWindowGeometry g{110,30,-10,false,false,false,{}, {},440,1};auto bad=w;bad.base.outFrame=999999;
		QVERIFY(clip.applyAudioWindow(bad,g)!="ok");QCOMPARE(save(clip).attribute("len"),before.attribute("len"));QVERIFY(!clip.hasAudioSourceWindow());
		QCOMPARE(clip.applyAudioWindow(w,g),QString("ok"));QCOMPARE(clip.sourceFileIdentity(),identity);QCOMPARE(clip.sample().buffer(),buffer);QVERIFY(clip.audioWindowPlaybackView());
		QCOMPARE(clip.length().getTicks(),30);QCOMPARE(clip.audioWindowStorage().valueV2->referenceEndFrame,qint64(39690));
	}
	void dryRunZeroWritesAndDeterministicPreview()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);auto n=state();n.removeChild(n.firstChildElement("audio_source_window"));clip.loadSettings(n);
		AiBridgeProjectContext ids;const auto id=ids.idForClip(&clip);AiBridgeAudioContext context;
		auto p=prepareAudioRange({AiBridgeAudioRangeKind::Split,id,context.clip(id).value->stateHash,140});QCOMPARE(p.code,QString("ok"));
		Engine::projectJournal()->clearJournal();auto result=executeAudioRange(p,true);QCOMPARE(result.code,QString("ok"));
		QCOMPARE(result.preview,executeAudioRange(p,true).preview);QCOMPARE(result.preview.size(),2);QVERIFY(result.createdClipIds.empty());QCOMPARE(track.numOfClips(),1);QCOMPARE(clip.length().getTicks(),80);QVERIFY(!clip.hasAudioSourceWindow());QVERIFY(!Engine::projectJournal()->canUndo());
		QVERIFY(!QJsonDocument(result.toJson()).toJson().contains(source.toUtf8()));QVERIFY(!result.expectedWorldHash.isEmpty());
	}
	void compoundNotificationsOnlyExposeMatchingState()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);clip.loadSettings(state());
		bool coherent=true;int notifications=0;
		const auto inspect=[&]{++notifications;const auto& w=*clip.audioWindowStorage().valueV2;coherent=coherent && w.base.lengthTicks==clip.length().getTicks() && w.base.offsetTicks==clip.startTimeOffset().getTicks();};
		connect(&clip,&Clip::positionChanged,this,inspect);connect(&clip,&Clip::lengthChanged,this,inspect);
		AiBridgeProjectContext ids;AiBridgeAudioContext context;const auto id=ids.idForClip(&clip);
		const auto plan=prepareAudioRange({AiBridgeAudioRangeKind::TrimLeft,id,context.clip(id).value->stateHash,140});QCOMPARE(executeAudioRange(plan,false).code,QString("ok"));QVERIFY(notifications>0);QVERIFY(coherent);
	}
	void policyRevocationImmediatelyBeforePublicationHasZeroWrites()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);clip.loadSettings(state());AiBridgeProjectContext ids;AiBridgeAudioContext context;const auto id=ids.idForClip(&clip);
		const auto plan=prepareAudioRange({AiBridgeAudioRangeKind::Split,id,context.clip(id).value->stateHash,140});QCOMPARE(plan.code,QString("ok"));
		Engine::projectJournal()->clearJournal();setAudioRangeTestHook([](AudioRangeTestStage stage,int,SampleClip*){if(stage==AudioRangeTestStage::BeforeWrite)lmms::ConfigManager::inst()->setValue("aibridge","audio_import_roots","[]");});
		const auto result=executeAudioRange(plan,false);QCOMPARE(result.code,QString("import_path_denied"));QVERIFY(result.affectedClipIds.empty());QCOMPARE(clip.length().getTicks(),80);QCOMPARE(track.numOfClips(),1);QVERIFY(!Engine::projectJournal()->canUndo());
	}
	void actualSplitHasFreshIdentityExactPartitionsAndContinuity()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);auto n=state();n.removeChild(n.firstChildElement("audio_source_window"));clip.loadSettings(n);
		const auto buffer=clip.sample().buffer();AiBridgeProjectContext ids;AiBridgeAudioContext context;const auto id=ids.idForClip(&clip);
		const auto period=Engine::audioEngine()->framesPerPeriod();std::vector<SampleFrame> reference,block(period);
		{clip.setSampleStartFrame(4410);clip.setSamplePlayLength(39690);SamplePlayHandle legacy(&clip);while(reference.size()<80*441){legacy.play(block.data());reference.insert(reference.end(),block.begin(),block.end());}}
		auto p=prepareAudioRange({AiBridgeAudioRangeKind::Split,id,context.clip(id).value->stateHash,140});QCOMPARE(p.code,QString("ok"));const auto dry=executeAudioRange(p,true);
		auto result=executeAudioRange(p,false);QCOMPARE(result.code,QString("ok"));QCOMPARE(result.preview,dry.preview);QCOMPARE(result.applied,(std::vector<int>{0,1}));QCOMPARE(result.createdClipIds.size(),1);
		auto* right=dynamic_cast<SampleClip*>(ids.resolveClip(result.createdClipIds.front()));QVERIFY(right);QVERIFY(ids.idForClip(&clip)==id);QVERIFY(result.createdClipIds.front()!=id);
		QCOMPARE(right->sample().buffer(),buffer);QCOMPARE(right->sourceFileIdentity(),clip.sourceFileIdentity());QCOMPARE(right->startPosition().getTicks(),140);QCOMPARE(right->startTimeOffset().getTicks(),-40);
		QCOMPARE(right->audioWindowStorage().valueV2->referenceEndFrame,qint64(39690));QCOMPARE(right->audioWindowStorage().valueV2->base.inFrame,clip.audioWindowStorage().valueV2->base.outFrame);
		std::vector<SampleFrame> joined;for(auto* half:{&clip,right}){const auto view=half->audioWindowPlaybackView();QVERIFY(view);std::vector<SampleFrame> frames(view->outputFrameCount);renderAudioWindow(*view,0,frames);joined.insert(joined.end(),frames.begin(),frames.end());}
		QCOMPARE(joined.size(),size_t(80*441));QCOMPARE(std::memcmp(joined.data(),reference.data(),joined.size()*sizeof(SampleFrame)),0);
		QCOMPARE(save(*right).firstChildElement("audio_source_window").attribute("reference_end"),QString("39690"));
		QCOMPARE(executeAudioRange(p,false).code,QString("plan_stale"));QCOMPARE(track.numOfClips(),2);
	}
	void trimAndSplitAfterTrimPreserveEndpointAndUndo()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);clip.loadSettings(state());AiBridgeProjectContext ids;AiBridgeAudioContext context;const auto id=ids.idForClip(&clip);
		Engine::projectJournal()->clearJournal();auto p=prepareAudioRange({AiBridgeAudioRangeKind::TrimLeft,id,context.clip(id).value->stateHash,120});QCOMPARE(p.code,QString("ok"));QCOMPARE(executeAudioRange(p,false).code,QString("ok"));
		QCOMPARE(clip.startPosition().getTicks(),120);QCOMPARE(clip.audioWindowStorage().valueV2->referenceEndFrame,qint64(88200));QVERIFY(Engine::projectJournal()->canUndo());Engine::projectJournal()->undo();
		QCOMPARE(clip.startPosition().getTicks(),110);QCOMPARE(clip.length().getTicks(),80);QCOMPARE(clip.audioWindowStorage().valueV2->referenceEndFrame,qint64(88200));
		p=prepareAudioRange({AiBridgeAudioRangeKind::TrimRight,id,context.clip(id).value->stateHash,170});QCOMPARE(executeAudioRange(p,false).code,QString("ok"));
		p=prepareAudioRange({AiBridgeAudioRangeKind::Split,id,context.clip(id).value->stateHash,140});auto result=executeAudioRange(p,false);QCOMPARE(result.code,QString("ok"));auto* right=dynamic_cast<SampleClip*>(ids.resolveClip(result.createdClipIds.front()));QVERIFY(right);
		QCOMPARE(right->audioWindowStorage().valueV2->base.outFrame,qint64(30870));QCOMPARE(right->audioWindowStorage().valueV2->referenceEndFrame,qint64(88200));
		SampleTrack restored(Engine::getSong());SampleClip reloaded(&restored);reloaded.loadSettings(save(*right));QCOMPARE(reloaded.audioWindowStorage().valueV2->referenceEndFrame,qint64(88200));QCOMPARE(reloaded.audioWindowStatus(),QString("ok"));
	}
	void nativeArrangementWholeStreamEquality_data()
	{
		QTest::addColumn<int>("rate");QTest::addColumn<int>("channels");for(int r:{44100,48000})for(int c:{1,2})QTest::newRow(qPrintable(QString::number(r)+"-"+QString::number(c)))<<r<<c;
	}
	void nativeArrangementWholeStreamEquality()
	{
		QFETCH(int,rate);QFETCH(int,channels);using namespace lmms;auto* song=Engine::getSong();auto* engine=Engine::audioEngine();engine->setAudioDevice(new RangeProbeDevice(rate),false);
		const int frames=rate*2,align=2*channels,q=rate/100;QByteArray bytes(44+frames*align,0);
		std::memcpy(bytes.data(),"RIFF",4);std::memcpy(bytes.data()+8,"WAVEfmt ",8);std::memcpy(bytes.data()+36,"data",4);
		qToLittleEndian<quint32>(bytes.size()-8,bytes.data()+4);qToLittleEndian<quint32>(16,bytes.data()+16);qToLittleEndian<quint16>(1,bytes.data()+20);qToLittleEndian<quint16>(channels,bytes.data()+22);qToLittleEndian<quint32>(rate,bytes.data()+24);qToLittleEndian<quint32>(rate*align,bytes.data()+28);qToLittleEndian<quint16>(align,bytes.data()+32);qToLittleEndian<quint16>(16,bytes.data()+34);qToLittleEndian<quint32>(frames*align,bytes.data()+40);
		for(int i=0;i<frames;++i)for(int ch=0;ch<channels;++ch)qToLittleEndian<qint16>((i*(ch?3571u:7919u))%29009u-14504,bytes.data()+44+i*align+ch*2);
		const QString recording=directory+"/comparison.wav";{QFile f(recording);QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::NewOnly));QCOMPARE(f.write(bytes),qint64(bytes.size()));}
		SampleTrack referenceTrack(song),editedTrack(song);SampleClip reference(&referenceTrack),edited(&editedTrack);
		for(auto* clip:{&reference,&edited}){auto audio=AudioImportPolicy::configured().prepare(recording);QCOMPARE(audio.code,QString("ok"));QCOMPARE(clip->adoptAuthorizedAudio(std::move(audio.audio)),QString("ok"));clip->setAutoResize(false);clip->movePosition(110);clip->changeLength(80);clip->setStartTimeOffset(-10);}
		const int period=engine->framesPerPeriod();
		const auto capture=[&]{song->stop();engine->clear();engine->renderNextPeriod();engine->renderNextPeriod();song->getTimeline(Song::PlayMode::Song).setLoopEnabled(false);song->playSong();song->setPlayPos(110);std::vector<SampleFrame> output;for(int base=0;base<80*q+period*3;base+=period){const auto block=engine->renderNextPeriod();output.insert(output.end(),block.begin(),block.end());}song->stop();return output;};
		editedTrack.setMuted(true);const auto original=capture();editedTrack.setMuted(false);referenceTrack.setMuted(true);
		AiBridgeProjectContext ids;AiBridgeAudioContext context;const auto id=ids.idForClip(&edited);
		for(const auto& request:std::vector<std::pair<AiBridgeAudioRangeKind,int>>{{AiBridgeAudioRangeKind::TrimLeft,111},{AiBridgeAudioRangeKind::TrimRight,189},{AiBridgeAudioRangeKind::Split,140}})
		{const auto plan=prepareAudioRange({request.first,id,context.clip(id).value->stateHash,request.second});QCOMPARE(plan.code,QString("ok"));QCOMPARE(executeAudioRange(plan,false).code,QString("ok"));}
		const auto actual=capture();QCOMPARE(actual.size(),original.size());for(size_t i=0;i<actual.size();++i){const qint64 elapsed=qint64(i)-period;const auto expected=elapsed>=q && elapsed<79*q?original[i]:SampleFrame{};QCOMPARE(std::memcmp(&actual[i],&expected,sizeof(SampleFrame)),0);}
		QFile f(recording);QVERIFY(f.open(QIODevice::ReadOnly));QCOMPARE(f.readAll(),bytes);
	}
	void staleNeighborTempoPolicyAndSourceStopBeforeJournal_data()
	{QTest::addColumn<int>("mode");for(int i=0;i<5;++i)QTest::newRow(qPrintable(QString::number(i)))<<i;}
	void staleNeighborTempoPolicyAndSourceStopBeforeJournal()
	{
		QFETCH(int,mode);using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);clip.loadSettings(state());AiBridgeProjectContext ids;AiBridgeAudioContext context;const auto id=ids.idForClip(&clip);
		auto p=prepareAudioRange({AiBridgeAudioRangeKind::Split,id,context.clip(id).value->stateHash,140});QCOMPARE(p.code,QString("ok"));
		if(mode==0){auto* neighbor=new SampleClip(&track);neighbor->setAutoResize(false);neighbor->movePosition(300);neighbor->changeLength(10);}else if(mode==1)Engine::getSong()->setTempo(123);else if(mode==2)ConfigManager::inst()->setValue("aibridge",keys[0],"[]");else {QVERIFY(QFile::rename(source,source+".retained"));if(mode==4)QVERIFY(QFile::copy(source+".retained",source));}
		Engine::projectJournal()->clearJournal();auto result=executeAudioRange(p,false);QVERIFY(result.code!="ok");QVERIFY(result.applied.empty());QVERIFY(result.createdClipIds.empty());QVERIFY(!Engine::projectJournal()->canUndo());QCOMPARE(clip.length().getTicks(),80);
		if(mode==4)QVERIFY(QFile::rename(source,source+".replacement"));
		if(mode>=3)QVERIFY(QFile::rename(source+".retained",source));
	}
	void conversionUndoRestoresLegacyAndAllSourceReferences()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);auto node=state();node.removeChild(node.firstChildElement("audio_source_window"));clip.loadSettings(node);
		const auto identity=clip.sourceFileIdentity();const auto reference=clip.sampleFile();AiBridgeProjectContext ids;AiBridgeAudioContext context;const auto id=ids.idForClip(&clip);Engine::projectJournal()->clearJournal();
		const auto plan=prepareAudioRange({AiBridgeAudioRangeKind::TrimRight,id,context.clip(id).value->stateHash,140});QCOMPARE(executeAudioRange(plan,false).code,QString("ok"));QVERIFY(clip.hasAudioSourceWindow());
		QVERIFY(Engine::projectJournal()->canUndo());Engine::projectJournal()->undo();QVERIFY(!clip.hasAudioSourceWindow());QCOMPARE(clip.length().getTicks(),80);QCOMPARE(clip.startTimeOffset().getTicks(),-10);QCOMPARE(clip.sampleFile(),reference);QCOMPARE(clip.sourceFileIdentity(),identity);
	}
	void tamperedInternalPlansCannotReachNativeJournal()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);clip.loadSettings(state());AiBridgeProjectContext ids;AiBridgeAudioContext context;const auto id=ids.idForClip(&clip);
		auto plan=prepareAudioRange({AiBridgeAudioRangeKind::Split,id,context.clip(id).value->stateHash,140});QCOMPARE(plan.code,QString("ok"));plan.steps[1].window.referenceEndFrame-=441;
		Engine::projectJournal()->clearJournal();QCOMPARE(executeAudioRange(plan,false).code,QString("invalid_argument"));QCOMPARE(track.numOfClips(),1);QCOMPARE(clip.length().getTicks(),80);QVERIFY(!Engine::projectJournal()->canUndo());
	}
	void precisePartialFailure_data()
	{QTest::addColumn<int>("mode");for(int i=0;i<3;++i)QTest::newRow(qPrintable(QString::number(i)))<<i;}
	void precisePartialFailure()
	{
		QFETCH(int,mode);using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);clip.loadSettings(state());AiBridgeProjectContext ids;AiBridgeAudioContext context;const auto id=ids.idForClip(&clip);
		auto p=prepareAudioRange({AiBridgeAudioRangeKind::Split,id,context.clip(id).value->stateHash,140});QCOMPARE(p.code,QString("ok"));
		static int failureMode;failureMode=mode;setAudioRangeTestHook([](AudioRangeTestStage stage,int index,SampleClip* target){if((failureMode==0 && stage==AudioRangeTestStage::AfterPublication && index==0) || (failureMode==1 && stage==AudioRangeTestStage::AfterConstruction)){throw std::runtime_error("injected");}if(failureMode==2 && stage==AudioRangeTestStage::BeforeVerification && index==1)target->movePosition(500);});
		auto result=executeAudioRange(p,false);setAudioRangeTestHook(nullptr);QCOMPARE(result.code,mode==2?QString("post_verification_failed"):QString("partial_failure"));
		QCOMPARE(result.failed,(std::vector<int>{mode==0?0:1}));QCOMPARE(result.applied,mode==0?std::vector<int>{}:std::vector<int>{0});QCOMPARE(result.unapplied,mode==0?std::vector<int>{1}:std::vector<int>{});
		QCOMPARE(clip.length().getTicks(),30);QCOMPARE(track.numOfClips(),mode==0?1:2);QCOMPARE(result.createdClipIds.size(),mode==0?0:1);QVERIFY(!result.observed.empty());QVERIFY(Engine::getSong()->isModified());
		if(mode==0)QCOMPARE(result.expectedWorldHash,result.observedWorldHash);
		if(mode>0)QCOMPARE(result.expected[1].toObject()["clip_id"].toString(),result.createdClipIds.front());
		QCOMPARE(executeAudioRange(p,false).code,QString("plan_stale"));QCOMPARE(track.numOfClips(),mode==0?1:2);
	}
	void nativeNotificationExceptionReportsActualAffectedClip()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);clip.loadSettings(state());AiBridgeProjectContext ids;AiBridgeAudioContext context;const auto id=ids.idForClip(&clip);
		const auto plan=prepareAudioRange({AiBridgeAudioRangeKind::TrimRight,id,context.clip(id).value->stateHash,140});QCOMPARE(plan.code,QString("ok"));
		const auto connection=connect(&clip,&SampleClip::sampleChanged,this,[]{throw std::runtime_error("native notification");});
		const auto result=executeAudioRange(plan,false);disconnect(connection);QCOMPARE(result.code,QString("partial_failure"));QCOMPARE(result.failed,(std::vector<int>{0}));QCOMPARE(result.affectedClipIds,QStringList{id});QCOMPARE(clip.length().getTicks(),30);
	}
};
QTEST_MAIN(AiBridgeAudioRangeActionsTest)
#include "AiBridgeAudioRangeActionsTest.moc"
