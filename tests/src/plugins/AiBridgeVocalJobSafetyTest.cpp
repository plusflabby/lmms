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
#include "AiBridgeVocalContext.h"
#include "AiBridgeVocalWire.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "AutomatableModel.h"
#include "PatternStore.h"
#include "AiBridgeAudioContext.h"
#include "AudioImportPolicy.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "ProjectJournal.h"
#include "QDomDocument"
using namespace lmms;
class AiBridgeVocalJobSafetyTest:public QObject
{
	Q_OBJECT
	QStringList old;
	const QStringList keys{"audio_import_roots","audio_import_max_file_bytes","audio_import_max_decoded_bytes"};
	std::unique_ptr<QTemporaryDir> dir;
	std::unique_ptr<SampleTrack> track;
	SampleClip* clip=nullptr;
	VocalAnalyzeRequest req;
	std::unique_ptr<AiBridgeVocalJobs> earlyService;
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
	QJsonObject wireRequest() const
	{
		const QJsonObject frames{{"in_frame","0"},{"out_frame","100"}};
		const QJsonObject selection{{"clip_id",req.selections[0].clipId},{"expected_audio_state_hash",req.selections[0].expectedAudioHash},{"source_frames",frames}};
		return {{"kind","levels"},{"selections",QJsonArray{selection}}};
	}
private slots:
	void initTestCase(){Engine::init(true);earlyService=std::make_unique<AiBridgeVocalJobs>();}
	void cleanupTestCase(){earlyService.reset();Engine::destroy();}
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
	void shutdownDuringAdmissionCannotReportSuccess()
	{
		AiBridgeVocalJobs jobs;
		connect(&jobs,&AiBridgeVocalJobs::jobsChanged,this,[&]{jobs.shutdown();});
		const auto started=jobs.start(req);
		QCOMPARE(started.code,QString("job_invalidated"));
		QVERIFY(started.jobId.isEmpty());QCOMPARE(jobs.chargedSessionBytes(),quint64(0));
	}
	void destructionDuringAdmissionCannotReportSuccess()
	{
		auto jobs=std::make_unique<AiBridgeVocalJobs>();
		connect(jobs.get(),&AiBridgeVocalJobs::jobsChanged,this,[&]{jobs.reset();});
		const auto started=jobs->start(req);
		QVERIFY(!jobs);QCOMPARE(started.code,QString("job_invalidated"));QVERIFY(started.jobId.isEmpty());
	}
	void policyNotificationShutdownStopsStatusAndAdmission()
	{
		AiBridgeVocalJobs jobs;const auto first=jobs.start(req);QCOMPARE(first.code,QString("ok"));
		QTRY_VERIFY(completed(jobs,first.jobId));
		connect(&jobs,&AiBridgeVocalJobs::jobsChanged,this,[&]{jobs.shutdown();});
		ConfigManager::inst()->setValue("aibridge",keys[1],"19999");
		QCOMPARE(jobs.job(first.jobId).code,QString("job_invalidated"));
		QCOMPARE(jobs.start(req).code,QString("job_invalidated"));
	}
	void reentrantCancelIsIdempotentAndDoesNotReemit()
	{
		Gate gate;AiBridgeVocalJobs jobs(nullptr,[&](const auto& s,const auto& cancel,const auto& phase){return gate.run(s,cancel,phase);});
		const auto unblock=qScopeGuard([&]{gate.release();});
		const auto started=jobs.start(req);QCOMPARE(started.code,QString("ok"));QTRY_VERIFY(gate.entered.load());
		int notifications=0;bool nestedApplied=false;
		connect(&jobs,&AiBridgeVocalJobs::jobsChanged,this,[&]{
			++notifications;if(notifications<3)nestedApplied=jobs.cancel(started.jobId).cancelApplied;
		});
		QVERIFY(jobs.cancel(started.jobId).cancelApplied);QCOMPARE(notifications,1);QVERIFY(!nestedApplied);
		gate.release();
	}
	void threadStartupFailureReleasesAdmissionAndCharge()
	{
		AiBridgeVocalJobs jobs(nullptr,runVocalAnalysis,[](QThread&){return false;});
		for(int i=0;i<2;++i){QCOMPARE(jobs.start(req).code,QString("job_start_failed"));QCOMPARE(jobs.chargedSessionBytes(),quint64(0));QVERIFY(jobs.snapshots().empty());}
	}
	void repeatedInvalidationDoesNotReemitRecursively()
	{
		AiBridgeVocalJobs jobs;const auto started=jobs.start(req);QCOMPARE(started.code,QString("ok"));QTRY_VERIFY(completed(jobs,started.jobId));
		int notifications=0;
		connect(&jobs,&AiBridgeVocalJobs::jobsChanged,this,[&]{if(++notifications<3)jobs.invalidateProject();});
		jobs.invalidateProject();QCOMPARE(notifications,1);
	}
	void startupExceptionAfterLaunchJoinsBeforeUncharging()
	{
		Gate gate;std::atomic_bool exited=false;
		AiBridgeVocalJobs jobs(nullptr,[&](const auto& s,const auto& cancel,const auto& phase){auto result=gate.run(s,cancel,phase);exited=true;return result;},
			[&](QThread& thread){thread.start();while(!gate.entered.load())QThread::yieldCurrentThread();gate.release();throw std::runtime_error("private-startup-path");return false;});
		QCOMPARE(jobs.start(req).code,QString("job_start_failed"));QVERIFY(exited.load());
		QCOMPARE(jobs.chargedSessionBytes(),quint64(0));QVERIFY(jobs.snapshots().empty());
	}
	void projectTrackCeilingRejectsBeforeDiscovery()
	{
		std::vector<std::unique_ptr<SampleTrack>> extra;
		for(int i=0;i<4096;++i)extra.push_back(std::make_unique<SampleTrack>(Engine::patternStore()));
		QCOMPARE(validateVocalProjectBounds(),QString("resource_limit"));
		QCOMPARE(captureVocalInputs(req).code,QString("resource_limit"));
	}
	void projectClipCeilingKeepsTerminalStatusAndReleaseAvailable()
	{
		AiBridgeVocalJobs jobs;const auto started=jobs.start(req);QCOMPARE(started.code,QString("ok"));QTRY_VERIFY(completed(jobs,started.jobId));
		const auto analysisId=jobs.job(started.jobId).value->analysisId;
		std::vector<std::unique_ptr<SampleClip>> extra;
		for(int i=0;i<4096;++i)extra.push_back(std::make_unique<SampleClip>(track.get()));
		QCOMPARE(validateVocalProjectBounds(),QString("resource_limit"));
		QCOMPARE(jobs.start(req).code,QString("resource_limit"));
		QCOMPARE(jobs.analysis(analysisId,0,1).code,QString("resource_limit"));
		QCOMPARE(jobs.job(started.jobId).code,QString("ok"));QCOMPARE(jobs.cancel(started.jobId).code,QString("ok"));
		QCOMPARE(jobs.release(started.jobId).code,QString("ok"));QCOMPARE(jobs.chargedSessionBytes(),quint64(0));
	}
	void projectBindingCeilingIncludesGlobalAutomation()
	{
		std::vector<std::unique_ptr<FloatModel>> models;
		auto automation=std::make_unique<AutomationClip>(Engine::getSong()->globalAutomationTrack());
		for(int i=0;i<65537;++i){models.push_back(std::make_unique<FloatModel>());QVERIFY(automation->addObject(models.back().get(),false));}
		QCOMPARE(validateVocalProjectBounds(),QString("resource_limit"));
		QCOMPARE(captureVocalInputs(req).code,QString("resource_limit"));
	}
	void policyTeardownFromEachReadCannotReturnStaleData()
	{
		for(int operation=0;operation<4;++operation)
		{
			auto jobs=std::make_unique<AiBridgeVocalJobs>();const auto started=jobs->start(req);QCOMPARE(started.code,QString("ok"));QTRY_VERIFY(completed(*jobs,started.jobId));
			const auto analysisId=jobs->job(started.jobId).value->analysisId;
			connect(jobs.get(),&AiBridgeVocalJobs::jobsChanged,this,[&]{jobs.reset();});
			ConfigManager::inst()->setValue("aibridge",keys[1],QString::number(19000+operation));
			if(operation==0)QCOMPARE(jobs->analysis(analysisId,0,1).code,QString("job_invalidated"));
			if(operation==1)QVERIFY(jobs->snapshots().empty());
			if(operation==2)QCOMPARE(jobs->release(started.jobId).code,QString("job_invalidated"));
			if(operation==3)QCOMPARE(jobs->cancel(started.jobId).code,QString("job_invalidated"));
			QVERIFY(!jobs);
		}
	}
	void completionNotificationCanDestroyOwner()
	{
		auto jobs=std::make_unique<AiBridgeVocalJobs>();const auto started=jobs->start(req);QCOMPARE(started.code,QString("ok"));
		connect(jobs.get(),&AiBridgeVocalJobs::jobsChanged,this,[&]{if(jobs && completed(*jobs,started.jobId))jobs.reset();});
		QTRY_VERIFY(!jobs);
	}
	void analysisLifecycleDoesNotMutateNativeProjectOrSource()
	{
		QDomDocument before;auto root=before.createElement("sampletrack");before.appendChild(root);track->saveSettings(before,root);
		QFile source(dir->filePath("original.wav"));QVERIFY(source.open(QIODevice::ReadOnly));const auto bytes=source.readAll();source.close();
		const bool modified=Engine::getSong()->isModified();int notifications=0;
		connect(track->volumeModel(),&Model::dataChanged,this,[&]{++notifications;});connect(track->panningModel(),&Model::dataChanged,this,[&]{++notifications;});
		AiBridgeVocalJobs jobs;const auto started=jobs.start(req);QCOMPARE(started.code,QString("ok"));QTRY_VERIFY(completed(jobs,started.jobId));
		const auto state=jobs.job(started.jobId);const auto first=jobs.analysis(state.value->analysisId,0,1);QCOMPARE(first.code,QString("ok"));
		const auto second=jobs.analysis(state.value->analysisId,1,1);QCOMPARE(second.code,QString("ok"));QCOMPARE(second.page.offset,1);
		QCOMPARE(jobs.analysis(state.value->analysisId,999,1).records.size(),size_t(0));
		QVERIFY(!jobs.cancel(started.jobId).cancelApplied);QCOMPARE(jobs.release(started.jobId).code,QString("ok"));
		QDomDocument after;root=after.createElement("sampletrack");after.appendChild(root);track->saveSettings(after,root);
		QCOMPARE(after.toByteArray(),before.toByteArray());QCOMPARE(Engine::getSong()->isModified(),modified);QCOMPARE(notifications,0);
		QVERIFY(source.open(QIODevice::ReadOnly));QCOMPARE(source.readAll(),bytes);
	}
	void strictWireAdmissionAndPrivateFieldRejection()
	{
		auto parameters=wireRequest();
		QVERIFY(AiBridgeVocalWire::handles("analyze_vocal_audio"));QVERIFY(!AiBridgeVocalWire::handles("set_audio_track_properties"));
		QVERIFY(AiBridgeVocalWire::validRequest("analyze_vocal_audio",parameters));
		for(const auto& frame:QJsonArray{0,"00","+0","-1","9223372036854775808"})
		{
			auto bad=parameters;auto selections=bad["selections"].toArray();auto selection=selections[0].toObject();selection["source_frames"]=QJsonObject{{"in_frame",frame},{"out_frame","100"}};selections[0]=selection;bad["selections"]=selections;
			QVERIFY(!AiBridgeVocalWire::validRequest("analyze_vocal_audio",bad));
		}
		parameters["path"]="G:/private/recording.wav";QVERIFY(!AiBridgeVocalWire::validRequest("analyze_vocal_audio",parameters));
		QVERIFY(!AiBridgeVocalWire::validRequest("get_vocal_capabilities",{{"dry_run",true}}));
		QVERIFY(!AiBridgeVocalWire::validRequest("get_vocal_analysis",{{"analysis_id","id"},{"limit",257}}));
		QVERIFY(!AiBridgeVocalWire::validRequest("get_vocal_job",{{"job_id","id"},{"mode","delete"}}));
	}
	void wireLifecyclePrivacyAndCompleteEnvelopeLimit()
	{
		QDomDocument before;auto root=before.createElement("sampletrack");before.appendChild(root);track->saveSettings(before,root);
		const bool modified=Engine::getSong()->isModified();
		const bool canUndo=Engine::projectJournal()->canUndo(),canRedo=Engine::projectJournal()->canRedo();
		QSignalSpy volumeSignals(track->volumeModel(),&Model::dataChanged),panSignals(track->panningModel(),&Model::dataChanged);
		AiBridgeVocalJobs jobs;
		const auto call=[&](const QString& action,const QJsonObject& parameters){return AiBridgeVocalWire::execute({2,"wire-test",action,parameters},&jobs);};
		auto caps=call("get_vocal_capabilities",{});QCOMPARE(caps.code,QString("ok"));QVERIFY(!caps.data.toObject()["eq_supported"].toBool());QVERIFY(!caps.data.toObject()["track_property_writes_supported"].toBool());
		auto started=call("analyze_vocal_audio",wireRequest());
		QCOMPARE(started.code,QString("ok"));const auto id=started.data.toObject()["job_id"].toString();QVERIFY(!id.isEmpty());QTRY_VERIFY(completed(jobs,id));
		auto status=call("get_vocal_job",{{"job_id",id},{"mode","status"}});QCOMPARE(status.code,QString("ok"));
		auto result=call("get_vocal_analysis",{{"analysis_id",status.data.toObject()["analysis_id"]},{"limit",1}});QCOMPARE(result.code,QString("ok"));
		const auto frame=AiBridgeProtocol::serializeResponse(result);QVERIFY(frame.size()<=65536);
		QVERIFY(!frame.contains(dir->path().toUtf8()));QVERIFY(!frame.contains("original.wav"));QVERIFY(!frame.contains("source_sha256"));QVERIFY(!frame.contains("identity"));QVERIFY(!frame.contains("pcm"));
		QCOMPARE(call("cancel_vocal_job",{{"job_id",id}}).data.toObject()["cancel_applied"].toBool(),false);
		QCOMPARE(call("get_vocal_job",{{"job_id",id},{"mode","release"}}).code,QString("ok"));
		QCOMPARE(call("get_vocal_analysis",{{"analysis_id",status.data.toObject()["analysis_id"]}}).code,QString("stale_id"));
		QDomDocument after;root=after.createElement("sampletrack");after.appendChild(root);track->saveSettings(after,root);
		QCOMPARE(after.toByteArray(),before.toByteArray());QCOMPARE(Engine::getSong()->isModified(),modified);
		QCOMPARE(Engine::projectJournal()->canUndo(),canUndo);QCOMPARE(Engine::projectJournal()->canRedo(),canRedo);
		QCOMPARE(volumeSignals.count(),0);QCOMPARE(panSignals.count(),0);
		track->setName(QString(65536,'x'));QCOMPARE(call("get_vocal_capabilities",{}).code,QString("resource_limit"));
		track->setName(QString(256,'x'));std::vector<std::unique_ptr<SampleTrack>> extra;
		for(int i=0;i<127;++i){extra.push_back(std::make_unique<SampleTrack>(Engine::getSong()));extra.back()->setName(QString(256,'x'));}
		QCOMPARE(call("get_vocal_capabilities",{}).code,QString("result_too_large"));
	}
	void wireCapabilitiesCannotSucceedAfterServiceShutdown()
	{
		AiBridgeVocalJobs jobs;jobs.shutdown();
		QCOMPARE(AiBridgeVocalWire::execute({2,"stopped","get_vocal_capabilities",{}},&jobs).code,QString("job_invalidated"));
	}
	void backendFailureCodeCannotLeakPrivateText()
	{
		AiBridgeVocalJobs jobs(nullptr,[](const auto&,const auto&,const auto&){return VocalAnalysisResult{"G:/private/source.wav",{},{},0};});
		const auto started=jobs.start(req);QCOMPARE(started.code,QString("ok"));
		QTRY_VERIFY(jobs.job(started.jobId).value->state==VocalJobState::Failed);
		QCOMPARE(jobs.job(started.jobId).value->failureCode,QString("analysis_failed"));
	}
	void admissionObserverMayProcessActualCompletion()
	{
		AiBridgeVocalJobs jobs;bool processed=false;
		connect(&jobs,&AiBridgeVocalJobs::jobsChanged,this,[&]{
			if(processed)return;
			processed=true;
			QElapsedTimer timer;timer.start();
			while(timer.elapsed()<2000){const auto snapshots=jobs.snapshots();if(!snapshots.empty() && snapshots[0].state==VocalJobState::Succeeded)break;QTest::qWait(1);}
		});
		const auto started=jobs.start(req);QCOMPARE(started.code,QString("ok"));QVERIFY(completed(jobs,started.jobId));
	}
	void invalidationNotificationCannotReenterAdmission()
	{
		AiBridgeVocalJobs jobs;const auto started=jobs.start(req);QCOMPARE(started.code,QString("ok"));QTRY_VERIFY(completed(jobs,started.jobId));
		QString nested;bool entered=false;
		connect(&jobs,&AiBridgeVocalJobs::jobsChanged,this,[&]{if(entered)return;entered=true;nested=jobs.start(req).code;});
		jobs.invalidateProject();QCOMPARE(nested,QString("job_invalidated"));
	}
	void lifecycleHookRunsBeforeEarlyServiceNotification()
	{
		const auto started=earlyService->start(req);QCOMPARE(started.code,QString("ok"));QTRY_VERIFY(completed(*earlyService,started.jobId));
		AiBridgeProjectContext ids;const auto before=ids.projectGeneration();quint64 observed=0;
		const auto observer=connect(earlyService.get(),&AiBridgeVocalJobs::jobsChanged,this,[&]{observed=ids.projectGeneration();});
		emit Engine::getSong()->projectAboutToChange();disconnect(observer);QVERIFY(observed>before);
		QCOMPARE(earlyService->release(started.jobId).code,QString("ok"));
	}
	void cancellationBeforePublicationRetainsSlotUntilWorkerExit()
	{
		Gate gate;std::atomic_bool ready=false;
		AiBridgeVocalJobs jobs(nullptr,[&](const auto& s,const auto& cancelled,const auto& phase){
			auto result=runVocalAnalysis(s,cancelled,phase);ready=true;std::unique_lock lock(gate.mutex);gate.cv.wait(lock,[&]{return gate.open;});return result;
		});const auto unblock=qScopeGuard([&]{gate.release();});
		const auto started=jobs.start(req);QCOMPARE(started.code,QString("ok"));QTRY_VERIFY(ready.load());
		const auto charge=jobs.chargedSessionBytes();QVERIFY(charge>0);QVERIFY(jobs.cancel(started.jobId).cancelApplied);
		QCOMPARE(jobs.chargedSessionBytes(),charge);QCOMPARE(jobs.start(req).code,QString("job_busy"));
		gate.release();QTRY_VERIFY(jobs.job(started.jobId).value->state==VocalJobState::Cancelled);
		QVERIFY(jobs.job(started.jobId).value->analysisId.isEmpty());QCOMPARE(jobs.release(started.jobId).code,QString("ok"));QCOMPARE(jobs.chargedSessionBytes(),quint64(0));
	}
	void shutdownReturnsOnlyAfterCooperativeWorkerExit()
	{
		std::atomic_bool entered=false,exited=false;
		AiBridgeVocalJobs jobs(nullptr,[&](const auto&,const auto& cancelled,const auto&){entered=true;while(!cancelled.load())QThread::yieldCurrentThread();exited=true;return VocalAnalysisResult{"cancelled",{},{},0};});
		QCOMPARE(jobs.start(req).code,QString("ok"));QTRY_VERIFY(entered.load());jobs.shutdown();QVERIFY(exited.load());QCOMPARE(jobs.chargedSessionBytes(),quint64(0));
	}
	void wireRejectsOldProjectAndPolicyResults()
	{
		for(bool project:{false,true})
		{
			AiBridgeVocalJobs jobs;const auto started=jobs.start(req);QCOMPARE(started.code,QString("ok"));QTRY_VERIFY(completed(jobs,started.jobId));
			const auto id=jobs.job(started.jobId).value->analysisId;
			if(project)emit Engine::getSong()->projectAboutToChange();else ConfigManager::inst()->setValue("aibridge",keys[1],"19001");
			const auto status=AiBridgeVocalWire::execute({2,"stale","get_vocal_job",{{"job_id",started.jobId},{"mode","status"}}},&jobs);
			QCOMPARE(status.data.toObject()["state"].toString(),QString("invalidated"));
			QCOMPARE(AiBridgeVocalWire::execute({2,"stale","get_vocal_analysis",{{"analysis_id",id}}},&jobs).code,QString("stale_id"));
			QCOMPARE(jobs.release(started.jobId).code,QString("ok"));
		}
	}
	void wireWrongThreadRejectsBeforeServiceOrNativeAccess()
	{
		auto result=std::async(std::launch::async,[]{return AiBridgeVocalWire::execute({2,"thread","get_vocal_capabilities",{}},nullptr);});
		QCOMPARE(result.get().code,QString("wrong_thread"));
	}
	void capabilitiesRejectOversizedPolicyBeforeJsonDiscovery()
	{
		AiBridgeVocalJobs jobs;ConfigManager::inst()->setValue("aibridge",keys[0],"["+QString(65536,' ')+"]");
		QCOMPARE(AiBridgeVocalWire::execute({2,"policy","get_vocal_capabilities",{}},&jobs).code,QString("resource_limit"));
	}
};
QTEST_MAIN(AiBridgeVocalJobSafetyTest)
#include "AiBridgeVocalJobSafetyTest.moc"
