#include "AiBridgeMidiExportJobs.h"

#include "AiBridgeProjectContext.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "MidiExportOutput.h"
#include "Song.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <mutex>

namespace lmms
{
namespace
{
enum class Phase { Capturing, Planning, Writing, Publishing, Finished };

QString newId(){return QUuid::createUuid().toString(QUuid::WithoutBraces);}

QJsonObject issueJson(const MidiExportIssue& issue)
{
	QJsonObject value{{"code",issue.code},{"severity",issue.severity},{"blocking",issue.blocking},{"count",qint64(issue.count)},{"message",issue.message}};
	if(!issue.trackId.isEmpty())value.insert("track_id",issue.trackId);
	if(!issue.clipId.isEmpty())value.insert("clip_id",issue.clipId);
	if(issue.tick>=0)value.insert("tick",issue.tick);
	return value;
}

QJsonObject summaryJson(const QString& operationId,const QString& jobId,const QString& kind,const QString& state,
	const MidiExportPlanResult& plan,const MidiExportFileResult& file,const QString& failureCode,bool outputRootConfigured,const QString& snapshotHash)
{
	QJsonObject data{{"operation_id",operationId},{"job_id",jobId},{"kind",kind},{"state",state},
		{"format",1},{"ppq",480},{"musical_tracks",plan.statistics.musicalTracks},{"midi_tracks",plan.statistics.midiTracks},
		{"notes_written",qint64(plan.statistics.notesWritten)},{"notes_clipped",qint64(plan.statistics.notesClipped)},
		{"notes_skipped",qint64(plan.statistics.notesSkipped)},{"tempo_events",qint64(plan.statistics.tempoEvents)},
		{"time_signature_events",qint64(plan.statistics.timeSignatureEvents)},
		{"controller_events",qint64(plan.statistics.controllerEvents)},{"pitch_bend_events",qint64(plan.statistics.pitchBendEvents)},
		{"program_events",qint64(plan.statistics.programEvents)},
		{"start_tick",plan.statistics.startTick},{"end_tick",plan.statistics.endTick},
		{"issue_count",plan.issues.size()},{"exportable",plan.success},{"output_root_configured",outputRootConfigured},{"snapshot_hash",snapshotHash},
		{"limits",QJsonObject{{"tracks",256},{"clips",4096},{"notes",262144},{"events",1000000},{"planning_iterations",1000000},{"output_bytes",134217728}}}};
	if(state=="succeeded"&&kind=="export")
	{
		data.insert("destination_path",file.destinationPath);data.insert("file_sha256",file.sha256);data.insert("bytes_written",file.bytesWritten);
	}
	if(!failureCode.isEmpty())data.insert("failure_code",failureCode);
	return data;
}

QJsonArray page(const QVector<MidiExportIssue>& issues,int offset,int limit)
{
	QJsonArray result;
	for(int i=offset;i<qMin(issues.size(),offset+limit);++i)result.append(issueJson(issues[i]));
	return result;
}
}

struct AiBridgeMidiExportJobs::Entry
{
	QString operationId,jobId,kind,state,failureCode,inputHash,snapshotHash;
	quint64 generation=0,order=0;
	bool outputRootConfigured=false;
	MidiExportPlanResult planned;
	MidiExportFileResult file;
	QVector<QString> trackNames;
	QVector<int> trackEventCounts;
};

struct AiBridgeMidiExportJobs::Running
{
	QString id,operationId,kind,filename,outputRoot;
	MidiExportSelection selection;
	MidiExportSnapshot snapshot;
	bool overwrite=false;
	std::atomic_bool cancelled=false;
	std::atomic<Phase> phase=Phase::Capturing;
	std::atomic_bool invalidated=false;
	std::unique_ptr<QThread> thread;
	std::mutex mutex,resultMutex;
	MidiExportPlanResult planned;
	MidiExportFileResult file;
	QString failureCode;
};

AiBridgeMidiExportJobs::AiBridgeMidiExportJobs(QObject* parent):QObject(parent)
{
	AiBridgeProjectContext identities;
	if(auto* song=Engine::getSong())connect(song,&Song::projectAboutToChange,this,&AiBridgeMidiExportJobs::invalidateProject,Qt::DirectConnection);
}

AiBridgeMidiExportJobs::~AiBridgeMidiExportJobs(){shutdown();}

MidiExportStartResult AiBridgeMidiExportJobs::start(const QString& operationId,const MidiExportSelection& selection,
	bool exportFile,const QString& filename,bool overwrite,const QJsonObject& normalizedRequest)
{
	if(QThread::currentThread()!=thread())return {"wrong_thread",{}};
	if(m_stopping)return {"job_invalidated",{}};
	const auto inputHash=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(normalizedRequest).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
	for(const auto& [id,entry]:m_entries)if(entry->operationId==operationId)
	{
		if(entry->inputHash!=inputHash)return {"operation_conflict",{}};
		return {"ok",QJsonObject{{"operation_id",entry->operationId},{"job_id",entry->jobId},{"state",entry->state}}};
	}
	if(m_active)return {"busy",{}};
	if(m_entries.size()>=8)
	{
		auto victim=m_entries.end();
		for(auto it=m_entries.begin();it!=m_entries.end();++it)
			if((it->second->state=="succeeded"||it->second->state=="failed"||it->second->state=="cancelled"||it->second->state=="invalidated")
				&&(victim==m_entries.end()||it->second->order<victim->second->order))victim=it;
		if(victim==m_entries.end())return {"resource_limit",{}};
		m_entries.erase(victim);
	}
	AiBridgeProjectContext identities;
	const auto captured=captureMidiExport(selection,identities);
	if(captured.code!="ok")return {captured.code,{}};
	const auto outputRoot=ConfigManager::inst()->value("aibridge","midi_export_root");
	if(exportFile&&outputRoot.isEmpty())return {"output_root_not_configured",{}};
	try
	{
		auto running=std::make_shared<Running>();running->id=newId();running->operationId=operationId;running->kind=exportFile?"export":"inspect";
		running->selection=selection;running->snapshot=captured.snapshot;running->filename=filename;running->overwrite=overwrite;
		running->outputRoot=outputRoot;
		running->phase=Phase::Planning;
		running->thread=std::make_unique<QThread>();auto* worker=new QObject;worker->moveToThread(running->thread.get());
		connect(running->thread.get(),&QThread::finished,worker,&QObject::deleteLater);
		const auto owner=QPointer<AiBridgeMidiExportJobs>(this);const auto weak=std::weak_ptr<Running>(running);
		connect(running->thread.get(),&QThread::started,worker,[weak,exportFile]{
			const auto task=weak.lock();if(!task)return;
			MidiExportPlanResult planned;
			MidiExportFileResult file;
			try
			{
				planned=planMidiExport(task->snapshot,task->selection,&task->cancelled);
				if(task->cancelled.load(std::memory_order_acquire))task->failureCode="cancelled";
				else if(exportFile&&planned.success)
				{
					task->phase=Phase::Writing;
					const auto permit=[weak]{
						const auto live=weak.lock();if(!live)return false;
						std::lock_guard lock(live->mutex);
						if(live->cancelled.load(std::memory_order_acquire)||live->invalidated.load(std::memory_order_acquire))return false;
						live->phase=Phase::Publishing;return true;
					};
					file=writeMidiExportFile(task->outputRoot,task->filename,task->overwrite,planned.plan,&task->cancelled,permit);
					if(!file.success)task->failureCode=file.code;
				}
				else if(!planned.success)task->failureCode=planned.code;
			}
			catch(...){task->failureCode="internal_error";}
			{std::lock_guard lock(task->resultMutex);task->planned=std::move(planned);task->file=std::move(file);}
			task->phase=Phase::Finished;
			QThread::currentThread()->quit();
		});
		connect(running->thread.get(),&QThread::finished,this,[owner,id=running->id]{if(owner)owner->complete(id);},Qt::QueuedConnection);
		auto entry=std::make_unique<Entry>();entry->operationId=operationId;entry->jobId=running->id;entry->kind=running->kind;
		entry->state="planning";entry->generation=running->snapshot.projectGeneration;entry->order=++m_order;entry->inputHash=inputHash;entry->outputRootConfigured=!outputRoot.isEmpty();entry->snapshotHash=running->snapshot.contentHash;
		m_entries.emplace(operationId,std::move(entry));m_active=running;
		running->thread->start();
		if(!running->thread->isRunning()&&!running->thread->isFinished()){m_entries.erase(operationId);m_active.reset();return {"job_start_failed",{}};}
		return {"ok",QJsonObject{{"operation_id",operationId},{"job_id",running->id},{"state","planning"}}};
	}
	catch(const std::bad_alloc&){return {"resource_limit",{}};}
	catch(...){return {"job_start_failed",{}};}
}

void AiBridgeMidiExportJobs::complete(const QString& jobId)
{
	if(!m_active||m_active->id!=jobId)return;
	const auto running=m_active;running->thread->wait();
	auto it=m_entries.find(running->operationId);if(it==m_entries.end()){m_active.reset();return;}
	MidiExportPlanResult planned;MidiExportFileResult file;
	{std::lock_guard lock(running->resultMutex);planned=std::move(running->planned);file=std::move(running->file);}
	for(auto& track:planned.plan.tracks){it->second->trackNames.append(track.name);it->second->trackEventCounts.append(track.events.size());track.events.clear();track.events.squeeze();}
	planned.plan.tracks.squeeze();
	it->second->planned=std::move(planned);it->second->file=std::move(file);
	if(running->invalidated.load(std::memory_order_acquire))it->second->state="invalidated";
	else if(running->cancelled.load(std::memory_order_acquire)||running->failureCode=="cancelled")it->second->state="cancelled";
	else if(!running->failureCode.isEmpty()||!it->second->planned.success){it->second->state="failed";it->second->failureCode=running->failureCode.isEmpty()?it->second->planned.code:running->failureCode;}
	else it->second->state="succeeded";
	m_active.reset();
}

MidiExportQueryResult AiBridgeMidiExportJobs::status(const QString& operationId,const QString& section,int offset,int limit)
{
	if(QThread::currentThread()!=thread())return {"wrong_thread",{}};
	auto it=m_entries.find(operationId);if(it==m_entries.end())return {"job_not_found",{}};
	if(m_active&&m_active->operationId==operationId)
	{
		if(m_active->cancelled.load())it->second->state="cancel_requested";
		else switch(m_active->phase.load()){case Phase::Capturing:it->second->state="capturing";break;case Phase::Planning:it->second->state="planning";break;case Phase::Writing:it->second->state="writing";break;case Phase::Publishing:it->second->state="publishing";break;case Phase::Finished:break;}
	}
	const auto& e=*it->second;auto data=summaryJson(e.operationId,e.jobId,e.kind,e.state,e.planned,e.file,e.failureCode,e.outputRootConfigured,e.snapshotHash);
	data.insert("project_generation",QString::number(e.generation));
	if(section=="tracks")
	{
		QJsonArray rows;const int total=e.trackNames.size();const int safeOffset=qBound(0,offset,total);const int safeLimit=qBound(1,limit,64);const int end=qMin(total,safeOffset+safeLimit);
		for(int i=safeOffset;i<end;++i)rows.append(QJsonObject{{"index",i},{"name",e.trackNames[i]},{"events",e.trackEventCounts[i]}});
		data.insert("tracks",rows);data.insert("total",total);data.insert("offset",safeOffset);data.insert("limit",safeLimit);data.insert("has_more",end<total);
	}
	else if(section=="issues")
	{
		const int safeOffset=qBound(0,offset,e.planned.issues.size()),safeLimit=qBound(1,limit,64);
		data.insert("issues",page(e.planned.issues,safeOffset,safeLimit));data.insert("total",e.planned.issues.size());data.insert("offset",safeOffset);data.insert("limit",safeLimit);data.insert("has_more",safeOffset+safeLimit<e.planned.issues.size());
	}
	return {"ok",data};
}

MidiExportQueryResult AiBridgeMidiExportJobs::list()
{
	if(QThread::currentThread()!=thread())return {"wrong_thread",{}};
	QJsonArray jobs;for(const auto& [id,entry]:m_entries)jobs.append(summaryJson(entry->operationId,entry->jobId,entry->kind,entry->state,entry->planned,entry->file,entry->failureCode,entry->outputRootConfigured,entry->snapshotHash));
	return {"ok",QJsonObject{{"jobs",jobs},{"total",jobs.size()}}};
}

MidiExportQueryResult AiBridgeMidiExportJobs::cancel(const QString& operationId)
{
	if(QThread::currentThread()!=thread())return {"wrong_thread",{}};
	auto it=m_entries.find(operationId);if(it==m_entries.end())return {"job_not_found",{}};
	bool applied=false;
	if(m_active&&m_active->operationId==operationId)
	{
		std::lock_guard lock(m_active->mutex);
		if(m_active->phase.load()!=Phase::Publishing&&m_active->phase.load()!=Phase::Finished){m_active->cancelled=true;applied=true;it->second->state="cancel_requested";}
	}
	return {"ok",QJsonObject{{"operation_id",it->second->operationId},{"job_id",it->second->jobId},
		{"state",it->second->state},{"cancel_applied",applied}}};
}

MidiExportQueryResult AiBridgeMidiExportJobs::release(const QString& operationId)
{
	if(QThread::currentThread()!=thread())return {"wrong_thread",{}};
	auto it=m_entries.find(operationId);if(it==m_entries.end())return {"job_not_found",{}};
	if(m_active&&m_active->operationId==operationId)return {"job_running",{}};
	if(it->second->state!="succeeded"&&it->second->state!="failed"&&it->second->state!="cancelled"&&it->second->state!="invalidated")return {"job_running",{}};
	m_entries.erase(it);return {"ok",QJsonObject{{"operation_id",operationId},{"released",true}}};
}

void AiBridgeMidiExportJobs::invalidateProject()
{
	if(!m_active)return;
	std::lock_guard lock(m_active->mutex);
	const auto phase=m_active->phase.load();
	if(phase!=Phase::Publishing&&phase!=Phase::Finished){m_active->invalidated=true;m_active->cancelled=true;}
}

void AiBridgeMidiExportJobs::shutdown()
{
	if(m_stopping)return;
	m_stopping=true;
	if(m_active){
		{std::lock_guard lock(m_active->mutex);if(m_active->phase.load()!=Phase::Publishing)m_active->cancelled=true;}
		if(m_active->thread){m_active->thread->quit();m_active->thread->wait();}
		m_active.reset();
	}
}

} // namespace lmms
