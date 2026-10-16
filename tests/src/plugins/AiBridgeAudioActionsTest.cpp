#include <QtTest>
#include <QtEndian>
#include <QTemporaryDir>
#include <QFile>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <cstring>
#include "AiBridgeAudioActions.h"
#include "AudioImportPolicy.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "ProjectJournal.h"
#include "AutomationTrack.h"
#include "AutomationClip.h"
#include "InstrumentTrack.h"
class InterferingSampleTrack : public lmms::SampleTrack
{
public:
	using SampleTrack::SampleTrack;
	int creations=0;
	QString property;
	lmms::Clip* createClip(const lmms::TimePos& pos) override
	{
		++creations;auto* clip=static_cast<lmms::SampleClip*>(SampleTrack::createClip(pos));
		QObject::connect(clip,&lmms::SampleClip::sampleChanged,clip,[clip,owner=this,p=property] {
			if(p=="muted")clip->setMuted(true);else if(p=="track_volume")owner->volumeModel()->setValue(12);else clip->sample().setAmplification(0.5f);
		});
		return clip;
	}
};
class AiBridgeAudioActionsTest : public QObject
{
	Q_OBJECT
	QStringList old;
	const QStringList keys{"audio_import_roots","audio_import_max_file_bytes","audio_import_max_decoded_bytes"};
	std::unique_ptr<QTemporaryDir> dir;
	QString source;
	QByteArray bytes;
	int oldTempo;
	bool oldJournalling;
private slots:
	void initTestCase() {lmms::Engine::init(true);}
	void cleanupTestCase() {lmms::Engine::destroy();}
	void init()
	{
		using namespace lmms;oldTempo=Engine::getSong()->getTempo();Engine::getSong()->setTempo(120);
		oldJournalling=Engine::projectJournal()->isJournalling();Engine::projectJournal()->setJournalling(true);
		for(auto& key:keys)old<<ConfigManager::inst()->value("aibridge",key,"test-absent");
		dir=std::make_unique<QTemporaryDir>(QCoreApplication::applicationDirPath()+"/audio-actions-XXXXXX");QVERIFY(dir->isValid());source=dir->filePath("Verse.wav");
		bytes=QByteArray(44+88200,0);std::memcpy(bytes.data(),"RIFF",4);std::memcpy(bytes.data()+8,"WAVEfmt ",8);std::memcpy(bytes.data()+36,"data",4);
		qToLittleEndian<quint32>(88236,bytes.data()+4);qToLittleEndian<quint32>(16,bytes.data()+16);qToLittleEndian<quint16>(1,bytes.data()+20);qToLittleEndian<quint16>(1,bytes.data()+22);
		qToLittleEndian<quint32>(44100,bytes.data()+24);qToLittleEndian<quint32>(88200,bytes.data()+28);qToLittleEndian<quint16>(2,bytes.data()+32);qToLittleEndian<quint16>(16,bytes.data()+34);qToLittleEndian<quint32>(88200,bytes.data()+40);
		{QFile f(source);QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::NewOnly));QCOMPARE(f.write(bytes),qint64(bytes.size()));}
		ConfigManager::inst()->setValue("aibridge",keys[0],QString::fromUtf8(QJsonDocument(QJsonArray{dir->path()}).toJson(QJsonDocument::Compact)));
		ConfigManager::inst()->setValue("aibridge",keys[1],"1000000");ConfigManager::inst()->setValue("aibridge",keys[2],"1000000");
	}
	void cleanup()
	{
		using namespace lmms;
		for(int i=0;i<keys.size();++i){if(old[i]=="test-absent")ConfigManager::inst()->deleteValue("aibridge",keys[i]);else ConfigManager::inst()->setValue("aibridge",keys[i],old[i]);}
		old.clear();AudioImportPolicy::configured().refresh();dir.reset();Engine::getSong()->setTempo(oldTempo);
		Engine::projectJournal()->setJournalling(oldJournalling);
	}
	void trackDryRunAndExecutionUseTheSameConcreteProperties()
	{
		using namespace lmms;AiBridgeAudioActions actions;AiBridgeAudioTrackRequest request;request.name="Vocals";request.volume=73;request.panning=-11;request.muted=true;request.dryRun=true;
		const auto count=Engine::getSong()->tracks().size();auto preview=actions.createTrack(request);QCOMPARE(preview.code,QStringLiteral("ok"));QCOMPARE(Engine::getSong()->tracks().size(),count);QVERIFY(preview.trackId.isEmpty());
		request.dryRun=false;auto result=actions.createTrack(request);QCOMPARE(result.code,QStringLiteral("ok"));QCOMPARE(result.preview,preview.preview);QVERIFY(!result.trackId.isEmpty());
		AiBridgeProjectContext ids;auto* track=dynamic_cast<SampleTrack*>(ids.resolveTrack(result.trackId));QVERIFY(track);QCOMPARE(track->displayName(),QStringLiteral("Vocals"));QCOMPARE(track->volumeModel()->value(),73.0f);QCOMPARE(track->panningModel()->value(),-11.0f);QVERIFY(track->isMuted());delete track;
	}
	void importPreviewCreatesNothingAndExecutionUsesAuthorizedNativeAudio()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());AiBridgeProjectContext ids;AiBridgeAudioActions actions;
		AiBridgeAudioImportRequest request{ids.idForTrack(&track),source,960,true};QVERIFY(Engine::getSong()->guiSaveProjectAs(dir->filePath("before-import.mmp")));Engine::projectJournal()->clearJournal();const bool modifiedBefore=Engine::getSong()->isModified();QVERIFY(!modifiedBefore);
		auto preview=actions.importFile(request);QCOMPARE(preview.code,QStringLiteral("ok"));QCOMPARE(track.numOfClips(),0);QCOMPARE(Engine::getSong()->isModified(),modifiedBefore);
		QVERIFY(!Engine::projectJournal()->canUndo());
		QCOMPARE(preview.preview.value("length_ticks").toInt(),96);QCOMPARE(preview.preview.value("position_ticks").toInt(),960);
		request.dryRun=false;auto result=actions.importFile(request);QCOMPARE(result.code,QStringLiteral("ok"));QCOMPARE(result.preview,preview.preview);QCOMPARE(track.numOfClips(),1);QVERIFY(Engine::getSong()->isModified());
		auto* clip=dynamic_cast<SampleClip*>(ids.resolveClip(result.clipId));QVERIFY(clip);QVERIFY(clip->usesBoundedSampleSource());QCOMPARE(clip->sample().sampleSize(),size_t(44100));QCOMPARE(clip->startPosition().getTicks(),960);QCOMPARE(clip->length().getTicks(),96);
		QCOMPARE(result.observed.value("source_label").toString(),QStringLiteral("Verse.wav"));QVERIFY(!QJsonDocument(result.observed).toJson().contains(dir->path().toUtf8()));
		QFile f(source);QVERIFY(f.open(QIODevice::ReadOnly));QCOMPARE(f.readAll(),bytes);
		QVERIFY(Engine::projectJournal()->canUndo());Engine::projectJournal()->undo();QCOMPARE(track.numOfClips(),0);QCOMPARE(AiBridgeAudioContext{}.clip(result.clipId).code,QStringLiteral("stale_id"));
	}
	void deniedMalformedAndCollidingSourcesRejectBeforeNativeWrites()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());AiBridgeProjectContext ids;AiBridgeAudioActions actions;AiBridgeAudioImportRequest request{ids.idForTrack(&track),source,0,false};
		ConfigManager::inst()->setValue("aibridge",keys[0],"[]");QVERIFY(actions.importFile(request).code!="ok");QCOMPARE(track.numOfClips(),0);
		ConfigManager::inst()->setValue("aibridge",keys[0],QString::fromUtf8(QJsonDocument(QJsonArray{dir->path()}).toJson(QJsonDocument::Compact)));
		ConfigManager::inst()->setValue("aibridge",keys[1],"1");QCOMPARE(actions.importFile(request).code,QStringLiteral("source_too_large"));QCOMPARE(track.numOfClips(),0);
		ConfigManager::inst()->setValue("aibridge",keys[1],"1000000");auto result=actions.importFile(request);QCOMPARE(result.code,QStringLiteral("ok"));
		QCOMPARE(actions.importFile(request).code,QStringLiteral("destination_conflict"));QCOMPARE(track.numOfClips(),1);
		request.position=-1;QCOMPARE(actions.importFile(request).code,QStringLiteral("invalid_argument"));QCOMPARE(track.numOfClips(),1);
		request.position=std::numeric_limits<qint32>::max();QCOMPARE(actions.importFile(request).code,QStringLiteral("invalid_argument"));QCOMPARE(track.numOfClips(),1);
	}
	void revocationDuringNativeCreationReturnsAnExactPartialFailure()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());AiBridgeProjectContext ids;
		connect(&track,&Track::clipAdded,this,[](Clip*){ConfigManager::inst()->setValue("aibridge","audio_import_roots","[]");});
		auto result=AiBridgeAudioActions{}.importFile({ids.idForTrack(&track),source,0,false});
		QCOMPARE(result.code,QStringLiteral("partial_failure"));QCOMPARE(result.failureCode,QStringLiteral("plan_stale"));
		QCOMPARE(result.applied,QStringList{"create_clip"});QCOMPARE(result.failed,QStringList{"adopt_source"});QCOMPARE(result.unapplied,(QStringList{"position_clip"}));
		QCOMPARE(track.numOfClips(),1);QVERIFY(ids.resolveClip(result.clipId));QCOMPARE(static_cast<SampleClip*>(track.getClip(0))->sample().sampleSize(),size_t(0));
	}
	void nativePostVerificationNeverReplaysOrCompensates_data()
	{
		QTest::addColumn<QString>("property");QTest::newRow("muted")<<QStringLiteral("muted");QTest::newRow("amplification")<<QStringLiteral("amplification");QTest::newRow("track_volume")<<QStringLiteral("track_volume");
	}
	void nativePostVerificationNeverReplaysOrCompensates()
	{
		QFETCH(QString,property);using namespace lmms;InterferingSampleTrack track(Engine::getSong());track.property=property;AiBridgeProjectContext ids;
		auto result=AiBridgeAudioActions{}.importFile({ids.idForTrack(&track),source,300,false});
		QCOMPARE(result.code,QStringLiteral("post_verification_failed"));QCOMPARE(track.creations,1);QCOMPARE(track.numOfClips(),1);QVERIFY(ids.resolveClip(result.clipId));
		QCOMPARE(result.applied,(QStringList{"create_clip","adopt_source","position_clip"}));QCOMPARE(result.failed,QStringList{"verify_import"});QVERIFY(result.unapplied.empty());
	}
	void malformedFilesStaleTargetsAndTempoAutomationRejectBeforeWrites()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());InstrumentTrack other(Engine::getSong());AiBridgeProjectContext ids;AiBridgeAudioActions actions;
		QCOMPARE(actions.importFile({"stale",source,0,false}).code,QStringLiteral("stale_id"));
		QCOMPARE(actions.importFile({ids.idForTrack(&other),source,0,false}).code,QStringLiteral("unsupported_audio_operation"));
		{QFile f(source);QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::Truncate));f.write("not audio");}
		QCOMPARE(actions.importFile({ids.idForTrack(&track),source,0,false}).code,QStringLiteral("audio_decode_failed"));QCOMPARE(track.numOfClips(),0);
		{QFile f(source);QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::Truncate));f.write(bytes);}
		AutomationTrack automation(Engine::getSong());AutomationClip tempo(&automation);QVERIFY(tempo.addObject(&Engine::getSong()->tempoModel()));tempo.putValue(TimePos(0),120);tempo.putValue(TimePos(480),140);
		QCOMPARE(actions.importFile({ids.idForTrack(&track),source,0,false}).code,QStringLiteral("unsupported_tempo_map"));QCOMPARE(track.numOfClips(),0);
	}
};
QTEST_MAIN(AiBridgeAudioActionsTest)
#include "AiBridgeAudioActionsTest.moc"
