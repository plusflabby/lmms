#include "RecordingSessionState.h"
#include <QUuid>
namespace lmms::recording {
Result RecordingSessionState::quiesced(const QString& take)
{
 if(take!=m_status.takeId)return result(false,"stale_take");
 if(m_status.phase!=Phase::Quiescing)return result(false,"invalid_state");
 if(m_captured){m_status.phase=Phase::Finalizing;return result(true,"finalizing");}
 const bool failed=m_primingFailed;
 auto completed=terminal(TakeOutcome::NoTakeCreated,failed?"render_unavailable":"no_take_created");
 if(failed){completed.ok=false;m_terminals.insert(take,completed);}
 return completed;
}
namespace {
bool opaque(const QString& text)
{
 if(text.isEmpty() || text.size()>128)return false;
 for(const auto c:text)if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-' || c=='_'))return false;
 return true;
}
bool contextValid(const LocalArmContext& c)
{
 return c.projectGeneration && opaque(c.processId) && opaque(c.projectId) && opaque(c.deviceId) && opaque(c.libraryRootId);
}
bool uuid(const QString& text)
{
 const QUuid value(text);return !value.isNull() && value.toString(QUuid::WithoutBraces)==text;
}
bool hash(const QString& text)
{
 if(text.size()!=64)return false;
 for(const auto c:text)if(!((c>='a' && c<='f') || (c>='0' && c<='9')))return false;
 return true;
}
}
bool RecordingSessionState::busy() const
{
 return m_status.phase==Phase::Priming || m_status.phase==Phase::Preroll || m_status.phase==Phase::Recording || m_status.phase==Phase::Quiescing || m_status.phase==Phase::Finalizing;
}
Result RecordingSessionState::failPriming(const QString& take)
{
 if(take!=m_status.takeId)return result(false,"stale_take");
 if(m_status.phase!=Phase::Priming)return result(false,"invalid_state");
 m_primingFailed=true;m_status.phase=Phase::Quiescing;
 return result(false,"render_unavailable");
}
Result RecordingSessionState::retained(const QString& take) const
{
 auto reply=m_terminals.value(take);
 // Terminal take metadata is retained; an old snapshot is never authority to rearm.
 reply.status.armed=m_status.armed;
 reply.status.phase=m_status.armed?Phase::Armed:Phase::Disarmed;
 return reply;
}
Result RecordingSessionState::arm(const LocalArmContext& context)
{
 if(context!=m_context)return result(false,"arm_expired");
 if(!contextValid(context))return result(false,"invalid_state");
 if(busy())return result(false,"recording_busy");
 m_status.armed=true;
 if(m_status.phase!=Phase::Prepared)m_status.phase=Phase::Armed;
 return result(true,"armed");
}
Result RecordingSessionState::disarm()
{
 m_status.armed=false;
 if(busy() || m_status.phase==Phase::Prepared)return stop(m_status.takeId);
 m_status.phase=Phase::Disarmed;
 return result(true,"disarmed");
}
void RecordingSessionState::invalidate(const LocalArmContext& context)
{
 if(context==m_context)return;
 m_status.armed=false;
 if(m_captured)m_status.incomplete=true;
 if(busy() || m_status.phase==Phase::Prepared)stop(m_status.takeId);
 else m_status.phase=Phase::Disarmed;
 m_context=context;m_requests.clear();m_terminals.clear();
}
Result RecordingSessionState::prepare(const PreparedTake& take)
{
 if(busy())return result(false,"recording_busy");
 if(take.context!=m_context)return result(false,"stale_project");
 if(!contextValid(take.context) || take.startTick<0 || take.countInBars<0 || take.countInBars>2
  || take.label.trimmed().isEmpty() || take.label.toUtf8().size()>256 || !hash(take.transportHash))return result(false,"invalid_request");
 for(const auto c:take.label)if(c.unicode()<32)return result(false,"invalid_request");
 if(take.sampleRate!=44100 && take.sampleRate!=48000)return result(false,"unsupported_rate");
 const bool armed=m_status.armed;
 m_prepared=take;m_prepared->preparedId=QUuid::createUuid().toString(QUuid::WithoutBraces);
 m_status={};m_status.armed=armed;m_status.phase=Phase::Prepared;
 m_status.projectGeneration=m_context.projectGeneration;
 m_captured=false;m_primingFailed=false;
 // Stable potential take ID exists at preparation, but represents metadata only.
 // There can be at most one actual take for a preparation; no file or stream is created here.
 m_status.preparedId=m_prepared->preparedId;m_status.takeId=m_status.preparedId;
 return result(true,"prepared");
}
Result RecordingSessionState::start(const QString& prepared,const QString& request,quint64 generation)
{
 if(generation!=m_context.projectGeneration)return result(false,"stale_project");
 if(!uuid(prepared) || !uuid(request))return result(false,"invalid_request");
 if(m_requests.contains(request))
 {
  const auto take=m_requests.value(request);
  if(take!=prepared)return result(false,"conflicting_start");
  if(m_terminals.contains(take))return retained(take);
  if(take==m_status.takeId && busy())return result(true,"start_accepted");
  return result(false,"stale_preparation");
 }
 if(busy())return result(false,"recording_busy");
 if(!m_prepared || m_prepared->preparedId!=prepared)return result(false,"stale_preparation");
 if(!m_status.armed)return result(false,"not_armed");
 if(m_prepared->context!=m_context)return result(false,"arm_expired");
 // Never evict old request IDs and accidentally allow a replay to start again.
 if(m_requests.size()>=64 || m_terminals.size()>=64)return result(false,"resource_limit");
 m_requests.insert(request,m_status.takeId);m_status.startRequestId=request;m_status.phase=Phase::Priming;
 return result(true,"start_accepted");
}
Result RecordingSessionState::terminal(TakeOutcome outcome,const QString& code)
{
 m_status.outcome=outcome;m_status.phase=m_status.armed?Phase::Armed:Phase::Disarmed;m_prepared.reset();
 m_captured=false;m_primingFailed=false;
 const auto completed=result(true,code);
 if(m_terminals.size()<64)m_terminals.insert(m_status.takeId,completed);
 return completed;
}
Result RecordingSessionState::stop(const QString& take)
{
 if(m_terminals.contains(take))return retained(take);
 if(take.isEmpty() || take!=m_status.takeId)return result(false,"stale_take");
 if(m_status.phase==Phase::Finalizing)return result(true,"finalizing");
 if(m_status.phase==Phase::Quiescing)return result(!m_primingFailed,m_primingFailed?"render_unavailable":"quiescing");
 if(m_status.phase==Phase::Recording || m_status.phase==Phase::Priming || m_status.phase==Phase::Preroll)
 {m_status.phase=Phase::Quiescing;return result(true,"quiescing");}
 if(m_status.phase==Phase::Prepared)return terminal(TakeOutcome::NoTakeCreated,"no_take_created");
 if(m_status.outcome==TakeOutcome::NoTakeCreated)return result(true,"no_take_created");
 return result(false,"invalid_state");
}
Result RecordingSessionState::primed(const QString& take)
{
 if(take!=m_status.takeId)return result(false,"stale_take");
 if(m_status.phase!=Phase::Priming || !m_status.armed)return result(false,"invalid_state");
 m_status.phase=Phase::Preroll;return result(true,"preroll");
}
Result RecordingSessionState::captureStarted(const QString& take)
{
 if(take!=m_status.takeId)return result(false,"stale_take");
 if(m_status.phase!=Phase::Preroll || !m_status.armed)return result(false,"invalid_state");
 m_captured=true;m_status.phase=Phase::Recording;return result(true,"recording");
}
Result RecordingSessionState::finish(const QString& take,TakeOutcome outcome)
{
 if(take!=m_status.takeId)return result(false,"stale_take");
 if(m_status.phase!=Phase::Finalizing || (outcome!=TakeOutcome::Complete && outcome!=TakeOutcome::SavedUnattached && outcome!=TakeOutcome::Incomplete)
  || (m_status.incomplete && outcome==TakeOutcome::Complete))return result(false,"invalid_state");
 m_status.incomplete=m_status.incomplete || outcome==TakeOutcome::Incomplete;
 return terminal(outcome,outcome==TakeOutcome::Complete?"take_complete":outcome==TakeOutcome::SavedUnattached?"take_saved_unattached":"take_incomplete");
}
}
