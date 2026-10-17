#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QtEndian>
#include <condition_variable>
#include <future>
#include <cstring>
#include "AiBridgeVocalJobs.h"
#include "AiBridgeAudioContext.h"
#include "AudioImportPolicy.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
using namespace lmms;
class AiBridgeVocalJobsTest:public QObject
{
	Q_OBJECT
	QStringList old;
	const QStringList keys{"audio_import_roots","audio_import_max_file_bytes","audio_import_max_decoded_bytes"};
	std::unique_ptr<QTemporaryDir> dir;
	std::unique_ptr<SampleTrack> track;
	SampleClip* clip=nullptr;
	VocalAnalyzeRequest req;
	struct Gate
	{
		std::mutex mutex;std::condition_variable cv;std::atomic_bool entered=false;bool open=false;
		void release(){std::lock_guard lock(mutex);open=true;cv.notify_all();}
		VocalAnalysisResult run(const VocalInputSnapshot& s,const std::atomic_bool& cancelled,const VocalPhaseCallback& phase)
		{
			entered=true;std::unique_lock lock(mutex);cv.wait(lock,[&]{return open || cancelled.load();});lock.unlock();
			return runVocalAnalysis(s,cancelled,phase);
		}
	};
	bool completed(AiBridgeVocalJobs& jobs,const QString& id)
	{const auto r=jobs.job(id);return r.value && r.value->state==VocalJobState::Succeeded;}
private slots:
	void initTestCase(){Engine::init(true);}
	void cleanupTestCase(){Engine::destroy();}
	void init()
	{
		for(const auto& key:keys)old<<ConfigManager::inst()->value("aibridge",key,"jobs-absent");
		dir=std::make_unique<QTemporaryDir>(QCoreApplication::applicationDirPath()+"/vocal-jobs-XXXXXX");QVERIFY(dir->isValid());
		QByteArray bytes(244,0);std::memcpy(bytes.data(),"RIFF",4);std::memcpy(bytes.data()+8,"WAVEfmt ",8);std::memcpy(bytes.data()+36,"data",4);
		qToLittleEndian<quint32>(236,bytes.data()+4);qToLittleEndian<quint32>(16,bytes.data()+16);
		qToLittleEndian<quint16>(1,bytes.data()+20);qToLittleEndian<quint16>(1,bytes.data()+22);
		qToLittleEndian<quint32>(1000,bytes.data()+24);qToLittleEndian<quint32>(2000,bytes.data()+28);
		qToLittleEndian<quint16>(2,bytes.data()+32);qToLittleEndian<quint16>(16,bytes.data()+34);qToLittleEndian<quint32>(200,bytes.data()+40);
		const auto path=dir->filePath("original.wav");{QFile f(path);QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::NewOnly));QCOMPARE(f.write(bytes),qint64(bytes.size()));}
		ConfigManager::inst()->setValue("aibridge",keys[0],QString::fromUtf8(QJsonDocument(QJsonArray{dir->path()}).toJson(QJsonDocument::Compact)));
		ConfigManager::inst()->setValue("aibridge",keys[1],"20000");ConfigManager::inst()->setValue("aibridge",keys[2],"20000");
		track=std::make_unique<SampleTrack>(Engine::getSong());clip=new SampleClip(track.get());
		auto p=AudioImportPolicy::configured().prepare(path);QCOMPARE(p.code,QString("ok"));QCOMPARE(clip->adoptAuthorizedAudio(std::move(p.audio)),QString("ok"));
		AiBridgeProjectContext ids;const auto id=ids.idForClip(clip);const auto before=AiBridgeAudioContext{}.clip(id);QVERIFY(before.value);
		req={{{id,before.value->stateHash,{0,100}}},{}};
	}
	void cleanup()
	{
		track.reset();clip=nullptr;
		for(int i=0;i<keys.size();++i){if(old[i]=="jobs-absent")ConfigManager::inst()->deleteValue("aibridge",keys[i]);else ConfigManager::inst()->setValue("aibridge",keys[i],old[i]);}
		old.clear();AudioImportPolicy::configured().refresh();dir.reset();
	}
	void oneWorkerEightRecordsNoImplicitEviction()
	{
		Gate gate;AiBridgeVocalJobs jobs(nullptr,[&](const auto& s,const auto& cancel,const auto& phase){return gate.run(s,cancel,phase);});
		const auto unblock=qScopeGuard([&]{gate.release();});
		auto first=jobs.start(req);QCOMPARE(first.code,QString("ok"));
		QCOMPARE(jobs.start(req).code,QString("job_busy"));QVERIFY(jobs.chargedSessionBytes()>0);
		gate.release();QTRY_VERIFY(completed(jobs,first.jobId));
		for(int i=1;i<8;++i){const auto next=jobs.start(req);QCOMPARE(next.code,QString("ok"));QTRY_VERIFY(completed(jobs,next.jobId));}
		QCOMPARE(jobs.start(req).code,QString("resource_limit"));
		QCOMPARE(jobs.release(first.jobId).code,QString("ok"));
		const auto next=jobs.start(req);QCOMPARE(next.code,QString("ok"));QTRY_VERIFY(completed(jobs,next.jobId));
	}
	void cancelDuringReadKeepsActiveSlotAndQtResponsive()
	{
		Gate gate;AiBridgeVocalJobs jobs(nullptr,[&](const auto& s,const auto& cancel,const auto& phase){return gate.run(s,cancel,phase);});
		const auto unblock=qScopeGuard([&]{gate.release();});
		const auto start=jobs.start(req);QCOMPARE(start.code,QString("ok"));QTRY_VERIFY(gate.entered.load());
		bool heartbeat=false;QTimer::singleShot(0,[&]{heartbeat=true;});QTRY_VERIFY(heartbeat);
		QCOMPARE(jobs.release(start.jobId).code,QString("job_busy"));
		QVERIFY(jobs.cancel(start.jobId).cancelApplied);QCOMPARE(jobs.start(req).code,QString("job_busy"));
		gate.release();QTRY_VERIFY(jobs.job(start.jobId).value && jobs.job(start.jobId).value->state==VocalJobState::Cancelled);
		QCOMPARE(jobs.release(start.jobId).code,QString("ok"));QCOMPARE(jobs.chargedSessionBytes(),quint64(0));
	}
	void lateCancelAndReleaseRetiresAnalysis()
	{
		AiBridgeVocalJobs jobs;const auto start=jobs.start(req);QCOMPARE(start.code,QString("ok"));QTRY_VERIFY(completed(jobs,start.jobId));
		const auto cancel=jobs.cancel(start.jobId);QVERIFY(!cancel.cancelApplied);QCOMPARE(cancel.value->state,VocalJobState::Succeeded);
		const auto analysis=cancel.value->analysisId;const auto page=jobs.analysis(analysis,0,256);QCOMPARE(page.code,QString("ok"));
		QCOMPARE(page.applicability,QString("current"));QCOMPARE(page.records.size(),size_t(3));
		clip->setMuted(true);QCOMPARE(jobs.analysis(analysis,0,256).applicability,QString("stale"));
		QCOMPARE(jobs.release(start.jobId).code,QString("ok"));QCOMPARE(jobs.analysis(analysis,0,256).code,QString("stale_id"));
		QCOMPARE(jobs.chargedSessionBytes(),quint64(0));
	}
	void policyAndProjectChangesNeverPublishSuccess()
	{
		for(bool project:{false,true})
		{
			Gate gate;AiBridgeVocalJobs jobs(nullptr,[&](const auto& s,const auto& cancel,const auto& phase){return gate.run(s,cancel,phase);});
			const auto unblock=qScopeGuard([&]{gate.release();});
			const auto start=jobs.start(req);QCOMPARE(start.code,QString("ok"));QTRY_VERIFY(gate.entered.load());
			if(project)emit Engine::getSong()->projectAboutToChange();else ConfigManager::inst()->setValue("aibridge",keys[1],"19999");
			QCOMPARE(jobs.job(start.jobId).value->state,VocalJobState::Invalidated);
			QCOMPARE(jobs.start(req).code,QString("job_busy"));
			const auto charged=jobs.chargedSessionBytes();QVERIFY(charged>0);QCOMPARE(jobs.release(start.jobId).code,QString("ok"));
			QCOMPARE(jobs.chargedSessionBytes(),charged); // invalidated task still owns private charges until actual exit.
			gate.release();QTRY_COMPARE(jobs.chargedSessionBytes(),quint64(0));
			AiBridgeProjectContext ids;const auto id=ids.idForClip(clip);const auto current=AiBridgeAudioContext{}.clip(id);QVERIFY(current.value);
			req.selections[0].clipId=id;req.selections[0].expectedAudioHash=current.value->stateHash;
		}
	}
	void shutdownWaitsAndWorkerExceptionIsSanitized()
	{
		AiBridgeVocalJobs jobs(nullptr,[](const auto&,const auto&,const auto&)->VocalAnalysisResult{throw std::runtime_error("G:/private/secret.wav");});
		const auto start=jobs.start(req);QCOMPARE(start.code,QString("ok"));
		QTRY_VERIFY(jobs.job(start.jobId).value && jobs.job(start.jobId).value->state==VocalJobState::Failed);
		QCOMPARE(jobs.job(start.jobId).value->failureCode,QString("analysis_failed"));
		AiBridgeVocalJobs stopping;const auto next=stopping.start(req);QCOMPARE(next.code,QString("ok"));
		stopping.shutdown();QCOMPARE(stopping.start(req).code,QString("job_invalidated"));
	}
	void sourceChangeFailsAndClipChangePreservesStaleProvenance()
	{
		for(bool changeSource:{false,true})
		{
			Gate gate;AiBridgeVocalJobs jobs(nullptr,[&](const auto& s,const auto& cancel,const auto& phase){return gate.run(s,cancel,phase);});
			const auto unblock=qScopeGuard([&]{gate.release();});
			const auto start=jobs.start(req);QCOMPARE(start.code,QString("ok"));QTRY_VERIFY(gate.entered.load());
			if(changeSource){QFile f(dir->filePath("original.wav"));QVERIFY(f.open(QIODevice::ReadWrite));QVERIFY(f.seek(44));QCOMPARE(f.write("xx"),qint64(2));}
			else clip->setMuted(true);
			gate.release();QTRY_VERIFY(jobs.job(start.jobId).value &&
				(jobs.job(start.jobId).value->state==VocalJobState::Succeeded || jobs.job(start.jobId).value->state==VocalJobState::Failed));
			const auto state=jobs.job(start.jobId).value;
			if(changeSource){QCOMPARE(state->state,VocalJobState::Failed);QCOMPARE(state->failureCode,QString("audio_source_changed"));}
			else{QCOMPARE(state->state,VocalJobState::Succeeded);QCOMPARE(jobs.analysis(state->analysisId,0,256).applicability,QString("stale"));}
			clip->setMuted(false);
		}
	}
};
QTEST_MAIN(AiBridgeVocalJobsTest)
#include "AiBridgeVocalJobsTest.moc"
