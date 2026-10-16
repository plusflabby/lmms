#include <QtTest>
#include <QApplication>
#include <QTemporaryDir>
#include <QProcess>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include "AiBridgeAudioContext.h"
#include "AudioImportPolicy.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "SampleTrack.h"
#include "SampleClip.h"
#include "Song.h"
class AiBridgeAudioLifecycleTest : public QObject
{
	Q_OBJECT
private slots:
	void initTestCase() { lmms::Engine::init(true); }
	void cleanupTestCase() { lmms::Engine::destroy(); }
	void liveIdentityIsSharedAndDeletionCannotRetargetRecreatedObjects()
	{
		using namespace lmms;AiBridgeProjectContext ids;AiBridgeAudioContext first,second;
		auto* track=new SampleTrack(Engine::getSong());auto* clip=new SampleClip(track);
		const auto trackId=ids.idForTrack(track),clipId=ids.idForClip(clip);
		auto initial=first.clip(clipId);QCOMPARE(initial.code,QStringLiteral("ok"));
		QCOMPARE(second.clip(clipId).value->stateHash,initial.value->stateHash);
		delete track;
		QCOMPARE(first.clip(clipId).code,QStringLiteral("stale_id"));QCOMPARE(first.clips(trackId,0,1).code,QStringLiteral("stale_id"));
		SampleTrack replacement(Engine::getSong());SampleClip recreated(&replacement);
		QVERIFY(ids.idForTrack(&replacement)!=trackId);const auto newId=ids.idForClip(&recreated);QVERIFY(newId!=clipId);
		QCOMPARE(first.clip(newId).code,QStringLiteral("ok"));QCOMPARE(first.validate(newId,initial.value->stateHash),QStringLiteral("clip_state_mismatch"));
	}
	void realNativeSaveReloadAdvancesGenerationAndPreservesAudioState()
	{
		using namespace lmms;const QStringList keys{"audio_import_roots","audio_import_max_file_bytes","audio_import_max_decoded_bytes"};QStringList old;
		for(auto& k:keys)old<<ConfigManager::inst()->value("aibridge",k,"test-absent");
		const auto source=QFileInfo(QFINDTESTDATA("../../../data/samples/shapes/vowel_u.wav")).canonicalFilePath();
		ConfigManager::inst()->setValue("aibridge",keys[0],QString::fromUtf8(QJsonDocument(QJsonArray{QFileInfo(source).absolutePath()}).toJson(QJsonDocument::Compact)));
		ConfigManager::inst()->setValue("aibridge",keys[1],"20000");ConfigManager::inst()->setValue("aibridge",keys[2],"20000");
		QTemporaryDir directory(QCoreApplication::applicationDirPath()+"/audio-reload-XXXXXX");QVERIFY(directory.isValid());
		auto* track=new SampleTrack(Engine::getSong());auto* clip=new SampleClip(track);
		auto prepared=AudioImportPolicy::configured().prepare(source);QCOMPARE(prepared.code,QStringLiteral("ok"));QCOMPARE(clip->adoptAuthorizedAudio(std::move(prepared.audio)),QStringLiteral("ok"));
		clip->setAutoResize(false);clip->movePosition(540);clip->changeLength(40);clip->setStartTimeOffset(-10);
		AiBridgeProjectContext ids;AiBridgeAudioContext context;auto oldId=ids.idForClip(clip);auto before=context.clip(oldId);QCOMPARE(before.code,QStringLiteral("ok"));
		const auto path=directory.filePath("project.mmp");QVERIFY(!QFileInfo::exists(path));QVERIFY(Engine::getSong()->saveProjectFile(path));
		Engine::getSong()->loadProject(path);QCOMPARE(context.clip(oldId).code,QStringLiteral("stale_id"));
		auto tracks=context.tracks(0,256);QCOMPARE(tracks.code,QStringLiteral("ok"));QCOMPARE(tracks.page.total,1);
		auto clips=context.clips(tracks.items[0].id,0,256);QCOMPARE(clips.code,QStringLiteral("ok"));QCOMPARE(clips.page.total,1);
		const auto& after=clips.items[0];QVERIFY(after.generation>before.value->generation);QVERIFY(after.state.id!=oldId);
		QCOMPARE(after.state.position,qint64(540));QCOMPARE(after.state.length,qint64(40));QCOMPARE(after.state.offset,qint64(-10));
		QCOMPARE(after.state.sourceFrames,quint64(337));QVERIFY(after.sourceVerified);QCOMPARE(after.state.sourceIdentity,before.value->state.sourceIdentity);
		QCOMPARE(context.validate(after.state.id,before.value->stateHash),QStringLiteral("clip_state_mismatch"));
		for(auto* t:Engine::getSong()->tracks()) if(t->type()==Track::Type::Sample){delete t;break;}
		for(int i=0;i<keys.size();++i){if(old[i]=="test-absent")ConfigManager::inst()->deleteValue("aibridge",keys[i]);else ConfigManager::inst()->setValue("aibridge",keys[i],old[i]);}
		AudioImportPolicy::configured().refresh();
	}
	void freshProcessesRejectEarlierSessionsIndependently()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);AiBridgeProjectContext ids;const auto oldId=ids.idForClip(&clip);
		QTemporaryDir directory(QCoreApplication::applicationDirPath()+"/audio-process-XXXXXX");QVERIFY(directory.isValid());QString previous;
		for(int i=0;i<2;++i)
		{
			const auto path=directory.filePath(QString::number(i)+".json");QProcess child;
			child.start(QCoreApplication::applicationFilePath(),{"--audio-identity-probe",path,oldId});
			if(!child.waitForFinished(15000)){child.kill();child.waitForFinished();QFAIL("Fresh native identity process timed out");}
			QCOMPARE(child.exitCode(),0);QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));auto result=QJsonDocument::fromJson(file.readAll()).object();
			QCOMPARE(result.value("old_code").toString(),QStringLiteral("stale_id"));const auto current=result.value("clip_id").toString();
			QVERIFY(!current.isEmpty());QVERIFY(current!=oldId);QVERIFY(current!=previous);previous=current;
		}
	}
};
int main(int argc,char** argv)
{
	QApplication app(argc,argv);const auto args=app.arguments();
	if(args.size()==4 && args[1]=="--audio-identity-probe")
	{
		using namespace lmms;Engine::init(true);auto* track=new SampleTrack(Engine::getSong());auto* clip=new SampleClip(track);AiBridgeProjectContext ids;
		const auto id=ids.idForClip(clip);const auto status=AiBridgeAudioContext{}.clip(args[3]).code;
		QFile file(args[2]);if(!file.open(QIODevice::WriteOnly|QIODevice::NewOnly)){Engine::destroy();return 2;}
		file.write(QJsonDocument(QJsonObject{{"clip_id",id},{"old_code",status}}).toJson(QJsonDocument::Compact));file.close();Engine::destroy();return 0;
	}
	AiBridgeAudioLifecycleTest test;return QTest::qExec(&test,argc,argv);
}
#include "AiBridgeAudioLifecycleTest.moc"
