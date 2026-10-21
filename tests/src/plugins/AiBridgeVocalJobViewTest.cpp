#include <QtTest>
#include <QComboBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QFile>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtEndian>
#include <cstring>
#include "AiBridgeVocalJobView.h"
#include "AiBridgeVocalWire.h"
#include "AiBridgeAudioContext.h"
#include "AudioImportPolicy.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
using namespace lmms;
class AiBridgeVocalJobViewTest:public QObject
{
	Q_OBJECT
	QStringList old;
	const QStringList keys{"audio_import_roots","audio_import_max_file_bytes","audio_import_max_decoded_bytes"};
	std::unique_ptr<QTemporaryDir> dir;
	std::unique_ptr<SampleTrack> track;
	SampleClip* clip=nullptr;
	VocalAnalyzeRequest req;
	bool completed(AiBridgeVocalJobs& jobs,const QString& id){const auto r=jobs.job(id);return r.value && r.value->state==VocalJobState::Succeeded;}
private slots:
	void initTestCase(){Engine::init(true);}
	void cleanupTestCase(){if(!old.isEmpty())cleanup();Engine::destroy();}
	void init()
	{
		for(const auto& key:keys)old<<ConfigManager::inst()->value("aibridge",key,"view-absent");
		dir=std::make_unique<QTemporaryDir>(QCoreApplication::applicationDirPath()+"/vocal-view-XXXXXX");QVERIFY(dir->isValid());
		QByteArray bytes(4044,0);std::memcpy(bytes.data(),"RIFF",4);std::memcpy(bytes.data()+8,"WAVEfmt ",8);std::memcpy(bytes.data()+36,"data",4);
		qToLittleEndian<quint32>(4036,bytes.data()+4);qToLittleEndian<quint32>(16,bytes.data()+16);qToLittleEndian<quint16>(1,bytes.data()+20);qToLittleEndian<quint16>(1,bytes.data()+22);
		qToLittleEndian<quint32>(1000,bytes.data()+24);qToLittleEndian<quint32>(2000,bytes.data()+28);qToLittleEndian<quint16>(2,bytes.data()+32);qToLittleEndian<quint16>(16,bytes.data()+34);qToLittleEndian<quint32>(4000,bytes.data()+40);
		const auto path=dir->filePath("original.wav");{QFile f(path);QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::NewOnly));QCOMPARE(f.write(bytes),qint64(bytes.size()));}
		ConfigManager::inst()->setValue("aibridge",keys[0],QString::fromUtf8(QJsonDocument(QJsonArray{dir->path()}).toJson(QJsonDocument::Compact)));
		// 2000*sizeof(SampleFrame)+4172 scratch =20172 bytes; keep fixture admission above that bound.
		ConfigManager::inst()->setValue("aibridge",keys[1],"20000");ConfigManager::inst()->setValue("aibridge",keys[2],"32768");
		track=std::make_unique<SampleTrack>(Engine::getSong());clip=new SampleClip(track.get());auto p=AudioImportPolicy::configured().prepare(path);QCOMPARE(p.code,QString("ok"));QCOMPARE(clip->adoptAuthorizedAudio(std::move(p.audio)),QString("ok"));
		AiBridgeProjectContext ids;const auto id=ids.idForClip(clip);const auto before=AiBridgeAudioContext{}.clip(id);QVERIFY(before.value);req={{{id,before.value->stateHash,{0,2000}}},{}};
	}
	void cleanup()
	{
		track.reset();clip=nullptr;for(int i=0;i<keys.size();++i){if(old[i]=="view-absent")ConfigManager::inst()->deleteValue("aibridge",keys[i]);else ConfigManager::inst()->setValue("aibridge",keys[i],old[i]);}old.clear();AudioImportPolicy::configured().refresh();dir.reset();
	}
	void readonlyInspectorSharesJobsAcrossCloseAndReopen()
	{
		AiBridgeVocalJobs jobs;auto view=std::make_unique<AiBridgeVocalJobView>(&jobs);
		auto* selector=view->findChild<QComboBox*>("vocalJobSelector");QVERIFY(selector);
		auto* details=view->findChild<QPlainTextEdit*>("vocalDetails");QVERIFY(details);QVERIFY(details->isReadOnly());
		const auto started=jobs.start(req);QCOMPARE(started.code,QString("ok"));QTRY_VERIFY(completed(jobs,started.jobId));
		QTRY_COMPARE(selector->count(),1);QTRY_VERIFY(details->toPlainText().contains("Source before mixer"));
		QVERIFY(!details->toPlainText().contains(dir->path()));QVERIFY(!details->toPlainText().contains("original.wav"));
		view.reset();QVERIFY(completed(jobs,started.jobId));view=std::make_unique<AiBridgeVocalJobView>(&jobs);
		selector=view->findChild<QComboBox*>("vocalJobSelector");QVERIFY(selector);QTRY_COMPARE(selector->currentData().toString(),started.jobId);
		QCOMPARE(jobs.snapshots().size(),size_t(1));
	}
	void cancelAndReleaseUseOnlySharedSessionState()
	{
		AiBridgeVocalJobs jobs(nullptr,[](const auto&,const auto& cancelled,const auto&){while(!cancelled.load())QThread::yieldCurrentThread();return VocalAnalysisResult{"cancelled",{},{},0};});
		AiBridgeVocalJobView view(&jobs);const auto started=jobs.start(req);QCOMPARE(started.code,QString("ok"));
		auto* cancel=view.findChild<QPushButton*>("vocalCancel");auto* release=view.findChild<QPushButton*>("vocalRelease");QVERIFY(cancel && release);
		QTRY_VERIFY(cancel->isEnabled());cancel->click();QTRY_VERIFY(jobs.job(started.jobId).value->state==VocalJobState::Cancelled);
		QTRY_VERIFY(release->isEnabled());release->click();QTRY_VERIFY(jobs.snapshots().empty());QCOMPARE(jobs.chargedSessionBytes(),quint64(0));
	}
	void destructionDuringPolicyRefreshDoesNotResumeOnDeadView()
	{
		AiBridgeVocalJobs jobs;const auto started=jobs.start(req);QCOMPARE(started.code,QString("ok"));QTRY_VERIFY(completed(jobs,started.jobId));
		auto view=std::make_unique<AiBridgeVocalJobView>(&jobs);connect(&jobs,&AiBridgeVocalJobs::jobsChanged,this,[&]{view.reset();});
		ConfigManager::inst()->setValue("aibridge",keys[1],"19999");view->refreshNow();QVERIFY(!view);
	}
	void serviceDestructionLeavesAReadonlyUnavailableView()
	{
		auto jobs=std::make_unique<AiBridgeVocalJobs>();AiBridgeVocalJobView view(jobs.get());jobs.reset();
		auto* status=view.findChild<QLabel*>("vocalStatus");QVERIFY(status);QTRY_VERIFY(status->text().contains("unavailable"));
		auto* cancel=view.findChild<QPushButton*>("vocalCancel");auto* release=view.findChild<QPushButton*>("vocalRelease");QVERIFY(cancel && release);QVERIFY(!cancel->isEnabled());QVERIFY(!release->isEnabled());
	}
	void twoInspectorsAndWireUseOneRegistryAndInvalidateTogether()
	{
		AiBridgeVocalJobs jobs;AiBridgeVocalJobView first(&jobs),second(&jobs);
		const QJsonObject selection{{"clip_id",req.selections[0].clipId},{"expected_audio_state_hash",req.selections[0].expectedAudioHash},{"source_frames",QJsonObject{{"in_frame","0"},{"out_frame","100"}}}};
		const auto started=AiBridgeVocalWire::execute({2,"view","analyze_vocal_audio",{{"selections",QJsonArray{selection}}}},&jobs);QCOMPARE(started.code,QString("ok"));
		const auto id=started.data.toObject()["job_id"].toString();QTRY_VERIFY(completed(jobs,id));
		for(auto* view:{&first,&second}){auto* selector=view->findChild<QComboBox*>("vocalJobSelector");QVERIFY(selector);QTRY_COMPARE(selector->count(),1);}
		emit Engine::getSong()->projectAboutToChange();
		for(auto* view:{&first,&second}){auto* status=view->findChild<QLabel*>("vocalStatus");QVERIFY(status);QTRY_VERIFY(status->text().contains("invalidated"));}
	}
	void staleAndFailedResultsRemainExplicitAndPrivate()
	{
		AiBridgeVocalJobs jobs;AiBridgeVocalJobView view(&jobs);const auto started=jobs.start(req);QCOMPARE(started.code,QString("ok"));QTRY_VERIFY(completed(jobs,started.jobId));
		auto* details=view.findChild<QPlainTextEdit*>("vocalDetails");QVERIFY(details);QTRY_VERIFY(details->toPlainText().contains("Applicability: current"));
		clip->setMuted(true);view.refreshNow();QVERIFY(details->toPlainText().contains("Applicability: stale"));
		AiBridgeVocalJobs failed(nullptr,[](const auto&,const auto&,const auto&)->VocalAnalysisResult{throw std::runtime_error("G:/private/exception.wav");});
		AiBridgeProjectContext ids;req.selections[0].expectedAudioHash=AiBridgeAudioContext{}.clip(ids.idForClip(clip)).value->stateHash;
		AiBridgeVocalJobView failureView(&failed);const auto error=failed.start(req);QCOMPARE(error.code,QString("ok"));
		auto* status=failureView.findChild<QLabel*>("vocalStatus");QVERIFY(status);QTRY_VERIFY(status->text().contains("failed"));
		auto* failureDetails=failureView.findChild<QPlainTextEdit*>("vocalDetails");QVERIFY(failureDetails);QVERIFY(failureDetails->toPlainText().contains("analysis_failed"));QVERIFY(!failureDetails->toPlainText().contains("private"));
	}
	void boundedRecordPagingUsesExistingImmutableAnalysis()
	{
		AiBridgeVocalJobs jobs;AiBridgeVocalJobView view(&jobs);view.show();QTRY_VERIFY(view.isVisible());
		const auto started=jobs.start(req);QCOMPARE(started.code,QString("ok"));QTRY_VERIFY(completed(jobs,started.jobId));
		auto* next=view.findChild<QPushButton*>("vocalNext");auto* previous=view.findChild<QPushButton*>("vocalPrevious");auto* details=view.findChild<QPlainTextEdit*>("vocalDetails");QVERIFY(next && previous && details);
		QTRY_VERIFY(next->isEnabled());QVERIFY(!previous->isEnabled());next->click();QTRY_VERIFY(previous->isEnabled());QVERIFY(!next->isEnabled());
		QVERIFY(details->toPlainText().contains("33–41 of 41"));previous->click();QTRY_VERIFY(next->isEnabled());QVERIFY(!previous->isEnabled());QVERIFY(details->toPlainText().contains("1–32 of 41"));
		QCOMPARE(jobs.snapshots().size(),size_t(1));QVERIFY(completed(jobs,started.jobId));view.close();QVERIFY(completed(jobs,started.jobId));
	}
};
QTEST_MAIN(AiBridgeVocalJobViewTest)
#include "AiBridgeVocalJobViewTest.moc"
