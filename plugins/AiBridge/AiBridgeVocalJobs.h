#ifndef LMMS_AI_BRIDGE_VOCAL_JOBS_H
#define LMMS_AI_BRIDGE_VOCAL_JOBS_H
#include "AiBridgeVocalAssetActions.h"
#include "AiBridgeVocalTypes.h"
#include <QObject>
#include <QTimer>
#include <map>
#include <memory>
#include <functional>
class QThread;
namespace lmms {
using VocalJobRunner=std::function<VocalAnalysisResult(const VocalInputSnapshot&,const std::atomic_bool&,const VocalPhaseCallback&)>;
using VocalRenderRunner=std::function<VocalVariantRenderResult(
	const VocalEditRenderInput&,const std::atomic_bool&,const VocalPhaseCallback&)>;
// Internal native dependency; never configurable through IPC or MCP.
using VocalThreadStarter=std::function<bool(QThread&)>;
VocalAnalysisResult runVocalAnalysis(const VocalInputSnapshot&,const std::atomic_bool&,const VocalPhaseCallback&);
struct VocalEditPlanHandle
{
	QString planId;
	VocalEditRequest request;
	VocalEditPlan plan;
	VocalEditPlanningSnapshot planning;
	VocalEditLimits limits;
};
struct VocalEditHandleResult
{
	QString code;
	std::optional<VocalEditPlanHandle> value;
};
struct VocalRenderAdmissionResult
{
	QString code;
	QString planId,planHash,jobId;
	bool dryRun=false;
	VocalRenderBudget budget;
};
class AiBridgeVocalJobs:public QObject
{
	Q_OBJECT
public:
	explicit AiBridgeVocalJobs(QObject* parent=nullptr,VocalJobRunner runner=runVocalAnalysis,
		VocalThreadStarter starter={},VocalRenderRunner renderRunner={});
	~AiBridgeVocalJobs() override;
	VocalStartResult start(const VocalAnalyzeRequest&);
	VocalStartResult startRender(const VocalEditRenderInput&);
	VocalEditHandleResult planEdit(const VocalEditRequest&);
	VocalRenderAdmissionResult renderPlan(const QString& planId,const QString& planHash,bool dryRun);
	VocalJobResult job(const QString&);
	VocalJobResult cancel(const QString&);
	VocalJobResult release(const QString&);
	VocalVariantRenderResult renderedVariant(const QString&);
	VocalAnalysisPage analysis(const QString&,int offset,int limit);
	void invalidateProject();
	void shutdown();
	quint64 chargedSessionBytes() const;
	quint64 chargedWorkerBytes() const;
	quint64 chargedStagedBytes() const;
	bool isAvailable() const { return !m_stopping && !m_invalidationNotification; }
	std::vector<VocalJobSnapshot> snapshots();
signals:
	void jobsChanged();
private:
	struct Entry;
	struct Running;
	struct PlanEntry;
	std::map<QString,std::unique_ptr<Entry>> m_entries;
	std::map<QString,std::unique_ptr<PlanEntry>> m_plans;
	std::shared_ptr<Running> m_active;
	VocalJobRunner m_runner;
	VocalRenderRunner m_renderRunner;
	VocalThreadStarter m_starter;
	QTimer m_policyTimer;
	quint64 m_charged=0;
	quint64 m_workerCharged=0;
	quint64 m_stagedCharged=0;
	quint64 m_planCharged=0;
	bool m_stopping=false;
	bool m_invalidationNotification=false;
	void checkLifetime();
	void purgePlans();
	void complete(const QString&);
	VocalStartResult startWorker(std::shared_ptr<Running> task,std::unique_ptr<Entry> entry);
};
}
#endif
