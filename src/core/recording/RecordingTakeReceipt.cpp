#include "RecordingTakeReceipt.h"
#include "RecordingInputController.h"
#include "RecordingWriter.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <cmath>
#include <utility>
namespace lmms::recording {
RecordingTakeReceipt::RecordingTakeReceipt(TakeDescriptor take,std::unique_ptr<RecordingReceiptSink> sink)
 :m_take(std::move(take)),m_sink(std::move(sink)){}
namespace {
QString bounded(QString text,int limit)
{text=text.left(limit);for(auto& c:text)if(c.category()==QChar::Other_Control)c=QChar(' ');return text;}
bool validId(const QString& text)
{const QUuid id(text);return !id.isNull() && id.toString(QUuid::WithoutBraces)==text;}
QString outcomeName(InputDiagnosticOutcome value)
{return value==InputDiagnosticOutcome::Succeeded?QStringLiteral("succeeded"):value==InputDiagnosticOutcome::Failed?QStringLiteral("failed"):QStringLiteral("not_attempted");}
}
bool RecordingTakeReceipt::append(const RecordingControllerSnapshot& input,const WriterSnapshot& written,bool closed,RecordingStopAction action) noexcept
{
 if(m_poisoned)return false;
 if(!validId(m_take.projectId) || !validId(m_take.takeId) || !m_take.epoch || !m_sink)
 {m_code="receipt_invalid_identity";m_poisoned=true;return false;}
 const auto& d=input.diagnostic;const auto& capture=input.input;
 const bool released=closed && input.state==RecordingControllerState::Closed && !input.driverInitialized && input.callbacksInFlight==0;
 const bool complete=written.state==WriterState::Sealed && written.audioSealed && written.journalSealed
  && written.writtenFrames>0 && written.writtenFrames==capture.acceptedFrames && capture.receivedFrames>=capture.acceptedFrames
  && capture.receivedFrames-capture.acceptedFrames==capture.intentionallyUnrecordedFrames
  && written.writtenFrames<=m_take.maxRawFrames && written.removedFrames==written.writtenFrames && written.pendingFrames==0
  && !capture.applicationDroppedFrames && !capture.invalidSamples && capture.fault==InputFault::None
  && capture.hardwareLossCountKnown && !capture.hardwareLostFrames && written.fault==WriterFault::None && written.sourceSha256.size()==32;
 const bool verified=released && d.failedStage==InputDiagnosticStage::None
  && d.outcome(InputDiagnosticStage::OpenStream)==InputDiagnosticOutcome::Succeeded
  && d.outcome(InputDiagnosticStage::StartStream)==InputDiagnosticOutcome::Succeeded
  && d.outcome(InputDiagnosticStage::FirstCallback)==InputDiagnosticOutcome::Succeeded
  && d.outcome(InputDiagnosticStage::StopStream)==InputDiagnosticOutcome::Succeeded
  && d.outcome(InputDiagnosticStage::CloseStream)==InputDiagnosticOutcome::Succeeded
  && d.outcome(InputDiagnosticStage::Terminate)==InputDiagnosticOutcome::Succeeded;
 QJsonObject stages,apiResults;
 for(std::size_t i=0;i<d.outcomes.size();++i)
 {
  const auto name=QString::fromLatin1(recordingInputStageName(InputDiagnosticStage(i)));stages[name]=outcomeName(d.outcomes[i]);
  apiResults[name]=d.portAudioResultsKnown[i]?QJsonValue(d.portAudioResults[i]):QJsonValue{};
 }
 QJsonObject profile{{"host_api",bounded(d.hostApiName,64)},{"device",bounded(d.deviceDescription,96)},
  {"portaudio_version",bounded(d.libraryVersion,96)},{"wasapi_shared",d.wasapiShared},
  {"requested_rate_hz",int(d.requestedRate)},{"input_channels",int(d.inputChannels)},{"mono_channel_index",int(d.selectedChannel)},
  {"max_input_channels",int(d.maxInputChannels)},{"max_callback_frames",int(d.maxCallbackFrames)},
  {"stream_info_known",d.streamInfoAvailable},{"sample_format","float32_interleaved"},{"wav_format","mono_pcm16"}};
 profile["actual_rate_hz"]=d.streamInfoAvailable && std::isfinite(d.actualSampleRate)?QJsonValue(d.actualSampleRate):QJsonValue{};
 profile["input_latency_seconds"]=d.streamInfoAvailable && std::isfinite(d.actualInputLatency)?QJsonValue(d.actualInputLatency):QJsonValue{};
 profile["reported_default_rate_hz"]=std::isfinite(d.defaultSampleRate)?QJsonValue(d.defaultSampleRate):QJsonValue{};
 QJsonObject counts{{"received",QString::number(capture.receivedFrames)},{"accepted",QString::number(capture.acceptedFrames)},
  {"written",QString::number(written.writtenFrames)},{"removed",QString::number(written.removedFrames)},
  {"durable",QString::number(written.durableFrames)},{"dropped",QString::number(capture.applicationDroppedFrames)},
  {"intentionally_unrecorded",QString::number(capture.intentionallyUnrecordedFrames)},
  {"clipped",QString::number(written.clippedSamples)},{"invalid",QString::number(capture.invalidSamples)},
  {"hardware_lost",QString::number(capture.hardwareLostFrames)},{"hardware_loss_known",capture.hardwareLossCountKnown},
  {"observations_dropped",QString::number(capture.observationsDropped)},{"input_fault",int(capture.fault)},{"writer_fault",int(written.fault)},{"timestamp_quality",int(capture.timestampQuality)},
  {"stop_reason",int(capture.stopReason)}};
 const auto& s=input.observations;
 const auto observation=[](const InputObservation& o,bool present){
  return QJsonObject{{"first_frame",QString::number(o.receivedFirstFrame)},{"frames",int(o.frames)},
   {"adc_seconds",present && std::isfinite(o.adcFirstSeconds)?QJsonValue(o.adcFirstSeconds):QJsonValue{}},
   {"current_seconds",present && std::isfinite(o.currentFirstSeconds)?QJsonValue(o.currentFirstSeconds):QJsonValue{}}};
 };
 QJsonObject timing{{"project_generation",QString::number(s.epoch.projectGeneration)},
  {"session_revision",QString::number(s.epoch.sessionRevision)},{"blocks",QString::number(s.observations)},
  {"received_frames",QString::number(s.receivedFrames)},{"frame_discontinuities",QString::number(s.frameDiscontinuities)},
  {"unavailable_timestamps",QString::number(s.unavailableTimestamps)},{"clock_regressions",QString::number(s.clockRegressions)},
  {"adc_interval_discontinuities",QString::number(s.adcIntervalDiscontinuities)},{"host_status_blocks",QString::number(s.hostStatusBlocks)},
  {"host_flags",int(s.hostFlags)},{"quality",int(s.quality)},{"first",observation(s.first,s.observations!=0)},
  {"last",observation(s.last,s.observations!=0)},{"calibrated",false},{"drift_measured",false}};
 QJsonObject record{{"schema",1},{"project_id",m_take.projectId},{"take_id",m_take.takeId},
  {"event",released?"closed":"retained"},{"lifecycle_closed",released},{"lifecycle_verified",verified},
  {"audio_complete",complete},{"wav_sha256",QString::fromLatin1(written.sourceSha256.toHex())},
  {"profile",profile},{"counts",counts},{"observations",timing},{"lifecycle",stages},{"api_results",apiResults},{"first_callback_seen",input.firstCallbackSeen},
  {"driver_initialized",input.driverInitialized},{"callbacks_in_flight",int(input.callbacksInFlight)},
  {"failed_stage",QString::fromLatin1(recordingInputStageName(d.failedStage))},
  {"error_code",bounded(input.errorCode,96)},{"error_text",bounded(d.description,160)}};
 switch(action)
 {
  case RecordingStopAction::Stop:record["stop_action"]="stop";break;
  case RecordingStopAction::Disarm:record["stop_action"]="disarm";break;
  case RecordingStopAction::ProjectChange:record["stop_action"]="project_change";break;
  case RecordingStopAction::BackendChange:record["stop_action"]="backend_change";break;
  case RecordingStopAction::Shutdown:record["stop_action"]="shutdown";break;
  default:record["stop_action"]="none";break;
 }
 record["portaudio_error"]=d.hasPortAudioError?QJsonValue(d.portAudioError):QJsonValue{};
 // Raw host error text, opaque endpoint IDs, paths, labels and samples are never persisted.
 record["host_error_known"]=d.hasHostError;
 if(d.hasHostError){record["host_error_code"]=QString::number(d.hostErrorCode);record["host_api_type"]=d.hostApiType;}
 const auto key=QJsonDocument(record).toJson(QJsonDocument::Compact);
 if(key==m_previous){m_code="receipt_saved";return true;}
 if(m_records>=MaximumRecords){m_code="receipt_resource_limit";m_poisoned=true;return false;}
 record["sequence"]=m_records+1;
 record["record_sha256"]=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(record).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
 auto bytes=QJsonDocument(record).toJson(QJsonDocument::Compact);bytes+='\n';
 if(bytes.size()>MaximumRecordBytes || bytes.size()>MaximumFileBytes-m_bytes)
 {m_code="receipt_resource_limit";m_poisoned=true;return false;}
 if(!m_sink->appendAndFlush(bytes)){m_code="receipt_write_failed";m_poisoned=true;return false;}
 m_previous=key;++m_records;m_bytes+=bytes.size();m_code="receipt_saved";return true;
}
}
