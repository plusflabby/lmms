#include "AiBridgeVocalJobs.h"
#include "AiBridgeVocalAnalysis.h"
#include "AiBridgeVocalContext.h"
#include "AiBridgeVocalSource.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "Song.h"
#include <QPointer>
#include <QThread>
#include <QUuid>
#include <mutex>
namespace lmms {
namespace {
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
}
struct AiBridgeVocalJobs::Entry
{
	VocalJobSnapshot state;
	VocalAnalyzeRequest request;
	VocalAnalysisResult result;
	quint64 policyRevision=0,charge=0;
};
struct AiBridgeVocalJobs::Running
{
	QString id;
	VocalInputSnapshot snapshot;
	std::atomic_bool cancel=false;
	std::atomic<VocalJobPhase> phase=VocalJobPhase::Authorizing;
	std::unique_ptr<QThread> thread;
	std::mutex resultMutex;
	VocalAnalysisResult result;
	quint64 charge=0;
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
AiBridgeVocalJobs::AiBridgeVocalJobs(QObject* p,VocalJobRunner runner,VocalThreadStarter starter):QObject(p),m_runner(std::move(runner)),m_starter(std::move(starter))
{
	if(!m_starter)m_starter=[](QThread& thread){thread.start();return thread.isRunning() || thread.isFinished();};
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
	bool changed=false;
	for(auto& [id,e]:m_entries)
		if(e->state.state!=VocalJobState::Invalidated && (e->state.generation!=gen || e->policyRevision!=revision))
		{
			e->state.state=VocalJobState::Invalidated;e->state.analysisId.clear();e->state.failureCode="job_invalidated";e->result={};
			if(m_active && m_active->id==id)m_active->cancel=true;
			changed=true;
		}
	if(changed)
	{
		const QPointer<AiBridgeVocalJobs> self(this);const bool notifying=m_invalidationNotification;m_invalidationNotification=true;
		emit jobsChanged();if(self)m_invalidationNotification=notifying;
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
		auto task=std::make_shared<Running>();task->id=uuid();task->snapshot=*captured.value;task->charge=budget.sessionBytes;
		task->thread=std::make_unique<QThread>();auto worker=std::make_unique<QObject>();worker->moveToThread(task->thread.get());
		connect(task->thread.get(),&QThread::finished,worker.get(),&QObject::deleteLater);
		const auto owner=QPointer<AiBridgeVocalJobs>(this);const auto weak=std::weak_ptr<Running>(task);const auto runner=m_runner;
		connect(task->thread.get(),&QThread::started,worker.get(),[weak,runner]{
			const auto running=weak.lock();if(!running)return;
			// Worker never dereferences a Qt owner, even to enqueue presentation work.
			// Qt polls this bounded mailbox; completion still uses QThread's queued signal.
			const auto phase=[&](VocalJobPhase value){running->phase.store(value);};
			VocalAnalysisResult result;
			try{result=runner(running->snapshot,running->cancel,phase);}catch(...){result.code="analysis_failed";}
			result.code=failureCode(result.code);
			{std::lock_guard lock(running->resultMutex);running->result=std::move(result);}
			QThread::currentThread()->quit();
		});
		connect(task->thread.get(),&QThread::finished,this,[owner,id=task->id]{if(owner)owner->complete(id);},Qt::QueuedConnection);
		auto entry=std::make_unique<Entry>();entry->request=request;entry->policyRevision=captured.value->policyRevision;entry->charge=budget.sessionBytes;
		entry->state.jobId=task->id;entry->state.inputHash=captured.value->inputHash;entry->state.generation=captured.value->generation;
		entry->state.state=VocalJobState::Running;
		m_entries.emplace(task->id,std::move(entry));m_charged+=budget.sessionBytes;m_active=task;
		bool started=false;
		try{started=m_starter(*task->thread);}catch(...){started=false;}
		const bool launched=task->thread->isRunning() || task->thread->isFinished();
		if(launched)worker.release(); // The thread-finished connection owns deletion after launch.
		if(!started || !launched)
		{
			task->cancel=true;task->thread->quit();task->thread->wait();
			m_entries.erase(task->id);m_charged-=task->charge;m_active.reset();
			return {"job_start_failed",{}, {}};
		}
		emit jobsChanged();
		if(!self || m_stopping)return {"job_invalidated",{}, {}};
		const auto live=m_entries.find(task->id);
		if(live==m_entries.end() || live->second->state.state==VocalJobState::Invalidated)return {"job_invalidated",{}, {}};
		return {"ok",task->id,captured.value->inputHash};
	}
	catch(const std::bad_alloc&){return {"resource_limit",{}, {}};}
}
void AiBridgeVocalJobs::complete(const QString& id)
{
	if(m_stopping || !m_active || m_active->id!=id)return;
	const QPointer<AiBridgeVocalJobs> self(this);
	const auto task=m_active;
	task->thread->wait();checkLifetime();
	if(!self || m_stopping || m_active!=task)return;
	auto it=m_entries.find(id);
	if(it!=m_entries.end())
	{
		auto& e=*it->second;std::lock_guard lock(m_active->resultMutex);
		if(e.state.state!=VocalJobState::Invalidated)
		{
			if(m_active->cancel.load() || e.state.state==VocalJobState::CancelRequested || m_active->result.code=="cancelled")
			{e.state.state=VocalJobState::Cancelled;e.state.failureCode="cancelled";}
			else if(m_active->result.code!="ok")
			{e.state.state=VocalJobState::Failed;e.state.failureCode=m_active->result.code;}
			else
			{
				e.result=std::move(m_active->result);e.state.state=VocalJobState::Succeeded;e.state.analysisId=uuid();
				e.state.phase=VocalJobPhase::Publishing;e.state.progress=1;
			}
		}
	}
	else m_charged-=m_active->charge;
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
VocalAnalysisPage AiBridgeVocalJobs::analysis(const QString& id,int offset,int limit)
{
	VocalAnalysisPage page;
	if(QThread::currentThread()!=thread()){page.code="wrong_thread";return page;}
	if(offset<0 || limit<1 || limit>VocalLimits::page){page.code="invalid_parameter";return page;}
	const QPointer<AiBridgeVocalJobs> self(this);
	checkLifetime();
	if(!self || m_stopping){page.code="job_invalidated";return page;}
	for(const auto& [key,e]:m_entries)if(e->state.analysisId==id && !id.isEmpty() && e->state.state==VocalJobState::Succeeded)
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
	const QPointer<AiBridgeVocalJobs> self(this);m_invalidationNotification=true;
	bool changed=false;
	for(auto& [id,e]:m_entries)if(e->state.state!=VocalJobState::Invalidated)
	{e->state.state=VocalJobState::Invalidated;e->state.analysisId.clear();e->result={};e->state.failureCode="job_invalidated";changed=true;}
	if(m_active)m_active->cancel=true;
	if(changed)emit jobsChanged();
	if(self)m_invalidationNotification=false;
}
void AiBridgeVocalJobs::shutdown()
{
	if(m_stopping)return;
	m_stopping=true;m_policyTimer.stop();
	if(m_active){m_active->cancel=true;m_active->thread->quit();m_active->thread->wait();m_active.reset();}
	m_entries.clear();m_charged=0;
}
quint64 AiBridgeVocalJobs::chargedSessionBytes()const{return m_charged;}
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
