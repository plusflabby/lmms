#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QtEndian>
#include <future>
#include <cstring>
#include "AiBridgeVocalContext.h"
#include "AiBridgeVocalSource.h"
#include "AiBridgeVocalTrackActions.h"
#include "AiBridgeAudioContext.h"
#include "AudioEngine.h"
#include "AudioImportPolicy.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#ifdef Q_OS_WIN
#include <windows.h>
#endif
using namespace lmms;
class AiBridgeVocalSourceTest:public QObject
{
	Q_OBJECT
	QStringList old;
	const QStringList keys{"audio_import_roots","audio_import_max_file_bytes","audio_import_max_decoded_bytes"};
	std::unique_ptr<QTemporaryDir> dir;
	QString path;
	QByteArray bytes;
	VocalAnalyzeRequest request(SampleClip& clip)
	{
		auto prepared=AudioImportPolicy::configured().prepare(path);
		if(!prepared.audio)return {};
		if(clip.adoptAuthorizedAudio(std::move(prepared.audio))!="ok")return {};
		AiBridgeProjectContext ids;const auto id=ids.idForClip(&clip);
		auto before=AiBridgeAudioContext{}.clip(id);
		if(!before.value)return {};
		return {{{id,before.value->stateHash,{0,64}}},{}};
	}
	VocalScanResult scan(VocalSourceTicket ticket)
	{
		return std::async(std::launch::async,[ticket]{std::atomic_bool cancelled=false;
			return scanVocalSource(ticket,[](quint64,std::span<const SampleFrame>){return PcmWaveVisitDecision::Continue;},cancelled);}).get();
	}
private slots:
	void initTestCase(){Engine::init(true);}
	void cleanupTestCase(){Engine::destroy();}
	void init()
	{
		for(const auto& key:keys)old<<ConfigManager::inst()->value("aibridge",key,"vocal-absent");
		dir=std::make_unique<QTemporaryDir>(QCoreApplication::applicationDirPath()+"/vocal-source-XXXXXX");QVERIFY(dir->isValid());
		path=dir->filePath("original.wav");bytes=QByteArray(172,0);
		std::memcpy(bytes.data(),"RIFF",4);std::memcpy(bytes.data()+8,"WAVEfmt ",8);std::memcpy(bytes.data()+36,"data",4);
		qToLittleEndian<quint32>(164,bytes.data()+4);qToLittleEndian<quint32>(16,bytes.data()+16);
		qToLittleEndian<quint16>(1,bytes.data()+20);qToLittleEndian<quint16>(1,bytes.data()+22);
		qToLittleEndian<quint32>(44100,bytes.data()+24);qToLittleEndian<quint32>(88200,bytes.data()+28);
		qToLittleEndian<quint16>(2,bytes.data()+32);qToLittleEndian<quint16>(16,bytes.data()+34);qToLittleEndian<quint32>(128,bytes.data()+40);
		{QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::NewOnly));QCOMPARE(file.write(bytes),qint64(bytes.size()));}
		ConfigManager::inst()->setValue("aibridge",keys[0],QString::fromUtf8(QJsonDocument(QJsonArray{dir->path()}).toJson(QJsonDocument::Compact)));
		ConfigManager::inst()->setValue("aibridge",keys[1],"20000");ConfigManager::inst()->setValue("aibridge",keys[2],"20000");
	}
	void cleanup()
	{
		for(int i=0;i<keys.size();++i){if(old[i]=="vocal-absent")ConfigManager::inst()->deleteValue("aibridge",keys[i]);else ConfigManager::inst()->setValue("aibridge",keys[i],old[i]);}
		old.clear();AudioImportPolicy::configured().refresh();dir.reset();
	}
	void fastQtCaptureDoesNotReadOrHashSource()
	{
		SampleTrack track(Engine::getSong());SampleClip clip(&track);const auto req=request(clip);QCOMPARE(req.selections.size(),size_t(1));
#ifdef Q_OS_WIN
		const auto handle=CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
		QVERIFY(handle!=INVALID_HANDLE_VALUE);
		const auto captured=captureVocalInputs(req);CloseHandle(handle);
#else
		const auto captured=captureVocalInputs(req);
#endif
		QCOMPARE(captured.code,QString("ok"));QVERIFY(captured.value);
		QCOMPARE(captured.value->sources[0].expectedInfo.frames,quint64(64));
		QCOMPARE(validateVocalSnapshot(*captured.value),QString("ok"));
		QCOMPARE(scan(captured.value->sources[0]).code,QString("ok"));
		ConfigManager::inst()->setValue("aibridge",keys[0],"[]");
		QCOMPARE(validateVocalSnapshot(*captured.value),QString("job_invalidated"));
	}
	void workerRejectsIdentityDigestRootsAndUnsafePaths()
	{
		SampleTrack track(Engine::getSong());SampleClip clip(&track);const auto req=request(clip);
		const auto captured=captureVocalInputs(req);QCOMPARE(captured.code,QString("ok"));QVERIFY(captured.value);
		const auto original=captured.value->sources[0];auto t=original;
		t.expectedIdentity="different";QCOMPARE(scan(t).code,QString("audio_source_changed"));
		t=original;t.expectedInfo.sourceSha256=QByteArray(32,'x');QCOMPARE(scan(t).code,QString("audio_source_changed"));
		t=original;t.roots.clear();QCOMPARE(scan(t).code,QString("import_root_not_configured"));
		for(const auto* ref:{"//server/share/voice.wav","//?/C:/voice.wav","G:/CON","relative.wav"})
		{t=original;t.reference=ref;QVERIFY(scan(t).code!="ok");}
		t=original;t.limits.maxFileBytes=100;QCOMPARE(scan(t).code,QString("source_too_large"));
		const auto moved=dir->filePath("retained-original.wav");QVERIFY(QFile::rename(path,moved));
		{QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::NewOnly));QCOMPARE(file.write(bytes),qint64(bytes.size()));}
		QCOMPARE(scan(original).code,QString("audio_source_changed"));
	}
	void workerRejectsJunctionAndMalformedOrTruncatedFile()
	{
		SampleTrack track(Engine::getSong());SampleClip clip(&track);const auto captured=captureVocalInputs(request(clip));
		QCOMPARE(captured.code,QString("ok"));auto t=captured.value->sources[0];
#ifdef Q_OS_WIN
		const auto junction=dir->filePath("junction");
		QCOMPARE(QProcess::execute("C:/Windows/System32/cmd.exe",{"/d","/c","mklink","/J",QDir::toNativeSeparators(junction),QDir::toNativeSeparators(dir->path())}),0);
		t.reference=junction+"/original.wav";QVERIFY(scan(t).code!="ok");
		QVERIFY(RemoveDirectoryW(reinterpret_cast<LPCWSTR>(junction.utf16())));
#endif
		t=captured.value->sources[0];
		{QFile f(path);QVERIFY(f.open(QIODevice::WriteOnly));QCOMPARE(f.write("bad"),qint64(3));}
		QVERIFY(scan(t).code!="ok");
	}
	void snapshotRejectsStaleSelectionAndWrongThread()
	{
		SampleTrack track(Engine::getSong());SampleClip clip(&track);auto req=request(clip);
		const auto before=captureVocalInputs(req);QCOMPARE(before.code,QString("ok"));
		req.selections[0].expectedAudioHash=QString(64,'f');QCOMPARE(captureVocalInputs(req).code,QString("clip_state_mismatch"));
		req=before.value->request;req.selections[0].frames.outFrame=65;QCOMPARE(captureVocalInputs(req).code,QString("invalid_parameter"));
		QCOMPARE(std::async(std::launch::async,[req]{return captureVocalInputs(req).code;}).get(),QString("wrong_thread"));
		clip.setMuted(true);QCOMPARE(validateVocalSnapshot(*before.value),QString("clip_state_mismatch"));
	}
	void v2RetainedRangeAndQuarantineUseNativePredicate()
	{
		// Source fixture only; production capture must not create or alter V2 metadata.
		const int rate=int(Engine::audioEngine()->outputSampleRate());QVERIFY(rate==44100 || rate==48000);
		const int frames=rate*2;bytes.resize(44+frames*2);bytes.fill(0,bytes.size());
		std::memcpy(bytes.data(),"RIFF",4);std::memcpy(bytes.data()+8,"WAVEfmt ",8);std::memcpy(bytes.data()+36,"data",4);
		qToLittleEndian<quint32>(bytes.size()-8,bytes.data()+4);qToLittleEndian<quint32>(16,bytes.data()+16);
		qToLittleEndian<quint16>(1,bytes.data()+20);qToLittleEndian<quint16>(1,bytes.data()+22);
		qToLittleEndian<quint32>(rate,bytes.data()+24);qToLittleEndian<quint32>(rate*2,bytes.data()+28);
		qToLittleEndian<quint16>(2,bytes.data()+32);qToLittleEndian<quint16>(16,bytes.data()+34);qToLittleEndian<quint32>(frames*2,bytes.data()+40);
		{QFile f(path);QVERIFY(f.open(QIODevice::WriteOnly));QCOMPARE(f.write(bytes),qint64(bytes.size()));}
		ConfigManager::inst()->setValue("aibridge",keys[1],"2000000");ConfigManager::inst()->setValue("aibridge",keys[2],"2000000");
		const auto tempo=Engine::getSong()->getTempo();Engine::getSong()->setTempo(125);
		const auto restore=qScopeGuard([&]{Engine::getSong()->setTempo(tempo);});
		SampleTrack track(Engine::getSong());SampleClip clip(&track);QVERIFY(!request(clip).selections.empty());
		clip.setAutoResize(false);clip.setName("");clip.changeLength(100);
		AudioWindowGeometry geometry{0,80,-10,false,false,false,{}, {},clip.sample().frequency(),1};
		const qint64 q=rate/100;
		AudioSourceWindowV2 w{{10*q,90*q,0,frames,rate,125,rate,48,qint32(q),80,-10},100*q};
		QCOMPARE(clip.applyAudioWindow(w,geometry),QString("ok"));
		AiBridgeProjectContext ids;const auto id=ids.idForClip(&clip);const auto before=AiBridgeAudioContext{}.clip(id);QVERIFY(before.value);
		VocalAnalyzeRequest req{{{id,before.value->stateHash,{10*q,10*q+5}}},{}};
		QCOMPARE(captureVocalInputs(req).code,QString("ok"));
		QDomDocument beforeDoc;auto beforeXml=beforeDoc.createElement("sampleclip");beforeDoc.appendChild(beforeXml);clip.saveSettings(beforeDoc,beforeXml);
		const auto trackId=ids.idForTrack(&track);const auto control=captureVocalTrack(trackId);QVERIFY(control.value);
		const auto mixed=setAudioTrackProperties({trackId,control.value->hash,80.1,-0.1,false});QCOMPARE(mixed.code,QString("ok"));
		QDomDocument afterDoc;auto afterXml=afterDoc.createElement("sampleclip");afterDoc.appendChild(afterXml);clip.saveSettings(afterDoc,afterXml);
		QCOMPARE(afterDoc.toString(),beforeDoc.toString());QCOMPARE(clip.audioWindowStatus(),QString("ok"));
		{QFile f(path);QVERIFY(f.open(QIODevice::ReadOnly));QCOMPARE(f.readAll(),bytes);}
		const auto refreshed=AiBridgeAudioContext{}.clip(id);QVERIFY(refreshed.value);req.selections[0].expectedAudioHash=refreshed.value->stateHash;
		req.selections[0].frames={0,5};QCOMPARE(captureVocalInputs(req).code,QString("invalid_parameter"));
		Engine::getSong()->setTempo(126);QCOMPARE(captureVocalInputs(req).code,QString("unsupported_playback_world"));
	}
};
QTEST_MAIN(AiBridgeVocalSourceTest)
#include "AiBridgeVocalSourceTest.moc"
