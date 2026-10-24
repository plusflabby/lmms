#include "AiBridgeVocalJobView.h"
#include "AiBridgeVocalWire.h"
#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QVBoxLayout>
namespace lmms {
AiBridgeVocalJobView::AiBridgeVocalJobView(AiBridgeVocalJobs* jobs,QWidget* parent):QWidget(parent),m_service(jobs),m_refreshTimer(this)
{
	auto* layout=new QVBoxLayout(this);
	layout->addWidget(new QLabel(tr("Vocal source analysis — no project or audio edits"),this));
	m_jobs=new QComboBox(this);m_jobs->setObjectName("vocalJobSelector");layout->addWidget(m_jobs);
	m_status=new QLabel(this);m_status->setObjectName("vocalStatus");layout->addWidget(m_status);
	m_details=new QPlainTextEdit(this);m_details->setObjectName("vocalDetails");m_details->setReadOnly(true);layout->addWidget(m_details);
	auto* controls=new QHBoxLayout();layout->addLayout(controls);
	auto* refresh=new QPushButton(tr("Refresh"),this);refresh->setObjectName("vocalRefresh");controls->addWidget(refresh);
	m_cancel=new QPushButton(tr("Cancel analysis"),this);m_cancel->setObjectName("vocalCancel");controls->addWidget(m_cancel);
	m_release=new QPushButton(tr("Release result"),this);m_release->setObjectName("vocalRelease");controls->addWidget(m_release);
	m_previous=new QPushButton(tr("Previous"),this);m_previous->setObjectName("vocalPrevious");controls->addWidget(m_previous);
	m_next=new QPushButton(tr("Next"),this);m_next->setObjectName("vocalNext");controls->addWidget(m_next);
	for(auto* button:{m_cancel,m_release,m_previous,m_next})button->setEnabled(false);
	connect(refresh,&QPushButton::clicked,this,[this]{scheduleRefresh();});
	connect(m_cancel,&QPushButton::clicked,this,[this]{sessionAction(false);});
	connect(m_release,&QPushButton::clicked,this,[this]{sessionAction(true);});
	connect(m_previous,&QPushButton::clicked,this,[this]{m_offset=std::max(0,m_offset-32);scheduleRefresh();});
	connect(m_next,&QPushButton::clicked,this,[this]{m_offset+=32;scheduleRefresh();});
	connect(m_jobs,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{if(!m_updatingList){m_offset=0;scheduleRefresh();}});
	m_refreshTimer.setSingleShot(true);m_refreshTimer.setInterval(0);
	connect(&m_refreshTimer,&QTimer::timeout,this,[this]{refreshNow();});
	if(m_service)
	{
		connect(m_service,&AiBridgeVocalJobs::jobsChanged,this,[this]{scheduleRefresh();},Qt::QueuedConnection);
		connect(m_service,&QObject::destroyed,this,[this]{scheduleRefresh();});
	}
	scheduleRefresh();
}
void AiBridgeVocalJobView::scheduleRefresh(){if(!m_refreshTimer.isActive())m_refreshTimer.start();}
void AiBridgeVocalJobView::sessionAction(bool release)
{
	const QPointer<AiBridgeVocalJobView> self(this);
	if(!m_service || !m_service->isAvailable())return;
	const auto id=m_jobs->currentData().toString();if(id.isEmpty())return;
	const QJsonObject parameters=release?QJsonObject{{"job_id",id},{"mode","release"}}:QJsonObject{{"job_id",id}};
	const auto result=AiBridgeVocalWire::execute({2,"inspector",release?"get_vocal_job":"cancel_vocal_job",parameters},m_service);
	if(!self)return;
	// This is one explicit session action, never an automatic mutation retry.
	if(!result.success){m_status->setText(tr("Session action rejected: %1").arg(result.code));if(!self)return;}
	scheduleRefresh();
}
void AiBridgeVocalJobView::refreshNow()
{
	if(m_refreshing)return;
	m_refreshing=true;const QPointer<AiBridgeVocalJobView> self(this);
	const auto done=qScopeGuard([self]{if(self)self->m_refreshing=false;});
	m_refreshTimer.stop();
	const auto states=m_service?m_service->snapshots():std::vector<VocalJobSnapshot>{};
	if(!self)return;
	const bool available=m_service && m_service->isAvailable();const auto selected=m_jobs->currentData().toString();
	m_updatingList=true;m_jobs->clear();if(!self)return;
	if(available)for(const auto& state:states)
	{
		m_jobs->addItem(state.jobId.left(8),state.jobId);if(!self)return;
	}
	const auto index=m_jobs->findData(selected);if(index>=0)m_jobs->setCurrentIndex(index);if(!self)return;
	m_updatingList=false;const auto id=m_jobs->currentData().toString();if(id!=selected)m_offset=0;
	QString status=available?tr("No analyses in this session"):tr("Vocal analysis service unavailable");
	QString details;bool canCancel=false,canRelease=false,hasPrevious=false,hasNext=false;
	if(available && !id.isEmpty())
	{
		const auto response=AiBridgeVocalWire::execute({2,"inspector","get_vocal_job",{{"job_id",id},{"mode","status"}}},m_service);
		if(!self)return;
		if(response.success)
		{
			const auto job=response.data.toObject();const auto state=job["state"].toString();
			status=tr("%1 — %2").arg(state,job["phase"].toString());
			canCancel=state=="queued" || state=="running";
			canRelease=state=="succeeded" || state=="failed" || state=="cancelled" || state=="invalidated";
			if(state=="failed" || state=="invalidated")details=tr("Analysis unavailable: %1").arg(job["failure_code"].toString());
			if(state=="succeeded")
			{
				const auto page=AiBridgeVocalWire::execute({2,"inspector","get_vocal_analysis",{{"analysis_id",job["analysis_id"]},{"offset",m_offset},{"limit",32}}},m_service);
				if(!self)return;
				if(!page.success)details=tr("Analysis inspection rejected: %1").arg(page.code);
				else
				{
					const auto data=page.data.toObject();
					details=tr("Source before mixer; measured snapshot.\nApplicability: %1\n").arg(data["applicability"].toString());
					int number=0;for(const auto& value:data["summaries"].toArray())
					{
						const auto summary=value.toObject();details+=tr("\nSelection %1: %2 Hz, %3 channel(s)\n").arg(++number).arg(summary["sample_rate"].toInt()).arg(summary["channels"].toInt());
						int channel=0;for(const auto& metric:summary["metrics"].toArray())
						{
							const auto m=metric.toObject();const auto db=[](const QJsonValue& value){return value.isNull()?QString("silence"):QString::number(value.toDouble(),'f',2)+" dBFS";};
							details+=tr("Channel %1: peak %2; RMS %3; DC %4; full-scale candidates %5\n").arg(++channel).arg(db(m["peak_dbfs"]),db(m["rms_dbfs"]),QString::number(m["dc"].toDouble(),'g',7),m["full_scale_candidates"].toString());
						}
					}
					details+=tr("\nRecords %1–%2 of %3 (source frames):\n").arg(data["records"].toArray().isEmpty()?0:data["offset"].toInt()+1).arg(data["offset"].toInt()+data["records"].toArray().size()).arg(data["total"].toInt());
					for(const auto& value:data["records"].toArray())
					{
						const auto record=value.toObject();const auto frames=record["source_frames"].toObject();
						details+=tr("Selection %1: %2 [%3, %4)\n").arg(record["selection_index"].toInt()+1).arg(record["kind"]=="window"?tr("Window"):tr("Low-level span"),frames["in_frame"].toString(),frames["out_frame"].toString());
					}
					hasPrevious=m_offset>0;hasNext=data["has_more"].toBool();
				}
			}
		}
		else status=tr("Status unavailable: %1").arg(response.code);
	}
	m_status->setText(status);if(!self)return;
	m_cancel->setEnabled(canCancel);if(!self)return;
	m_release->setEnabled(canRelease);if(!self)return;
	m_previous->setEnabled(hasPrevious);if(!self)return;
	m_next->setEnabled(hasNext);if(!self)return;
	m_details->setPlainText(details);
}
}
