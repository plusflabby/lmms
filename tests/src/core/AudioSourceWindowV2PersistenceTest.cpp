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

class AudioSourceWindowV2PersistenceTest : public QObject
{
	Q_OBJECT
	QString directory, source;
	QByteArray digest;
	QStringList old;
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
	void initTestCase() { lmms::Engine::init(true); lmms::Engine::audioEngine()->audioDev()->stopProcessing(); }
	void cleanupTestCase() { lmms::Engine::destroy(); }
	void init()
	{
		using namespace lmms; Engine::getSong()->setTempo(125);
		for(const auto& key:keys) old<<ConfigManager::inst()->value("aibridge",key,"window-test-absent");
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
		for(int i=0;i<keys.size();++i) {if(old[i]=="window-test-absent") ConfigManager::inst()->deleteValue("aibridge",keys[i]); else ConfigManager::inst()->setValue("aibridge",keys[i],old[i]);}
		old.clear(); AudioImportPolicy::configured().refresh();
	}
	void roundTripsV2WithoutCurrentTempoRebasing()
	{
		using namespace lmms; const auto original=state(); QVERIFY(!original.isNull());
		SampleTrack track(Engine::getSong()); SampleClip clip(&track); clip.loadSettings(original);
		for(int tempo:{123,130,125})
		{
			Engine::getSong()->setTempo(tempo);
			auto saved=save(clip); auto range=saved.firstChildElement("audio_source_window");
			QVERIFY2(!range.isNull(),"Native source window must survive save instead of being discarded");
			QCOMPARE(range.attribute("in"),QString("4410")); QCOMPARE(range.attribute("out"),QString("39690"));
			QCOMPARE(range.attribute("origin"),QString("4410")); QCOMPARE(range.attribute("reference_bpm"),QString("125"));
			QCOMPARE(range.attribute("reference_end"),QString("88200"));
			QCOMPARE(clip.audioWindowStorage().parseCode,QString("ok"));
			QCOMPARE(saved.attribute("off"),QString("-10")); QCOMPARE(saved.attribute("len"),QString("80"));
			clip.loadSettings(saved);
			QCOMPARE(save(clip).attribute("source_file_id"),original.attribute("source_file_id"));
		}
	}
	void nativeCloneRetainsSourceWindowAndOwner()
	{
		using namespace lmms; SampleTrack track(Engine::getSong()),target(Engine::getSong()); SampleClip clip(&track);
		const auto original=state(); clip.loadSettings(original);
		std::unique_ptr<SampleClip> copy(clip.cloneToTrack(&target)); QVERIFY(copy);
		QCOMPARE(copy->getTrack(),static_cast<Track*>(&target));
		QCOMPARE(save(*copy).firstChildElement("audio_source_window").attribute("in"),QString("4410"));
		QCOMPARE(copy->sourceFileIdentity(),clip.sourceFileIdentity()); QCOMPARE(copy->expectedSourceDigest(),clip.expectedSourceDigest());
		QCOMPARE(copy->audioWindowStorage().parseCode,QString("ok"));
		QCOMPARE(save(*copy).firstChildElement("audio_source_window").attribute("reference_end"),QString("88200"));
	}
	void nativeJournalRetainsReferenceEndpoint()
	{
		using namespace lmms; SampleTrack track(Engine::getSong()); SampleClip clip(&track);
		clip.loadSettings(state()); QCOMPARE(clip.audioWindowStorage().parseCode,QString("ok"));
		clip.addJournalCheckPoint(); clip.movePosition(192); Engine::projectJournal()->undo();
		QCOMPARE(clip.startPosition().getTicks(),110);
		QVERIFY(clip.audioWindowStorage().valueV2.has_value());
		QCOMPARE(clip.audioWindowStorage().valueV2->referenceEndFrame,qint64(88200));
		QCOMPARE(save(clip).firstChildElement("audio_source_window").attribute("reference_end"),QString("88200"));
	}
	void missingChangedSourceRetainsWindowAndA4Identity()
	{
		using namespace lmms; SampleTrack track(Engine::getSong()); SampleClip clip(&track);
		auto absent=state(); const auto identity=absent.attribute("source_file_id"),sha=absent.attribute("source_sha256");
		absent.setAttribute("src",directory+"/absent.wav"); clip.loadSettings(absent);
		QCOMPARE(clip.sourceLoadError(),QString("audio_source_missing"));
		auto saved=save(clip); QCOMPARE(saved.firstChildElement("audio_source_window").attribute("out"),QString("39690"));
		QCOMPARE(saved.attribute("source_file_id"),identity); QCOMPARE(saved.attribute("source_sha256"),sha);
		auto changed=state(); changed.setAttribute("source_file_id",QString(64,'0')); clip.loadSettings(changed);
		QCOMPARE(clip.sourceLoadError(),QString("audio_source_changed"));
		QCOMPARE(save(clip).firstChildElement("audio_source_window").attribute("in"),QString("4410"));
		QCOMPARE(save(clip).attribute("source_file_id"),QString(64,'0'));
	}
	void strictMetadataNeverFallsBack_data()
	{
		QTest::addColumn<QString>("attribute"); QTest::addColumn<QString>("value"); QTest::addColumn<QString>("code");
		QTest::newRow("future")<<"version"<<"3"<<"unsupported_audio_window_version";
		QTest::newRow("phase")<<"phase"<<"future"<<"unsupported_audio_window_version";
		for(const auto& value:QStringList{"+4410","04410","4.41e3"," 4410","-0","9223372036854775808","-1","4411"})
			QTest::newRow(qPrintable(value))<<"in"<<value<<"invalid_audio_window";
		QTest::newRow("unknown")<<"unknown"<<"1"<<"invalid_audio_window";
		QTest::newRow("empty")<<"out"<<"4410"<<"invalid_audio_window";
		QTest::newRow("rate")<<"source_rate"<<"48000"<<"invalid_audio_window";
		QTest::newRow("duplicate")<<"__duplicate"<<""<<"invalid_audio_window";
		QTest::newRow("missing")<<"__missing"<<""<<"invalid_audio_window";
		QTest::newRow("child")<<"__child"<<""<<"invalid_audio_window";
		QTest::newRow("text")<<"__text"<<""<<"invalid_audio_window";
		for(const auto& token:QStringList{"0","39689","88201","9223372036854775808","+88200","088200","-1","88200 ","1e5","44101"})
			QTest::newRow(qPrintable("reference-end-"+token))<<"reference_end"<<token<<"invalid_audio_window";
		QTest::newRow("missing-reference")<<"__missing_reference"<<""<<"invalid_audio_window";
	}
	void strictMetadataNeverFallsBack()
	{
		QFETCH(QString,attribute); QFETCH(QString,value); QFETCH(QString,code);
		using namespace lmms; auto node=state(); auto range=node.firstChildElement("audio_source_window");
		if(attribute=="__duplicate") node.appendChild(range.cloneNode(true));
		else if(attribute=="__missing") range.removeAttribute("in");
		else if(attribute=="__missing_reference") range.removeAttribute("reference_end");
		else if(attribute=="__child") range.appendChild(node.ownerDocument().createElement("unknown"));
		else if(attribute=="__text") range.appendChild(node.ownerDocument().createTextNode("not whitespace"));
		else range.setAttribute(attribute,value);
		SampleTrack track(Engine::getSong()); SampleClip clip(&track); clip.loadSettings(node);
		QCOMPARE(clip.property("audioWindowStatus").toString(),code);
		if(attribute=="__duplicate") QCOMPARE(save(clip).elementsByTagName("audio_source_window").size(),2);
		else if(!attribute.startsWith("__")) QCOMPARE(save(clip).firstChildElement("audio_source_window").attribute(attribute),value);
		else QCOMPARE(save(clip).firstChildElement("audio_source_window").text(),range.text());
	}
	void taggedSourceRequiresBoundedIdentityBeforeLoading()
	{
		using namespace lmms;
		for(const auto& attribute:QStringList{"bounded_source","source_file_id","source_sha256"})
		{
			auto node=state(); node.removeAttribute(attribute);
			SampleTrack track(Engine::getSong()); SampleClip clip(&track); clip.loadSettings(node);
			QCOMPARE(clip.sample().sampleSize(),size_t(0));
			QCOMPARE(clip.property("audioWindowStatus").toString(),QString("unsupported_audio_operation"));
			QCOMPARE(save(clip).firstChildElement("audio_source_window").attribute("in"),QString("4410"));
		}
	}
	void sourceCountMismatchCannotRebindWindow()
	{
		using namespace lmms; auto node=state(); node.firstChildElement("audio_source_window").setAttribute("source_frames","88201");
		SampleTrack track(Engine::getSong()); SampleClip clip(&track); clip.loadSettings(node);
		QCOMPARE(clip.property("audioWindowStatus").toString(),QString("audio_source_changed"));
		QCOMPARE(save(clip).firstChildElement("audio_source_window").attribute("source_frames"),QString("88201"));
	}
	void genericSourceReplacementDoesNotDiscardWindow()
	{
		using namespace lmms; SampleTrack track(Engine::getSong()); SampleClip clip(&track); clip.loadSettings(state());
		const auto identity=clip.sourceFileIdentity(); clip.setSampleFile(directory+"/absent.wav");
		QCOMPARE(clip.sampleFile(),source); QCOMPARE(clip.sourceFileIdentity(),identity); QCOMPARE(clip.sample().sampleSize(),size_t(88200));
		QCOMPARE(save(clip).firstChildElement("audio_source_window").attribute("in"),QString("4410"));
	}
	void taggedPlaybackIsBlockedBeforeRendererExists()
	{
		using namespace lmms; SampleTrack track(Engine::getSong()); SampleClip clip(&track); clip.loadSettings(state());
		const bool started=track.play(110,1,0,-1); Engine::audioEngine()->renderNextPeriod(); Engine::audioEngine()->clear(); Engine::audioEngine()->renderNextPeriod();
		QVERIFY(!started); QCOMPARE(clip.property("audioWindowStatus").toString(),QString("unsupported_audio_operation"));
	}
	void untaggedLegacyStateStaysUntagged()
	{
		using namespace lmms; auto node=state(); node.removeChild(node.firstChildElement("audio_source_window"));
		SampleTrack track(Engine::getSong()); SampleClip clip(&track); clip.loadSettings(node);
		QVERIFY(save(clip).firstChildElement("audio_source_window").isNull());
		QCOMPARE(clip.sample().sampleSize(),size_t(88200)); QCOMPARE(clip.startTimeOffset().getTicks(),-10);
	}
	void directClipHandleCannotBypassTaggedPlaybackGuard()
	{
		using namespace lmms; SampleTrack track(Engine::getSong()); SampleClip clip(&track); clip.loadSettings(state());
		SamplePlayHandle handle(&clip);
		std::vector<SampleFrame> output(Engine::audioEngine()->framesPerPeriod()); handle.play(output.data());
		QVERIFY(std::all_of(output.begin(),output.end(),[](const auto& f){return f[0]==0 && f[1]==0;}));
		QCOMPARE(handle.totalFrames(),f_cnt_t(0));
	}
};
QTEST_MAIN(AudioSourceWindowV2PersistenceTest)
#include "AudioSourceWindowV2PersistenceTest.moc"
