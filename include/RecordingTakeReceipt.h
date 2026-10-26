#ifndef LMMS_RECORDING_TAKE_RECEIPT_H
#define LMMS_RECORDING_TAKE_RECEIPT_H
#include "RecordingTakeTypes.h"
#include <QByteArray>
#include <memory>
namespace lmms::recording {
struct RecordingControllerSnapshot;
struct WriterSnapshot;
enum class RecordingStopAction {None,Stop,Disarm,ProjectChange,BackendChange,Shutdown};
class RecordingReceiptSink
{
public:
 virtual ~RecordingReceiptSink()=default;
 virtual bool appendAndFlush(const QByteArray&) noexcept=0;
};
class RecordingTakeReceipt
{
public:
 static constexpr int MaximumRecordBytes=4096,MaximumRecords=8,MaximumFileBytes=32768;
 RecordingTakeReceipt(TakeDescriptor,std::unique_ptr<RecordingReceiptSink>);
 bool append(const RecordingControllerSnapshot&,const WriterSnapshot&,bool lifecycleClosed,
  RecordingStopAction action=RecordingStopAction::None) noexcept;
 QString code() const {return m_code;}
 int recordCount() const noexcept{return m_records;}
private:
 TakeDescriptor m_take;
 std::unique_ptr<RecordingReceiptSink> m_sink;
 QByteArray m_previous;
 QString m_code;
 int m_records=0,m_bytes=0;
 bool m_poisoned=false;
};
}
#endif
