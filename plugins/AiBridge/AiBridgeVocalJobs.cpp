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
AiBridgeVocalJobs::AiBridgeVocalJobs(QObject* p,VocalJobRunner runner):QObject(p),m_runner(std::move(runner))
{
	if(auto* song=Engine::getSong())connect(song,&Song::projectAboutToChange,this,&AiBridgeVocalJobs::invalidateProject,Qt::DirectConnection);
	m_policyTimer.setInterval(25);connect(&m_policyTimer,&QTimer::timeout,this,&AiBridgeVocalJobs::checkLifetime);m_policyTimer.start();
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
	if(changed)emit jobsChanged();
}
VocalStartResult AiBridgeVocalJobs::start(const VocalAnalyzeRequest& request)
{
	if(QThread::currentThread()!=thread())return {"wrong_thread",{}, {}};
	if(m_stopping)return {"job_invalidated",{}, {}};
	checkLifetime();
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
		connect(task->thread.get(),&QThread::started,worker.get(),[owner,weak,runner]{
			const auto running=weak.lock();if(!running)return;
			// Coalesce phase changes: never enqueue one Qt callback per PCM chunk.
			std::optional<VocalJobPhase> last;
			const auto phase=[&](VocalJobPhase value){
				if(last && *last==value)return;
				last=value;
				if(owner)QMetaObject::invokeMethod(owner,[owner,id=running->id,value]{
					if(!owner || owner->m_stopping)return;
					const auto it=owner->m_entries.find(id);
					if(it!=owner->m_entries.end() && it->second->state.state==VocalJobState::Running)
					{it->second->state.phase=value;emit owner->jobsChanged();}
				},Qt::QueuedConnection);
			};
			VocalAnalysisResult result;
			try{result=runner(running->snapshot,running->cancel,phase);}catch(...){result.code="analysis_failed";}
			{std::lock_guard lock(running->resultMutex);running->result=std::move(result);}
			QThread::currentThread()->quit();
		});
		connect(task->thread.get(),&QThread::finished,this,[owner,id=task->id]{if(owner)owner->complete(id);},Qt::QueuedConnection);
		auto entry=std::make_unique<Entry>();entry->request=request;entry->policyRevision=captured.value->policyRevision;entry->charge=budget.sessionBytes;
		entry->state.jobId=task->id;entry->state.inputHash=captured.value->inputHash;entry->state.generation=captured.value->generation;
		entry->state.state=VocalJobState::Running;
		m_entries.emplace(task->id,std::move(entry));m_charged+=budget.sessionBytes;m_active=task;
		task->thread->start();worker.release();emit jobsChanged();return {"ok",task->id,captured.value->inputHash};
	}
	catch(const std::bad_alloc&){return {"resource_limit",{}, {}};}
}
void AiBridgeVocalJobs::complete(const QString& id)
{
	if(m_stopping || !m_active || m_active->id!=id)return;
	m_active->thread->wait();checkLifetime();
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
	checkLifetime();const auto it=m_entries.find(id);if(it==m_entries.end())return {"stale_id",{},false};
	return {"ok",it->second->state,false};
}
VocalJobResult AiBridgeVocalJobs::cancel(const QString& id)
{
	auto state=job(id);if(!state.value || terminal(state.value->state))return state;
	if(m_active && m_active->id==id)
	{
		m_active->cancel=true;m_entries.at(id)->state.state=VocalJobState::CancelRequested;
		state.value=m_entries.at(id)->state;state.cancelApplied=true;emit jobsChanged();
	}
	return state;
}
VocalJobResult AiBridgeVocalJobs::release(const QString& id)
{
	auto state=job(id);if(!state.value)return state;
	if(!terminal(state.value->state))return {"job_busy",{},false};
	const auto it=m_entries.find(id);
	if(!m_active || m_active->id!=id)m_charged-=it->second->charge;
	m_entries.erase(it);emit jobsChanged();return state;
}
VocalAnalysisPage AiBridgeVocalJobs::analysis(const QString& id,int offset,int limit)
{
	VocalAnalysisPage page;
	if(QThread::currentThread()!=thread()){page.code="wrong_thread";return page;}
	if(offset<0 || limit<1 || limit>VocalLimits::page){page.code="invalid_parameter";return page;}
	checkLifetime();
	for(const auto& [key,e]:m_entries)if(e->state.analysisId==id && !id.isEmpty() && e->state.state==VocalJobState::Succeeded)
	{
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
	if(QThread::currentThread()!=thread())return;
	for(auto& [id,e]:m_entries){e->state.state=VocalJobState::Invalidated;e->state.analysisId.clear();e->result={};e->state.failureCode="job_invalidated";}
	if(m_active)m_active->cancel=true;
	emit jobsChanged();
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
	checkLifetime();std::vector<VocalJobSnapshot> result;result.reserve(m_entries.size());
	for(const auto& [id,e]:m_entries)result.push_back(e->state);
	return result;
}
}
