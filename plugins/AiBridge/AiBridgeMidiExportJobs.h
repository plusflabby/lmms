#ifndef LMMS_AIBRIDGE_MIDI_EXPORT_JOBS_H
#define LMMS_AIBRIDGE_MIDI_EXPORT_JOBS_H

#include "AiBridgeMidiExportCapture.h"
#include <QObject>
#include <QJsonObject>
#include <QPointer>
#include <atomic>
#include <memory>
#include <mutex>
#include <map>

class QThread;

namespace lmms
{

struct MidiExportStartResult{QString code;QJsonObject data;};
struct MidiExportQueryResult{QString code;QJsonObject data;};

class AiBridgeMidiExportJobs final : public QObject
{
public:
	explicit AiBridgeMidiExportJobs(QObject* parent=nullptr);
	~AiBridgeMidiExportJobs() override;
	MidiExportStartResult start(const QString& operationId,const MidiExportSelection&,bool exportFile,
		const QString& filename,bool overwrite,const QJsonObject& normalizedRequest);
	MidiExportQueryResult status(const QString& operationId,const QString& section,int offset,int limit);
	MidiExportQueryResult list();
	MidiExportQueryResult cancel(const QString& operationId);
	MidiExportQueryResult release(const QString& operationId);
	void invalidateProject();
	void shutdown();
private:
	struct Entry;
	struct Running;
	std::map<QString,std::unique_ptr<Entry>> m_entries;
	std::shared_ptr<Running> m_active;
	quint64 m_order=0;
	bool m_stopping=false;
	void complete(const QString& jobId);
};

} // namespace lmms

#endif // LMMS_AIBRIDGE_MIDI_EXPORT_JOBS_H
