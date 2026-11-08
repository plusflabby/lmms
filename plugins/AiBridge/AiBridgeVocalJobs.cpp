#include "AiBridgeVocalJobs.h"
#include "AiBridgeVocalAnalysis.h"
#include "AiBridgeVocalContext.h"
#include "AiBridgeVocalSource.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "src/core/vocal/ManagedVocalAssetRoot.h"
#include "src/core/vocal/VocalRuntimeQualification.h"
#include "RecordingProjectIdentity.h"
#include "Song.h"
#include "src/core/vocal/ManagedVocalProjectOwnerStore.h"
#include <QPointer>
#include <QThread>
#include <QUuid>
#include <chrono>
#include <mutex>
#include <variant>
namespace lmms {
namespace {
using VocalWorkerResult = std::variant<VocalAnalysisResult, VocalVariantRenderResult>;
using VocalWorkerFunction = std::function<VocalWorkerResult(
	const std::atomic_bool&, const VocalPhaseCallback&)>;

bool terminal(VocalJobState state)
{
	return state==VocalJobState::Cancelled || state==VocalJobState::Succeeded
		|| state==VocalJobState::Failed || state==VocalJobState::Invalidated;
}
QString uuid(){return QUuid::createUuid().toString(QUuid::WithoutBraces);}
QString failureCode(const QString& code)
{
	for(const auto* known:{"ok","cancelled","analysis_failed","resource_limit","invalid_parameter","wrong_thread",
		"import_root_not_configured","import_path_denied","audio_source_missing","audio_source_changed","audio_decode_failed",
		"audio_resource_limits_not_configured","source_too_large","incomplete_analysis","invalid_audio"})
		if(code==QLatin1String(known))return code;
	return "analysis_failed";
}

QString renderFailureCode(const QString& code)
{
	for (const auto* known : {"ok", "cancelled", "feature_unavailable", "vocal_runtime_unqualified", "resource_limit", "invalid_parameter",
		"wrong_thread", "plan_stale", "job_invalidated", "asset_root_not_configured", "asset_root_denied",
		"asset_root_busy", "asset_root_revoked", "asset_integrity_failed", "asset_write_failed", "asset_incomplete",
		"project_identity_unverified", "asset_project_mismatch", "asset_missing", "source_changed",
		"clip_state_mismatch", "unsupported_sample_rate", "unsupported_audio_operation","stale_mixer_channel",
		"unsupported_playback_world", "unsupported_source_basis", "timing_anchor_out_of_tolerance",
		"unsupported_stretch_ratio", "stretch_segment_too_short", "stretch_backend_unavailable",
		"stretch_backend_failed", "protected_range_not_preserved", "import_root_not_configured", "import_path_denied",
		"audio_source_missing", "audio_source_changed", "audio_decode_failed", "audio_resource_limits_not_configured",
		"source_too_large", "invalid_audio"})
	{
		if (code == QLatin1String(known)) return code;
	}
	return QStringLiteral("render_failed");
}
VocalEditLimits approvedEditLimits(int sampleRate)
{
	VocalEditLimits limits;
	limits.maxSelections=VocalLimits::selections;
	limits.maxTimingAnchors=256;
	limits.maxGainFadePoints=1024;
	limits.maxCompSegments=8;
	limits.maxCrossfadeFrames=sampleRate>0?sampleRate/4:0;
	limits.minimumGainDb=-48.0;
	limits.maximumGainDb=24.0;
	limits.minimumLocalStretchRatio=0.80;
	limits.maximumLocalStretchRatio=1.25;
	limits.maximumAnchorErrorFrames=1.0;
	return limits;
}
}
struct AiBridgeVocalJobs::Entry
{
	VocalJobSnapshot state;
	VocalJobKind kind=VocalJobKind::Analysis;
	VocalAnalyzeRequest request;
	VocalAnalysisResult result;
	VocalVariantRenderResult renderResult;
	quint64 policyRevision=0,charge=0;
	quint64 stagedCharge=0;
	QString managedRootSetting,projectId;
};
struct AiBridgeVocalJobs::PlanEntry
{
	VocalEditRequest request;
	VocalEditPlan plan;
	VocalEditPlanningSnapshot planning;
	VocalEditLimits limits;
	std::chrono::steady_clock::time_point lastAccess;
	quint64 policyRevision=0,charge=0;
};
struct AiBridgeVocalJobs::Running
{
	QString id;
	VocalJobKind kind=VocalJobKind::Analysis;
	VocalWorkerFunction worker;
	std::atomic_bool cancel=false;
	std::atomic<VocalJobPhase> phase=VocalJobPhase::Authorizing;
	std::unique_ptr<QThread> thread;
	std::mutex resultMutex;
	VocalWorkerResult result=VocalAnalysisResult{};
	quint64 sessionCharge=0,workerCharge=0,stagedCharge=0;
};
VocalAnalysisResult runVocalAnalysis(const VocalInputSnapshot& s,const std::atomic_bool& cancel,const VocalPhaseCallback& phase)
{
	const auto budget=estimateVocalBudget(s);if(budget.code!="ok")return {budget.code,{},{},0};
	VocalAnalysisResult result;result.records.reserve(size_t(budget.recordCapacity));result.summaries.reserve(s.sources.size());
	for(size_t i=0;i<s.sources.size();++i)
	{
		if(cancel.load())return {"cancelled",{},{},0};
		const auto& source=s.sources[i];VocalSelectionMetadata meta;
		meta.clipId=source.clipId;meta.sourceRevision=source.sourceRevision;meta.generation=s.generation;
		meta.frames=s.request.selections[i].frames;meta.sampleRate=source.expectedInfo.sampleRate;meta.channels=source.expectedInfo.channels;
		VocalAnalysisAccumulator accumulator(s.request.settings,meta);phase(VocalJobPhase::Authorizing);
		const auto scanned=scanVocalSource(source,[&](quint64 first,std::span<const SampleFrame> frames){
			phase(VocalJobPhase::Scanning);return accumulator.consume(first,frames);
		},cancel);
		if(scanned.code!="ok")return {scanned.code,{},{},0};
		phase(VocalJobPhase::Analyzing);auto part=accumulator.finish();if(part.code!="ok")return part;
		if(result.records.size()+part.records.size()>budget.recordCapacity)return {"resource_limit",{},{},0};
		for(auto& record:part.records){record.selectionIndex=int(i);result.records.push_back(std::move(record));}
		for(auto& summary:part.summaries)result.summaries.push_back(std::move(summary));
	}
	if(cancel.load())return {"cancelled",{},{},0};
	result.accountedBytes=quint64(result.records.capacity())*sizeof(VocalAnalysisRecord)
		+quint64(result.summaries.capacity())*sizeof(VocalSelectionSummary);
	if(result.accountedBytes>budget.sessionBytes)return {"resource_limit",{},{},0};
	phase(VocalJobPhase::Publishing);result.code="ok";return result;
}
AiBridgeVocalJobs::AiBridgeVocalJobs(QObject* p,VocalJobRunner runner,VocalThreadStarter starter,
	VocalRenderRunner renderRunner):QObject(p),m_runner(std::move(runner)),
	m_renderRunner(std::move(renderRunner)),m_starter(std::move(starter))
{
	if(!m_starter)m_starter=[](QThread& thread){thread.start();return thread.isRunning() || thread.isFinished();};
	if(!m_renderRunner)m_renderRunner=AiBridgeVocalAssetActions::render;
	// Install the existing generation hook before service invalidation observers.
	AiBridgeProjectContext ids;
	if(auto* song=Engine::getSong())connect(song,&Song::projectAboutToChange,this,&AiBridgeVocalJobs::invalidateProject,Qt::DirectConnection);
	m_policyTimer.setInterval(25);connect(&m_policyTimer,&QTimer::timeout,this,[this]{
		const QPointer<AiBridgeVocalJobs> self(this);checkLifetime();
		if(!self || m_stopping || !m_active)return;
		const auto entry=m_entries.find(m_active->id);
		const auto phase=m_active->phase.load();
		if(entry!=m_entries.end() && entry->second->state.state==VocalJobState::Running && entry->second->state.phase!=phase)
		{entry->second->state.phase=phase;emit jobsChanged();}
	});m_policyTimer.start();
}
AiBridgeVocalJobs::~AiBridgeVocalJobs(){shutdown();}
void AiBridgeVocalJobs::checkLifetime()
{
	if(m_stopping || QThread::currentThread()!=thread())return;
	AiBridgeProjectContext ids;const auto gen=ids.projectGeneration();const auto revision=ConfigManager::inst()->audioImportPolicyRevision();
	purgePlans();
	const auto managedRootSetting=ConfigManager::inst()->value(QStringLiteral("vocal_assets"),
		QStringLiteral("managed_root"));
	auto* song=Engine::getSong();
	const auto managedOwner=song ? song->managedVocalOwner() : nullptr;
	const auto projectId=managedOwner ? managedOwner->projectId
		: (song ? song->recordingProjectIdentity().cachedIdentity().id : QString{});
	bool changed=false;
	for(auto& [id,e]:m_entries)
	{
		if (e->kind==VocalJobKind::Render && e->state.state==VocalJobState::Succeeded
			&& !e->state.variantId.isEmpty()) continue;
		const bool renderContextChanged=e->kind==VocalJobKind::Render
			&& (e->managedRootSetting!=managedRootSetting || e->projectId!=projectId);
		if(e->state.state!=VocalJobState::Invalidated
			&& (e->state.generation!=gen || e->policyRevision!=revision || renderContextChanged))
		{
			e->state.state=VocalJobState::Invalidated;e->state.analysisId.clear();e->state.failureCode="job_invalidated";
			e->result={};e->renderResult={};
			if(m_active && m_active->id==id)m_active->cancel=true;
			changed=true;
		}
	}
	if(changed)
	{
		const QPointer<AiBridgeVocalJobs> self(this);const bool notifying=m_invalidationNotification;m_invalidationNotification=true;
		emit jobsChanged();if(self)m_invalidationNotification=notifying;
	}
}

void AiBridgeVocalJobs::purgePlans()
{
	if(QThread::currentThread()!=thread())return;
	const auto now=std::chrono::steady_clock::now();
	AiBridgeProjectContext ids;
	const auto generation=ids.projectGeneration();
	const auto policyRevision=ConfigManager::inst()->audioImportPolicyRevision();
	for(auto it=m_plans.begin();it!=m_plans.end();)
	{
		const bool expired=now-it->second->lastAccess>=std::chrono::milliseconds(VocalLimits::planIdleMilliseconds);
		const bool stale=it->second->planning.generation!=generation || it->second->policyRevision!=policyRevision;
		if(expired || stale)
		{
			m_charged-=std::min(m_charged,it->second->charge);
			m_planCharged-=std::min(m_planCharged,it->second->charge);
			it=m_plans.erase(it);
		}
		else ++it;
	}
}
VocalStartResult AiBridgeVocalJobs::start(const VocalAnalyzeRequest& request)
{
	if(QThread::currentThread()!=thread())return {"wrong_thread",{}, {}};
	if(!isAvailable())return {"job_invalidated",{}, {}};
	const QPointer<AiBridgeVocalJobs> self(this);
	checkLifetime();
	if(!self || m_stopping)return {"job_invalidated",{}, {}};
	if(m_active)return {"job_busy",{}, {}};
	if(m_entries.size()>=VocalLimits::retainedJobs)return {"resource_limit",{}, {}};
	const auto captured=captureVocalInputs(request);if(!captured.value)return {captured.code,{}, {}};
	const auto budget=estimateVocalBudget(*captured.value);if(budget.code!="ok")return {budget.code,{}, {}};
	if(budget.sessionBytes>VocalLimits::sessionBytes-m_charged)return {"resource_limit",{}, {}};
	try
	{
		auto task=std::make_shared<Running>();
		task->kind=VocalJobKind::Analysis;
		task->workerCharge=budget.jobBytes;
		const auto runner=m_runner;
		const auto snapshot=*captured.value;
		task->worker=[runner,snapshot](const std::atomic_bool& cancelled,const VocalPhaseCallback& phase)
			->VocalWorkerResult{return runner(snapshot,cancelled,phase);};
		auto entry=std::make_unique<Entry>();
		entry->kind=VocalJobKind::Analysis;entry->request=request;
		entry->policyRevision=captured.value->policyRevision;entry->charge=budget.sessionBytes;
		entry->state.jobId=uuid();entry->state.inputHash=captured.value->inputHash;
		entry->state.generation=captured.value->generation;entry->state.kind=VocalJobKind::Analysis;
		entry->state.state=VocalJobState::Running;
		task->id=entry->state.jobId;
		return startWorker(std::move(task),std::move(entry));
	}
	catch(const std::bad_alloc&){return {"resource_limit",{}, {}};}
}

VocalStartResult AiBridgeVocalJobs::startRender(const VocalEditRenderInput& input)
{
	if(QThread::currentThread()!=thread())return {"wrong_thread",{}, {}};
	if(!isAvailable())return {"job_invalidated",{}, {}};
	const QPointer<AiBridgeVocalJobs> self(this);
	checkLifetime();
	if(!self || m_stopping)return {"job_invalidated",{}, {}};
	if(m_active)return {"job_busy",{}, {}};
	if(m_entries.size()>=VocalLimits::retainedJobs)return {"resource_limit",{}, {}};
	const auto prepared=AiBridgeVocalAssetActions::prepareRender(input.ticket);
	if(!prepared.value)return {prepared.code,{}, {}};
	if(!input.assetRoot || !input.assetRoot->isAuthorized()
		|| prepared.value->assetRoot->rootId()!=input.assetRoot->rootId()
		|| prepared.value->projectId!=input.projectId)
	{
		return {QStringLiteral("plan_stale"),{}, {}};
	}
	const auto budget=AiBridgeVocalAssetActions::estimate(input.ticket);
	if(budget.code!=QStringLiteral("ok"))return {budget.code,{}, {}};
	if(budget.sessionBytes>VocalLimits::sessionBytes-m_charged
		|| budget.jobBytes>VocalLimits::jobBytes-m_workerCharged
		|| budget.stagedBytes>VocalLimits::stagedOutputBytes-m_stagedCharged)
	{
		return {QStringLiteral("resource_limit"),{}, {}};
	}
	try
	{
		auto task=std::make_shared<Running>();
		task->kind=VocalJobKind::Render;
		task->workerCharge=budget.jobBytes;
		task->stagedCharge=budget.stagedBytes;
		const auto runner=m_renderRunner;
		const auto renderInput=*prepared.value;
		task->worker=[runner,renderInput](const std::atomic_bool& cancelled,const VocalPhaseCallback& phase)
			->VocalWorkerResult{return runner(renderInput,cancelled,phase);};
		auto entry=std::make_unique<Entry>();
		entry->kind=VocalJobKind::Render;
		entry->policyRevision=renderInput.ticket.sources.policyRevision;
		entry->charge=budget.sessionBytes;entry->stagedCharge=budget.stagedBytes;
		entry->managedRootSetting=ConfigManager::inst()->value(QStringLiteral("vocal_assets"),
			QStringLiteral("managed_root"));
		entry->projectId=renderInput.projectId;
		entry->state.jobId=uuid();entry->state.inputHash=renderInput.ticket.plan.planHash;
		entry->state.generation=renderInput.ticket.plan.projectGeneration;
		entry->state.kind=VocalJobKind::Render;entry->state.state=VocalJobState::Running;
		task->id=entry->state.jobId;
		return startWorker(std::move(task),std::move(entry));
	}
	catch(const std::bad_alloc&){return {"resource_limit",{}, {}};}
}

VocalEditHandleResult AiBridgeVocalJobs::planEdit(const VocalEditRequest& request)
{
	if(QThread::currentThread()!=thread())return {QStringLiteral("wrong_thread"),{}};
	if(!isAvailable())return {QStringLiteral("job_invalidated"),{}};
	const QPointer<AiBridgeVocalJobs> self(this);
	checkLifetime();
	if(!self || m_stopping)return {QStringLiteral("job_invalidated"),{}};
	purgePlans();
	if(m_plans.size()>=VocalLimits::retainedPlans)return {QStringLiteral("resource_limit"),{}};
	const auto world=captureVocalEditWorld();
	const auto limits=approvedEditLimits(world.outputRate);
	const auto captured=AiBridgeVocalAssetActions::plan(request,limits);
	if(!captured.value)return {captured.code,{}};
	if(m_charged>VocalLimits::sessionBytes-VocalLimits::planRecordCharge)
		return {QStringLiteral("resource_limit"),{}};
	try
	{
		auto entry=std::make_unique<PlanEntry>();
		entry->request=request;
		entry->plan=captured.value->plan;
		entry->planning=captured.value->planning;
		entry->limits=limits;
		entry->lastAccess=std::chrono::steady_clock::now();
		entry->policyRevision=captured.value->sources.policyRevision;
		entry->charge=VocalLimits::planRecordCharge;
		const auto id=uuid();
		const auto handle=VocalEditPlanHandle{id,entry->request,entry->plan,entry->planning,entry->limits};
		m_plans.emplace(id,std::move(entry));
		m_charged+=VocalLimits::planRecordCharge;
		m_planCharged+=VocalLimits::planRecordCharge;
		return {QStringLiteral("ok"),handle};
	}
	catch(const std::bad_alloc&)
	{
		return {QStringLiteral("resource_limit"),{}};
	}
}

VocalRenderAdmissionResult AiBridgeVocalJobs::renderPlan(
	const QString& planId,const QString& planHash,bool dryRun)
{
	VocalRenderAdmissionResult result;result.planId=planId;result.planHash=planHash;result.dryRun=dryRun;
	if(QThread::currentThread()!=thread()){result.code=QStringLiteral("wrong_thread");return result;}
	if(!isAvailable()){result.code=QStringLiteral("job_invalidated");return result;}
	if(!qualifiedVocalSamplerateRuntime()){result.code=QStringLiteral("feature_unavailable");return result;}
	const QPointer<AiBridgeVocalJobs> self(this);
	checkLifetime();
	if(!self || m_stopping){result.code=QStringLiteral("job_invalidated");return result;}
	purgePlans();
	const auto found=m_plans.find(planId);
	if(found==m_plans.end()){result.code=QStringLiteral("stale_id");return result;}
	const auto request=found->second->request;
	const auto expectedPlan=found->second->plan;
	const auto planning=found->second->planning;
	const auto limits=found->second->limits;
	if(expectedPlan.planHash!=planHash){result.code=QStringLiteral("plan_stale");return result;}
	const auto currentWorld=captureVocalEditWorld();
	if(planning.generation!=AiBridgeProjectContext().projectGeneration()
		|| planning.worldHash!=vocalEditWorldHash(currentWorld))
	{result.code=QStringLiteral("plan_stale");return result;}
	found->second->lastAccess=std::chrono::steady_clock::now();
	const auto recaptured=AiBridgeVocalAssetActions::plan(request,limits);
	if(!recaptured.value)
	{
		result.code=recaptured.code==QStringLiteral("clip_state_mismatch")
			?QStringLiteral("source_changed"):recaptured.code;
		return result;
	}
	if(recaptured.value->plan.planHash!=expectedPlan.planHash)
	{result.code=QStringLiteral("plan_stale");return result;}
	result.budget=dryRun?AiBridgeVocalAssetActions::dryRun(*recaptured.value)
		:AiBridgeVocalAssetActions::estimate(*recaptured.value);
	if(result.budget.code!=QStringLiteral("ok")){result.code=result.budget.code;return result;}
	if(dryRun){result.code=QStringLiteral("ok");return result;}
	const auto prepared=AiBridgeVocalAssetActions::prepareRender(*recaptured.value);
	if(!prepared.value){result.code=prepared.code;return result;}
	const auto started=startRender(*prepared.value);
	if(!self || m_stopping){result.code=QStringLiteral("job_invalidated");return result;}
	result.code=started.code;
	if(started.code==QStringLiteral("ok"))result.jobId=started.jobId;
	return result;
}

VocalStartResult AiBridgeVocalJobs::startWorker(std::shared_ptr<Running> task,std::unique_ptr<Entry> entry)
{
	if(!task || !entry || task->id.isEmpty())return {QStringLiteral("invalid_parameter"),{}, {}};
	const auto id=task->id;
	const auto inputHash=entry->state.inputHash;
	const auto sessionCharge=entry->charge;
	task->sessionCharge=sessionCharge;
	task->stagedCharge=entry->stagedCharge;
	try
	{
		task->thread=std::make_unique<QThread>();
		auto worker=std::make_unique<QObject>();
		worker->moveToThread(task->thread.get());
		connect(task->thread.get(),&QThread::finished,worker.get(),&QObject::deleteLater);
		const QPointer<AiBridgeVocalJobs> owner(this);
		const auto weak=std::weak_ptr<Running>(task);
		connect(task->thread.get(),&QThread::started,worker.get(),[weak]{
			const auto running=weak.lock();if(!running)return;
			// The worker owns only its copied ticket/runner and bounded result mailbox.
			const auto phase=[&](VocalJobPhase value){running->phase.store(value,std::memory_order_release);};
			VocalWorkerResult result;
			try{result=running->worker(running->cancel,phase);}
			catch(...)
			{
				result=running->kind==VocalJobKind::Analysis
					?VocalWorkerResult{VocalAnalysisResult{QStringLiteral("analysis_failed"),{},{},0}}
					:VocalWorkerResult{VocalVariantRenderResult{QStringLiteral("render_failed"),{},{},0}};
			}
			if(running->kind==VocalJobKind::Analysis)
			{
				auto& analysis=std::get<VocalAnalysisResult>(result);
				analysis.code=failureCode(analysis.code);
			}
			else
			{
				auto& render=std::get<VocalVariantRenderResult>(result);
				render.code=renderFailureCode(render.code);
			}
			{std::lock_guard lock(running->resultMutex);running->result=std::move(result);}
			QThread::currentThread()->quit();
		});
		connect(task->thread.get(),&QThread::finished,this,[owner,id]{if(owner)owner->complete(id);},Qt::QueuedConnection);
		m_entries.emplace(id,std::move(entry));
		m_charged+=sessionCharge;
		m_workerCharged+=task->workerCharge;
		m_stagedCharged+=task->stagedCharge;
		m_active=task;
		bool started=false;
		try{started=m_starter(*task->thread);}catch(...){started=false;}
		const bool launched=task->thread->isRunning() || task->thread->isFinished();
		if(launched)worker.release();
		if(!started || !launched)
		{
			task->cancel.store(true,std::memory_order_release);
			task->thread->quit();task->thread->wait();
			m_entries.erase(id);
			m_charged-=std::min(m_charged,sessionCharge);
			m_workerCharged-=std::min(m_workerCharged,task->workerCharge);
			m_stagedCharged-=std::min(m_stagedCharged,task->stagedCharge);
			m_active.reset();
			return {QStringLiteral("job_start_failed"),{}, {}};
		}
		emit jobsChanged();
		if(!owner || m_stopping)return {QStringLiteral("job_invalidated"),{}, {}};
		const auto live=m_entries.find(id);
		if(live==m_entries.end() || live->second->state.state==VocalJobState::Invalidated)
			return {QStringLiteral("job_invalidated"),{}, {}};
		return {QStringLiteral("ok"),id,inputHash};
	}
	catch(const std::bad_alloc&)
	{
		if(m_entries.erase(id))m_charged-=std::min(m_charged,sessionCharge);
		if(m_active==task)m_active.reset();
		return {QStringLiteral("resource_limit"),{}, {}};
	}
}
void AiBridgeVocalJobs::complete(const QString& id)
{
	if(m_stopping || !m_active || m_active->id!=id)return;
	const QPointer<AiBridgeVocalJobs> self(this);
	const auto task=m_active;
	task->thread->wait();checkLifetime();
	if(!self || m_stopping || m_active!=task)return;
	m_workerCharged-=std::min(m_workerCharged,task->workerCharge);
	m_stagedCharged-=std::min(m_stagedCharged,task->stagedCharge);
	auto it=m_entries.find(id);
	if(it!=m_entries.end())
	{
		auto& e=*it->second;std::lock_guard lock(task->resultMutex);
		if(task->kind==VocalJobKind::Render)
		{
			auto& rendered=std::get<VocalVariantRenderResult>(task->result);
			const bool published=rendered.code==QStringLiteral("ok") && rendered.variant.has_value();
			if(published)
			{
				e.renderResult=rendered;
				e.state.variantId=rendered.variant->assetId;
			}
			if(e.state.state!=VocalJobState::Invalidated)
			{
				if(task->cancel.load() || e.state.state==VocalJobState::CancelRequested
					|| rendered.code==QStringLiteral("cancelled"))
				{e.state.state=VocalJobState::Cancelled;e.state.failureCode=QStringLiteral("cancelled");}
				else if(rendered.code!=QStringLiteral("ok"))
				{e.state.state=VocalJobState::Failed;e.state.failureCode=rendered.code;}
				else if(!rendered.variant)
				{e.state.state=VocalJobState::Failed;e.state.failureCode=QStringLiteral("asset_integrity_failed");}
				else
				{e.state.state=VocalJobState::Succeeded;e.state.phase=VocalJobPhase::Publishing;e.state.progress=1;}
			}
		}
		else if(e.state.state!=VocalJobState::Invalidated)
		{
			auto& analysis=std::get<VocalAnalysisResult>(task->result);
			if(task->cancel.load() || e.state.state==VocalJobState::CancelRequested
				|| analysis.code==QStringLiteral("cancelled"))
			{e.state.state=VocalJobState::Cancelled;e.state.failureCode=QStringLiteral("cancelled");}
			else if(analysis.code!=QStringLiteral("ok"))
			{e.state.state=VocalJobState::Failed;e.state.failureCode=analysis.code;}
			else
			{
				e.result=std::move(analysis);e.state.state=VocalJobState::Succeeded;e.state.analysisId=uuid();
				e.state.phase=VocalJobPhase::Publishing;e.state.progress=1;
			}
		}
	}
	else m_charged-=std::min(m_charged,task->sessionCharge);
	m_active.reset();emit jobsChanged();
}
VocalJobResult AiBridgeVocalJobs::job(const QString& id)
{
	if(QThread::currentThread()!=thread())return {"wrong_thread",{},false};
	const QPointer<AiBridgeVocalJobs> self(this);
	checkLifetime();if(!self || m_stopping)return {"job_invalidated",{},false};
	const auto it=m_entries.find(id);if(it==m_entries.end())return {"stale_id",{},false};
	return {"ok",it->second->state,false};
}
VocalJobResult AiBridgeVocalJobs::cancel(const QString& id)
{
	const QPointer<AiBridgeVocalJobs> self(this);
	auto state=job(id);if(!state.value || terminal(state.value->state) || state.value->state==VocalJobState::CancelRequested)return state;
	if(m_active && m_active->id==id)
	{
		m_active->cancel=true;m_entries.at(id)->state.state=VocalJobState::CancelRequested;
		state.value=m_entries.at(id)->state;state.cancelApplied=true;emit jobsChanged();
		if(!self || m_stopping)return {"job_invalidated",{},false};
		const auto live=m_entries.find(id);if(live==m_entries.end())return {"stale_id",{},false};
		state.value=live->second->state;
	}
	return state;
}
VocalJobResult AiBridgeVocalJobs::release(const QString& id)
{
	const QPointer<AiBridgeVocalJobs> self(this);
	auto state=job(id);if(!state.value)return state;
	if(!terminal(state.value->state))return {"job_busy",{},false};
	const auto it=m_entries.find(id);
	if(!m_active || m_active->id!=id)m_charged-=it->second->charge;
	m_entries.erase(it);emit jobsChanged();
	if(!self || m_stopping)return {"job_invalidated",{},false};
	return state;
}
VocalVariantRenderResult AiBridgeVocalJobs::renderedVariant(const QString& id)
{
	if(QThread::currentThread()!=thread())return {QStringLiteral("wrong_thread"),{}, {},0};
	const QPointer<AiBridgeVocalJobs> self(this);
	checkLifetime();
	if(!self || m_stopping)return {QStringLiteral("job_invalidated"),{}, {},0};
	const auto it=m_entries.find(id);
	if(it==m_entries.end())return {QStringLiteral("stale_id"),{}, {},0};
	if(it->second->kind!=VocalJobKind::Render)return {QStringLiteral("invalid_parameter"),{}, {},0};
	if(it->second->renderResult.variant)return it->second->renderResult;
	if(!terminal(it->second->state.state))return {QStringLiteral("job_busy"),{}, {},0};
	return {it->second->state.failureCode.isEmpty()?QStringLiteral("asset_integrity_failed")
		:it->second->state.failureCode,{}, {},0};
}
VocalAnalysisPage AiBridgeVocalJobs::analysis(const QString& id,int offset,int limit)
{
	VocalAnalysisPage page;
	if(QThread::currentThread()!=thread()){page.code="wrong_thread";return page;}
	if(offset<0 || limit<1 || limit>VocalLimits::page){page.code="invalid_parameter";return page;}
	const QPointer<AiBridgeVocalJobs> self(this);
	checkLifetime();
	if(!self || m_stopping){page.code="job_invalidated";return page;}
	for(const auto& [key,e]:m_entries)if(e->kind==VocalJobKind::Analysis && e->state.analysisId==id
		&& !id.isEmpty() && e->state.state==VocalJobState::Succeeded)
	{
		const auto bounded=validateVocalProjectBounds();if(bounded!="ok"){page.code=bounded;return page;}
		page.code="ok";page.analysisId=id;page.applicability="current";
		for(const auto& selection:e->request.selections)if(vocalClipStateHash(selection.clipId)!=selection.expectedAudioHash)page.applicability="stale";
		page.summaries=e->result.summaries;const auto total=int(e->result.records.size());
		const auto begin=std::min(offset,total);const auto count=std::min(limit,total-begin);
		page.page={total,offset,limit,count<total-begin};
		page.records.assign(e->result.records.begin()+begin,e->result.records.begin()+begin+count);return page;
	}
	page.code="stale_id";return page;
}
void AiBridgeVocalJobs::invalidateProject()
{
	if(m_stopping || m_invalidationNotification || QThread::currentThread()!=thread())return;
	m_charged-=std::min(m_charged,m_planCharged);m_planCharged=0;m_plans.clear();
	const QPointer<AiBridgeVocalJobs> self(this);m_invalidationNotification=true;
	bool changed=false;
	for(auto& [id,e]:m_entries)
	{
		if(e->kind==VocalJobKind::Render && e->state.state==VocalJobState::Succeeded
			&& !e->state.variantId.isEmpty())continue;
		if(e->state.state!=VocalJobState::Invalidated)
		{e->state.state=VocalJobState::Invalidated;e->state.analysisId.clear();e->result={};e->renderResult={};
			e->state.failureCode="job_invalidated";changed=true;}
	}
	if(m_active)m_active->cancel=true;
	if(changed)emit jobsChanged();
	if(self)m_invalidationNotification=false;
}
void AiBridgeVocalJobs::shutdown()
{
	if(m_stopping)return;
	m_stopping=true;m_policyTimer.stop();
	if(m_active)
	{
		m_active->cancel=true;m_active->thread->quit();m_active->thread->wait();
		m_workerCharged-=std::min(m_workerCharged,m_active->workerCharge);
		m_stagedCharged-=std::min(m_stagedCharged,m_active->stagedCharge);
		m_active.reset();
	}
	m_entries.clear();m_charged=0;m_workerCharged=0;m_stagedCharged=0;
	m_plans.clear();m_planCharged=0;
}
quint64 AiBridgeVocalJobs::chargedSessionBytes()const{return m_charged;}
quint64 AiBridgeVocalJobs::chargedWorkerBytes()const{return m_workerCharged;}
quint64 AiBridgeVocalJobs::chargedStagedBytes()const{return m_stagedCharged;}
std::vector<VocalJobSnapshot> AiBridgeVocalJobs::snapshots()
{
	if(QThread::currentThread()!=thread())return {};
	const QPointer<AiBridgeVocalJobs> self(this);
	checkLifetime();if(!self || m_stopping)return {};
	std::vector<VocalJobSnapshot> result;result.reserve(m_entries.size());
	for(const auto& [id,e]:m_entries)result.push_back(e->state);
	return result;
}
}
