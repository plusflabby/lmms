#ifndef LMMS_AI_BRIDGE_VOCAL_JOBS_H
#define LMMS_AI_BRIDGE_VOCAL_JOBS_H
#include "AiBridgeVocalTypes.h"
#include <QObject>
#include <QTimer>
#include <map>
#include <memory>
#include <functional>
namespace lmms {
using VocalPhaseCallback=std::function<void(VocalJobPhase)>;
using VocalJobRunner=std::function<VocalAnalysisResult(const VocalInputSnapshot&,const std::atomic_bool&,const VocalPhaseCallback&)>;
VocalAnalysisResult runVocalAnalysis(const VocalInputSnapshot&,const std::atomic_bool&,const VocalPhaseCallback&);
class AiBridgeVocalJobs:public QObject
{
	Q_OBJECT
public:
	explicit AiBridgeVocalJobs(QObject* parent=nullptr,VocalJobRunner runner=runVocalAnalysis);
	~AiBridgeVocalJobs() override;
	VocalStartResult start(const VocalAnalyzeRequest&);
	VocalJobResult job(const QString&);
	VocalJobResult cancel(const QString&);
	VocalJobResult release(const QString&);
	VocalAnalysisPage analysis(const QString&,int offset,int limit);
	void invalidateProject();
	void shutdown();
	quint64 chargedSessionBytes() const;
	std::vector<VocalJobSnapshot> snapshots();
signals:
	void jobsChanged();
private:
	struct Entry;
	struct Running;
	std::map<QString,std::unique_ptr<Entry>> m_entries;
	std::shared_ptr<Running> m_active;
	VocalJobRunner m_runner;
	QTimer m_policyTimer;
	quint64 m_charged=0;
	bool m_stopping=false;
	void checkLifetime();
	void complete(const QString&);
};
}
#endif
