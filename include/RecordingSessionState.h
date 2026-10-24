#ifndef LMMS_RECORDING_SESSION_STATE_H
#define LMMS_RECORDING_SESSION_STATE_H
#include "RecordingTypes.h"
#include <QHash>
#include <optional>
#include <utility>
namespace lmms::recording {
// Qt/control-thread-only pure arbitration. No device, file or model behavior.
// No IPC route receives a LocalArmContext or exposes arm/disarm.
class LMMS_EXPORT RecordingSessionState
{
public:
 explicit RecordingSessionState(LocalArmContext current):m_context(std::move(current)){}
 RecordingSessionState(const RecordingSessionState&)=delete;
 RecordingSessionState& operator=(const RecordingSessionState&)=delete;
 Status status() const {return m_status;}
 Result arm(const LocalArmContext&);
 Result disarm();
 void invalidate(const LocalArmContext&);
 Result prepare(const PreparedTake&);
 Result start(const QString& preparedId,const QString& startRequestId,quint64 expectedGeneration);
 Result stop(const QString& takeId);
 Result primed(const QString& takeId);
 Result captureStarted(const QString& takeId);
 Result failPriming(const QString& takeId);
 Result quiesced(const QString& takeId);
 Result finish(const QString& takeId,TakeOutcome);
private:
 LocalArmContext m_context;
 Status m_status;
 std::optional<PreparedTake> m_prepared;
 QHash<QString,Result> m_terminals;
 QHash<QString,QString> m_requests;
 bool m_captured=false,m_primingFailed=false;
 Result result(bool ok,const QString& code) const {return {ok,code,m_status};}
 bool busy() const;
 Result terminal(TakeOutcome,const QString& code);
 Result retained(const QString& takeId) const;
};
}
#endif
