#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTimer>
#include <QtEndian>
#include <array>
#include <condition_variable>
#include <cmath>
#include <chrono>
#include <future>
#include <cstring>
#include <mutex>
#include "AiBridgeVocalJobs.h"
#include "AiBridgeVocalContext.h"
#include "AiBridgeVocalWire.h"
#include "AiBridgeVocalTrackActions.h"
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
#include "RecordingLibrary.h"
#include "ProjectJournal.h"
#include "RecordingLibrary.h"
#include "QDomDocument"
using namespace lmms;
class AiBridgeVocalJobSafetyTest:public QObject
{
	Q_OBJECT
	QStringList old;
	const QStringList keys{"audio_import_roots","audio_import_max_file_bytes","audio_import_max_decoded_bytes"};
	QString oldManagedRoot;
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
		oldManagedRoot=ConfigManager::inst()->value("vocal_assets","managed_root","jobs-absent");
		ConfigManager::inst()->setValue("vocal_assets","managed_root",QString{});
		dir=std::make_unique<QTemporaryDir>(QCoreApplication::applicationDirPath()+"/vocal-jobs-XXXXXX");QVERIFY(dir->isValid());
		std::array<char,244> bytes{};std::memcpy(bytes.data(),"RIFF",4);std::memcpy(bytes.data()+8,"WAVEfmt ",8);std::memcpy(bytes.data()+36,"data",4);
		qToLittleEndian<quint32>(236,bytes.data()+4);qToLittleEndian<quint32>(16,bytes.data()+16);
		qToLittleEndian<quint16>(1,bytes.data()+20);qToLittleEndian<quint16>(1,bytes.data()+22);
		qToLittleEndian<quint32>(1000,bytes.data()+24);qToLittleEndian<quint32>(2000,bytes.data()+28);
		qToLittleEndian<quint16>(2,bytes.data()+32);qToLittleEndian<quint16>(16,bytes.data()+34);qToLittleEndian<quint32>(200,bytes.data()+40);
		const auto path=dir->filePath("original.wav");{QFile f(path);QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::NewOnly));QCOMPARE(f.write(bytes.data(),qint64(bytes.size())),qint64(bytes.size()));}
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
		if(oldManagedRoot=="jobs-absent")ConfigManager::inst()->deleteValue("vocal_assets","managed_root");
		else ConfigManager::inst()->setValue("vocal_assets","managed_root",oldManagedRoot);
		oldManagedRoot.clear();
		old.clear();AudioImportPolicy::configured().refresh();dir.reset();
	}
	void variantLookupDoesNotFallThroughToCapabilities()
	{
		AiBridgeVocalJobs jobs;
		const auto response=AiBridgeVocalWire::execute({2,"variant-test","get_vocal_variant",
			{{"variant_id","00000000-0000-4000-8000-000000000001"}}},&jobs);
		QCOMPARE(response.code,QStringLiteral("project_identity_unverified"));
		QVERIFY(response.data.isUndefined());
	}
	void planRouteChecksCurrentSourceAndWorld()
	{
		AiBridgeVocalJobs jobs;AiBridgeProjectContext ids;const auto world=captureVocalEditWorld();
		const QJsonObject request{{"recipe_version",1},{"expected_project_generation",QString::number(ids.projectGeneration())},
			{"expected_world_hash",vocalEditWorldHash(world)},
			{"selections",QJsonArray{QJsonObject{{"clip_id",req.selections[0].clipId},
				{"expected_audio_state_hash",req.selections[0].expectedAudioHash},
				{"source_basis","source_pcm"},{"source_frames",QJsonObject{{"in_frame","0"},{"out_frame","100"}}}}}},
			{"input_kind","direct_source"},{"input_selection_index",0},
			{"operations",QJsonArray{QJsonObject{{"operation","gain_envelope"},{"points",QJsonArray{
				QJsonObject{{"render_input_frame","0"},{"gain_db",0.0}},
				QJsonObject{{"render_input_frame","100"},{"gain_db",0.0}}}}}}}};
		const auto before=vocalClipStateHash(req.selections[0].clipId);
		const auto response=AiBridgeVocalWire::execute({2,"plan-test","plan_vocal_edit",request},&jobs);
		QCOMPARE(response.code,QStringLiteral("unsupported_sample_rate"));
		QCOMPARE(vocalClipStateHash(req.selections[0].clipId),before);
	}
	void renderRouteRejectsUnknownImmutablePlan()
	{
		AiBridgeVocalJobs jobs;
		const auto response=AiBridgeVocalWire::execute({2,"render-test","render_vocal_variant",
			{{"plan_id","00000000-0000-4000-8000-000000000001"},{"plan_hash",QString(64,'a')},{"dry_run",true}}},&jobs);
		QCOMPARE(response.code,QStringLiteral("stale_id"));
	}
	void applyRouteRejectsUnidentifiedProject()
	{
		AiBridgeVocalJobs jobs;AiBridgeProjectContext ids;const auto world=captureVocalEditWorld();
		const auto trackSnapshot=captureVocalTrack(ids.idForTrack(this->track.get()));
		QVERIFY(trackSnapshot.value);
		const auto response=AiBridgeVocalWire::execute({2,"apply-test","apply_vocal_variant",
			{{"variant_id","00000000-0000-4000-8000-000000000001"},{"clip_id",req.selections[0].clipId},
			{"mode","replace"},{"expected_project_generation",QString::number(ids.projectGeneration())},
			{"expected_world_hash",vocalEditWorldHash(world)},{"expected_audio_state_hash",req.selections[0].expectedAudioHash},
			{"expected_track_state_hash",trackSnapshot.value->hash},{"dry_run",false}}},&jobs);
		QCOMPARE(response.code,QStringLiteral("project_identity_unverified"));
		QVERIFY(response.data.isUndefined());
	}
	void compareReplaceAndRevertKeepTheAuthorizedOriginalAndTrackSettings()
	{
		const int rate=int(Engine::audioEngine()->outputSampleRate());
		QVERIFY(rate==44100 || rate==48000);
		const quint64 frames=quint64(rate/10);
		const auto originalPath=dir->filePath(QStringLiteral("vocal-apply.wav"));
		QByteArray wave(int(44+frames*2),char(0));
		std::memcpy(wave.data(),"RIFF",4);qToLittleEndian<quint32>(quint32(wave.size()-8),reinterpret_cast<uchar*>(wave.data()+4));
		std::memcpy(wave.data()+8,"WAVEfmt ",8);qToLittleEndian<quint32>(16,reinterpret_cast<uchar*>(wave.data()+16));
		qToLittleEndian<quint16>(1,reinterpret_cast<uchar*>(wave.data()+20));
		qToLittleEndian<quint16>(1,reinterpret_cast<uchar*>(wave.data()+22));
		qToLittleEndian<quint32>(quint32(rate),reinterpret_cast<uchar*>(wave.data()+24));
		qToLittleEndian<quint32>(quint32(rate*2),reinterpret_cast<uchar*>(wave.data()+28));
		qToLittleEndian<quint16>(2,reinterpret_cast<uchar*>(wave.data()+32));
		qToLittleEndian<quint16>(16,reinterpret_cast<uchar*>(wave.data()+34));
		std::memcpy(wave.data()+36,"data",4);qToLittleEndian<quint32>(quint32(frames*2),reinterpret_cast<uchar*>(wave.data()+40));
		for(quint64 i=0;i<frames;++i)qToLittleEndian<qint16>(8192,reinterpret_cast<uchar*>(wave.data()+44+i*2));
		{QFile file(originalPath);QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::NewOnly));QCOMPARE(file.write(wave),qint64(wave.size()));}
		const auto originalFileHash=QCryptographicHash::hash(wave,QCryptographicHash::Sha256);
		const auto recordingRootPath=dir->filePath(QStringLiteral("identity-root"));
		QTemporaryDir managedRootDirectory;QVERIFY(managedRootDirectory.isValid());
		const auto managedRootPath=managedRootDirectory.path();
		QVERIFY(QDir().mkpath(recordingRootPath));
		auto recordingRoot=recording::RecordingLibrary::authorizeLocalRoot(recordingRootPath);
		QVERIFY(recordingRoot.library);
		const auto identity=Engine::getSong()->recordingProjectIdentity().getOrCreateForCurrentProject(*recordingRoot.library);
		QCOMPARE(identity.code,QStringLiteral("ok"));QVERIFY(!identity.identity.id.isEmpty());
		recordingRoot.library.reset();
		ConfigManager::inst()->setValue(QStringLiteral("vocal_assets"),QStringLiteral("managed_root"),managedRootPath);
		ConfigManager::inst()->setValue("aibridge",keys[1],"100000");
		ConfigManager::inst()->setValue("aibridge",keys[2],"100000");
		AudioImportPolicy::configured().refresh();
		auto imported=AudioImportPolicy::configured().prepare(originalPath);
		QCOMPARE(imported.code,QStringLiteral("ok"));QVERIFY(imported.audio);
		const auto sourceInfo=imported.audio->info();
		QCOMPARE(sourceInfo.frames,frames);QCOMPARE(sourceInfo.sampleRate,rate);QCOMPARE(sourceInfo.channels,1);
		auto sourceTrack=std::make_unique<SampleTrack>(Engine::getSong());
		auto sourceClip=std::make_unique<SampleClip>(sourceTrack.get());
		const auto adopted=sourceClip->adoptAuthorizedAudio(std::move(imported.audio));
		QCOMPARE(adopted,QStringLiteral("ok"));
		sourceClip->setAutoResize(false);sourceClip->movePosition(TimePos(0));
		auto destinationTrack=std::make_unique<SampleTrack>(Engine::getSong());
		AiBridgeProjectContext ids;const auto clipId=ids.idForClip(sourceClip.get());
		const auto sourceTrackId=ids.idForTrack(sourceTrack.get()),destinationTrackId=ids.idForTrack(destinationTrack.get());
		const auto originalClip=AiBridgeAudioContext{}.clip(clipId);QVERIFY(originalClip.value);
		const auto originalTrack=captureVocalTrack(sourceTrackId);QVERIFY(originalTrack.value);
		const auto destinationBefore=captureVocalTrack(destinationTrackId);QVERIFY(destinationBefore.value);
		const auto world=captureVocalEditWorld();const auto worldHash=vocalEditWorldHash(world);
		AiBridgeVocalJobs jobs;
		const QJsonObject planRequest{
			{"recipe_version",1},{"expected_project_generation",QString::number(ids.projectGeneration())},{"expected_world_hash",worldHash},
			{"selections",QJsonArray{QJsonObject{{"clip_id",clipId},{"expected_audio_state_hash",originalClip.value->stateHash},
				{"source_basis","source_pcm"},{"source_frames",QJsonObject{{"in_frame","0"},{"out_frame",QString::number(frames)}}}}}},
			{"input_kind","direct_source"},{"input_selection_index",0},
			{"operations",QJsonArray{QJsonObject{{"operation","gain_envelope"},{"points",QJsonArray{
				QJsonObject{{"render_input_frame","0"},{"gain_db",0.0}},
				QJsonObject{{"render_input_frame",QString::number(frames)},{"gain_db",0.0}}}}}}}};
		const auto plan=AiBridgeVocalWire::execute({2,"variant-plan","plan_vocal_edit",planRequest},&jobs);
		QCOMPARE(plan.code,QStringLiteral("ok"));
		const auto planData=plan.data.toObject();
		const auto planId=planData.value(QStringLiteral("plan_id")).toString();
		const auto planHash=planData.value(QStringLiteral("plan_hash")).toString();
		QVERIFY(!planId.isEmpty());QVERIFY(!planHash.isEmpty());
		QSet<QString> planIds{planId};
		for(size_t index=1;index<VocalLimits::retainedPlans;++index)
		{
			const auto extra=AiBridgeVocalWire::execute({2,QStringLiteral("variant-plan-%1").arg(qulonglong(index)),
				QStringLiteral("plan_vocal_edit"),planRequest},&jobs);
			QCOMPARE(extra.code,QStringLiteral("ok"));
			planIds.insert(extra.data.toObject().value(QStringLiteral("plan_id")).toString());
		}
		QCOMPARE(planIds.size(),int(VocalLimits::retainedPlans));
		QCOMPARE(jobs.chargedSessionBytes(),VocalLimits::planRecordCharge*VocalLimits::retainedPlans);
		const auto ninth=AiBridgeVocalWire::execute({2,"variant-plan-cap","plan_vocal_edit",planRequest},&jobs);
		QCOMPARE(ninth.code,QStringLiteral("resource_limit"));
		const auto managedFilesBefore=QDir(managedRootPath).entryList(QDir::AllEntries|QDir::NoDotAndDotDot);
		const auto dry=AiBridgeVocalWire::execute({2,"variant-dry-run","render_vocal_variant",
			{{"plan_id",planId},{"plan_hash",planHash},{"dry_run",true}}},&jobs);
		QCOMPARE(dry.code,QStringLiteral("ok"));QVERIFY(dry.data.toObject().value(QStringLiteral("dry_run")).toBool());
		QVERIFY(!dry.data.toObject().contains(QStringLiteral("job_id")));
		QVERIFY(jobs.snapshots().empty());
		QCOMPARE(QDir(managedRootPath).entryList(QDir::AllEntries|QDir::NoDotAndDotDot),managedFilesBefore);
		const auto started=AiBridgeVocalWire::execute({2,"variant-render","render_vocal_variant",
			{{"plan_id",planId},{"plan_hash",planHash},{"dry_run",false}}},&jobs);
		QCOMPARE(started.code,QStringLiteral("ok"));
		const auto jobId=started.data.toObject().value(QStringLiteral("job_id")).toString();QVERIFY(!jobId.isEmpty());
		QTRY_VERIFY_WITH_TIMEOUT(completed(jobs,jobId),5000);
		const auto status=AiBridgeVocalWire::execute({2,"variant-job","get_vocal_job",{{"job_id",jobId},{"mode","status"}}},&jobs);
		QCOMPARE(status.code,QStringLiteral("ok"));
		QCOMPARE(status.data.toObject().value(QStringLiteral("kind")).toString(),QStringLiteral("render"));
		const auto variantId=status.data.toObject().value(QStringLiteral("variant_id")).toString();QVERIFY(!variantId.isEmpty());
		const auto inspected=AiBridgeVocalWire::execute({2,"variant-inspect","get_vocal_variant",
			{{"variant_id",variantId}}},&jobs);
		QCOMPARE(inspected.code,QStringLiteral("ok"));
		QVERIFY(!QJsonDocument(inspected.data.toObject()).toJson().contains("private"));
		auto applyParameters=[&](const QString& mode,bool compare){
			const auto clip=AiBridgeAudioContext{}.clip(clipId);
			const auto track=captureVocalTrack(sourceTrackId);
			if(!clip.value || !track.value)return QJsonObject{};
			QJsonObject parameters{{"variant_id",variantId},{"clip_id",clipId},{"mode",mode},
				{"expected_project_generation",QString::number(ids.projectGeneration())},{"expected_world_hash",worldHash},
				{"expected_audio_state_hash",clip.value->stateHash},{"expected_track_state_hash",track.value->hash},{"dry_run",false}};
			if(compare)
			{
				const auto destination=captureVocalTrack(destinationTrackId);
				if(!destination.value)return QJsonObject{};
				parameters.insert("destination_track_id",destinationTrackId);
				parameters.insert("expected_destination_track_state_hash",destination.value->hash);
			}
			return parameters;
		};
		auto sendApply=[&](const QJsonObject& parameters){
			AiBridgeProtocolResponse response;
			for(int attempt=0;attempt<250;++attempt)
			{
				response=AiBridgeVocalWire::execute({2,"variant-apply", "apply_vocal_variant",parameters},&jobs);
				if(response.code!=QStringLiteral("asset_loading"))break;
				QTest::qWait(20);
			}
			return response;
		};
		const auto compareParameters=applyParameters(QStringLiteral("compare"),true);QVERIFY(!compareParameters.isEmpty());
		const auto compared=sendApply(compareParameters);QCOMPARE(compared.code,QStringLiteral("ok"));
		const auto comparisonClipId=compared.data.toObject().value(QStringLiteral("result_clip_id")).toString();
		QVERIFY(!comparisonClipId.isEmpty());
		QCOMPARE(AiBridgeAudioContext{}.clip(clipId).value->stateHash,originalClip.value->stateHash);
		const auto comparisonTrackAfter=captureVocalTrack(destinationTrackId);QVERIFY(comparisonTrackAfter.value);
		QCOMPARE(comparisonTrackAfter.value->volume,destinationBefore.value->volume);
		QCOMPARE(comparisonTrackAfter.value->panning,destinationBefore.value->panning);
		QCOMPARE(comparisonTrackAfter.value->muted,destinationBefore.value->muted);
		const auto* copied=dynamic_cast<SampleClip*>(ids.resolveClip(comparisonClipId));QVERIFY(copied);
		QCOMPARE(copied->managedVocalVariantAssociation()->active.assetId,variantId);
		const auto replaceParameters=applyParameters(QStringLiteral("replace"),false);QVERIFY(!replaceParameters.isEmpty());
		struct JournalStateGuard
		{
			ProjectJournal& journal;bool wasJournalling;
			~JournalStateGuard(){journal.clearJournal();journal.setJournalling(wasJournalling);}
		};
		JournalStateGuard journalGuard{*Engine::projectJournal(),Engine::projectJournal()->isJournalling()};
		Engine::projectJournal()->clearJournal();Engine::projectJournal()->setJournalling(true);
		const auto replaced=sendApply(replaceParameters);QCOMPARE(replaced.code,QStringLiteral("ok"));
		QVERIFY(Engine::projectJournal()->canUndo());
		Engine::projectJournal()->undo();
		QVERIFY(!sourceClip->hasManagedVocalVariant());
		QCOMPARE(sourceClip->length().getTicks(),originalClip.value->state.length);
		QCOMPARE(sourceClip->sampleFile(),originalPath);
		QVERIFY(Engine::projectJournal()->canRedo());
		Engine::projectJournal()->redo();
		QVERIFY(sourceClip->hasManagedVocalVariant());
		QCOMPARE(sourceClip->managedVocalVariantAssociation()->active.assetId,variantId);
		QTRY_COMPARE_WITH_TIMEOUT(sourceClip->managedVocalVariantStatus(),QStringLiteral("ok"),5000);
		QVERIFY(sourceClip->managedVocalVariantAssociation());
		QCOMPARE(sourceClip->managedVocalVariantAssociation()->active.assetId,variantId);
		const auto revertParameters=applyParameters(QStringLiteral("revert"),false);QVERIFY(!revertParameters.isEmpty());
		const auto reverted=sendApply(revertParameters);QCOMPARE(reverted.code,QStringLiteral("ok"));
		QVERIFY(!sourceClip->hasManagedVocalVariant());
		QCOMPARE(AiBridgeAudioContext{}.clip(clipId).value->stateHash,originalClip.value->stateHash);
		QCOMPARE(sourceClip->sampleFile(),originalPath);
		QCOMPARE(sourceClip->expectedSourceDigest(),sourceInfo.sourceSha256);
		QFile originalAfter(originalPath);QVERIFY(originalAfter.open(QIODevice::ReadOnly));
		QCOMPARE(QCryptographicHash::hash(originalAfter.readAll(),QCryptographicHash::Sha256),originalFileHash);
		QCOMPARE(jobs.release(jobId).code,QStringLiteral("ok"));

		VocalEditRequest cancelRequest;cancelRequest.expectedProjectGeneration=ids.projectGeneration();
		cancelRequest.expectedWorldHash=worldHash;cancelRequest.selections.push_back({clipId,originalClip.value->stateHash,
			VocalEditSourceBasis::SourcePcm,{0,qint64(frames)}});cancelRequest.input=VocalDirectInput{0};
		cancelRequest.operations.push_back(VocalGainEnvelope{{{0,0.0},{qint64(frames),0.0}}});
		VocalEditLimits limits;limits.maxSelections=8;limits.maxTimingAnchors=256;limits.maxGainFadePoints=1024;
		limits.maxCompSegments=8;limits.maxCrossfadeFrames=rate/4;limits.minimumGainDb=-48;limits.maximumGainDb=24;
		limits.minimumLocalStretchRatio=0.8;limits.maximumLocalStretchRatio=1.25;limits.maximumAnchorErrorFrames=1.0;
		const auto cancelPlan=AiBridgeVocalAssetActions::plan(cancelRequest,limits);QVERIFY(cancelPlan.value);
		const auto cancelInput=AiBridgeVocalAssetActions::prepareRender(*cancelPlan.value);QVERIFY(cancelInput.value);
		struct RenderGate {std::mutex mutex;std::condition_variable condition;bool release=false;std::atomic_bool entered=false;};
		RenderGate gate;
		VocalRenderRunner blockedRenderer=[&](const VocalEditRenderInput&,const std::atomic_bool& cancelled,const VocalPhaseCallback& phase){
			gate.entered=true;if(phase)phase(VocalJobPhase::Rendering);
			std::unique_lock lock(gate.mutex);
			while(!cancelled.load()||!gate.release)gate.condition.wait_for(lock,std::chrono::milliseconds(5));
			return VocalVariantRenderResult{QStringLiteral("cancelled"),{},{},0};
		};
		AiBridgeVocalJobs cancelJobs(nullptr,runVocalAnalysis,{},blockedRenderer);
		const auto cancelStart=cancelJobs.startRender(*cancelInput.value);QCOMPARE(cancelStart.code,QStringLiteral("ok"));
		QTRY_VERIFY(gate.entered.load());
		const auto cancelReply=cancelJobs.cancel(cancelStart.jobId);QVERIFY(cancelReply.cancelApplied);
		QCOMPARE(cancelJobs.start(req).code,QStringLiteral("job_busy"));
		QVERIFY(cancelJobs.chargedWorkerBytes()>0);QVERIFY(cancelJobs.chargedStagedBytes()>0);
		{std::lock_guard lock(gate.mutex);gate.release=true;}gate.condition.notify_all();
		QTRY_VERIFY(cancelJobs.job(cancelStart.jobId).value->state==VocalJobState::Cancelled);
		QCOMPARE(cancelJobs.chargedWorkerBytes(),quint64(0));QCOMPARE(cancelJobs.chargedStagedBytes(),quint64(0));
		QCOMPARE(cancelJobs.release(cancelStart.jobId).code,QStringLiteral("ok"));

		std::atomic_bool admissionCancellationObserved=false;
		VocalRenderRunner admissionRenderer=[&](const VocalEditRenderInput&,const std::atomic_bool& cancelled,const VocalPhaseCallback&){
			for(int wait=0;wait<1000 && !cancelled.load();++wait)QThread::msleep(1);
			admissionCancellationObserved=cancelled.load();
			return VocalVariantRenderResult{QStringLiteral("cancelled"),{},{},0};
		};
		auto admissionOwner=std::make_unique<AiBridgeVocalJobs>(nullptr,runVocalAnalysis,VocalThreadStarter{},admissionRenderer);
		connect(admissionOwner.get(),&AiBridgeVocalJobs::jobsChanged,this,[&]{admissionOwner.reset();});
		const auto invalidatedAdmission=admissionOwner->startRender(*cancelInput.value);
		QVERIFY(!admissionOwner);QCOMPARE(invalidatedAdmission.code,QStringLiteral("job_invalidated"));
		QVERIFY(invalidatedAdmission.jobId.isEmpty());QVERIFY(admissionCancellationObserved.load());

		struct CompletionGate {std::mutex mutex;std::condition_variable condition;bool release=false;std::atomic_bool entered=false;};
		CompletionGate completionGate;
		VocalRenderRunner completionRenderer=[&](const VocalEditRenderInput&,const std::atomic_bool&,const VocalPhaseCallback& phase){
			completionGate.entered=true;if(phase)phase(VocalJobPhase::Rendering);
			std::unique_lock lock(completionGate.mutex);
			completionGate.condition.wait(lock,[&]{return completionGate.release;});
			return VocalVariantRenderResult{QStringLiteral("cancelled"),{},{},0};
		};
		auto completionOwner=std::make_unique<AiBridgeVocalJobs>(nullptr,runVocalAnalysis,VocalThreadStarter{},completionRenderer);
		const auto completionStart=completionOwner->startRender(*cancelInput.value);QCOMPARE(completionStart.code,QStringLiteral("ok"));
		QTRY_VERIFY(completionGate.entered.load());
		bool completionDestroyedOwner=false;
		connect(completionOwner.get(),&AiBridgeVocalJobs::jobsChanged,this,[&]{
			if(!completionOwner)return;
			const auto state=completionOwner->job(completionStart.jobId);
			if(state.value && state.value->state==VocalJobState::Cancelled)
			{completionOwner.reset();completionDestroyedOwner=true;}
		});
		{std::lock_guard lock(completionGate.mutex);completionGate.release=true;}
		completionGate.condition.notify_all();
		QTRY_VERIFY(completionDestroyedOwner);QVERIFY(!completionOwner);
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
