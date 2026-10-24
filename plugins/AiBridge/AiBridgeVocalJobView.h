#ifndef LMMS_AI_BRIDGE_VOCAL_JOB_VIEW_H
#define LMMS_AI_BRIDGE_VOCAL_JOB_VIEW_H
#include <QWidget>
#include <QPointer>
#include <QTimer>
#include "AiBridgeVocalJobs.h"
class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
namespace lmms {
class AiBridgeVocalJobView:public QWidget
{
public:
	explicit AiBridgeVocalJobView(AiBridgeVocalJobs*,QWidget* parent=nullptr);
	void refreshNow();
private:
	QPointer<AiBridgeVocalJobs> m_service;
	QTimer m_refreshTimer;
	QComboBox* m_jobs=nullptr;
	QLabel* m_status=nullptr;
	QPlainTextEdit* m_details=nullptr;
	QPushButton *m_cancel=nullptr,*m_release=nullptr,*m_previous=nullptr,*m_next=nullptr;
	int m_offset=0;
	bool m_refreshing=false,m_updatingList=false;
	void scheduleRefresh();
	void sessionAction(bool release);
};
}
#endif
