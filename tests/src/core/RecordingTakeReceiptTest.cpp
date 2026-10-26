#include "RecordingTakeReceipt.h"
#include "RecordingInputController.h"
#include "RecordingWriter.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>
using namespace lmms::recording;
namespace {
struct Storage {QByteArray bytes;int calls=0;bool fail=false;};
class Sink final : public RecordingReceiptSink
{
public:
 explicit Sink(Storage& s):storage(s){}
 bool appendAndFlush(const QByteArray& b) noexcept override
 {++storage.calls;if(storage.fail){storage.bytes+=b.left(7);return false;}storage.bytes+=b;return true;}
 Storage& storage;
};
TakeDescriptor take()
{TakeDescriptor d;d.projectId="00000000-0000-4000-8000-000000000001";d.takeId="00000000-0000-4000-8000-000000000002";
 d.label="PRIVATE_PATH_label";d.epoch={1,1};d.projectGeneration=1;d.sampleRate=48000;d.maxRawFrames=5760000;return d;}
RecordingControllerSnapshot input()
{RecordingControllerSnapshot s;s.input.epoch={1,1};s.input.receivedFrames=4096;s.input.acceptedFrames=4096;s.firstCallbackSeen=true;
 auto& d=s.diagnostic;d.outcomes.fill(InputDiagnosticOutcome::Succeeded);d.hostApiName="Windows WASAPI";d.deviceDescription="Local headset";
 d.libraryVersion="PortAudio synthetic v19";d.wasapiShared=true;d.requestedRate=48000;d.actualSampleRate=48000;
 d.streamInfoAvailable=true;d.actualInputLatency=.01;d.inputChannels=2;d.selectedChannel=1;d.maxInputChannels=2;d.maxCallbackFrames=4096;
 d.hostErrorDescription="PRIVATE_PATH/opaque-endpoint-id";return s;}
WriterSnapshot writer()
{WriterSnapshot s;s.state=WriterState::Sealed;s.inputAcceptedFrames=4096;s.acceptedFrames=4096;s.removedFrames=4096;
 s.writtenFrames=4096;s.durableFrames=4096;s.inputClosed=true;s.audioSealed=true;s.journalSealed=true;s.sourceSha256=QByteArray(32,'a');return s;}
}
class RecordingTakeReceiptTest : public QObject
{
 Q_OBJECT
private slots:
 void rawApiResultsAreMeasuredAndValidationResultsRemainUnknown()
 {
  Storage store;auto s=input();const auto stop=std::size_t(InputDiagnosticStage::StopStream),term=std::size_t(InputDiagnosticStage::Terminate);
  s.diagnostic.portAudioResultsKnown[stop]=true;s.diagnostic.portAudioResults[stop]=-9983;
  s.diagnostic.portAudioResultsKnown[term]=true;s.diagnostic.portAudioResults[term]=0;
  RecordingTakeReceipt r(take(),std::make_unique<Sink>(store));QVERIFY(r.append(s,writer(),true));
  const auto api=QJsonDocument::fromJson(store.bytes).object()["api_results"].toObject();
  QCOMPARE(api["stop_stream"].toInt(),-9983);QCOMPARE(api["terminate"].toInt(-1),0);QVERIFY(api["validate_channels"].isNull());
 }
 void intentionalLimitPrefixReconcilesWithoutBeingReportedAsDataLoss()
 {
  Storage store;auto s=input();auto w=writer();s.input.receivedFrames=5763072;s.input.acceptedFrames=5760000;
  s.input.intentionallyUnrecordedFrames=3072;s.input.stopReason=InputStopReason::DurationLimit;
  w.writtenFrames=5760000;w.acceptedFrames=5760000;w.inputAcceptedFrames=5760000;w.removedFrames=5760000;w.durableFrames=5760000;
  RecordingTakeReceipt r(take(),std::make_unique<Sink>(store));QVERIFY(r.append(s,w,true));
  const auto obj=QJsonDocument::fromJson(store.bytes).object();QVERIFY(obj["audio_complete"].toBool());
  QCOMPARE(obj["counts"].toObject()["intentionally_unrecorded"].toString(),QString("3072"));
 }
 void closedReceiptBindsSafeProfileLifecycleCountsAndSealedDigest()
 {Storage store;RecordingTakeReceipt r(take(),std::make_unique<Sink>(store));QVERIFY(r.append(input(),writer(),true));
  const auto obj=QJsonDocument::fromJson(store.bytes).object();QCOMPARE(obj["event"].toString(),QString("closed"));
  QCOMPARE(obj["take_id"].toString(),take().takeId);QCOMPARE(obj["wav_sha256"].toString(),QString::fromLatin1(QByteArray(32,'a').toHex()));
  const auto profile=obj["profile"].toObject();QCOMPARE(profile["actual_rate_hz"].toDouble(),48000.);
  QCOMPARE(profile["input_channels"].toInt(),2);QCOMPARE(profile["mono_channel_index"].toInt(),1);
  QCOMPARE(profile["device"].toString(),QString("Local headset"));QCOMPARE(profile["portaudio_version"].toString(),QString("PortAudio synthetic v19"));
  QCOMPARE(obj["lifecycle"].toObject()["terminate"].toString(),QString("succeeded"));
  QCOMPARE(obj["counts"].toObject()["written"].toString(),QString("4096"));QVERIFY(obj["audio_complete"].toBool());
  QVERIFY(!store.bytes.contains("PRIVATE_PATH"));QVERIFY(!store.bytes.contains("opaque-endpoint-id"));
  auto checked=obj;const auto checksum=checked.take("record_sha256").toString();
  QCOMPARE(checksum,QString(QCryptographicHash::hash(QJsonDocument(checked).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex()));
 }
 void unclosedOrUnprovenStateCannotClaimLifecycleClosure()
 {Storage store;auto s=input();s.state=RecordingControllerState::FailedRetained;s.driverInitialized=true;
  RecordingTakeReceipt r(take(),std::make_unique<Sink>(store));QVERIFY(r.append(s,writer(),true));
  const auto obj=QJsonDocument::fromJson(store.bytes).object();QCOMPARE(obj["event"].toString(),QString("retained"));
  QVERIFY(!obj["lifecycle_closed"].toBool());}
 void fieldsAndRepeatedFailureHistoryAreBounded()
 {Storage store;auto s=input();s.diagnostic.deviceDescription=QString(5000,'d');s.diagnostic.libraryVersion=QString(5000,'v');
  RecordingTakeReceipt r(take(),std::make_unique<Sink>(store));QVERIFY(r.append(s,writer(),false));
  QVERIFY(r.append(s,writer(),false));QCOMPARE(store.calls,1);
  const auto obj=QJsonDocument::fromJson(store.bytes).object();QVERIFY(obj["profile"].toObject()["device"].toString().size()<=96);
  for(int i=1;i<8;++i){s.errorCode=QString("failure_%1").arg(i);QVERIFY(r.append(s,writer(),false));}
  s.errorCode="one-too-many";QVERIFY(!r.append(s,writer(),false));QCOMPARE(r.code(),QString("receipt_resource_limit"));
  QCOMPARE(store.calls,8);QVERIFY(store.bytes.size()<=RecordingTakeReceipt::MaximumFileBytes);
  for(const auto& line:store.bytes.split('\n'))if(!line.isEmpty())QVERIFY(line.size()+1<=RecordingTakeReceipt::MaximumRecordBytes);
 }
 void failedWriteIsVisibleAndNeverAppendsAfterATornTail()
 {Storage store;store.fail=true;RecordingTakeReceipt r(take(),std::make_unique<Sink>(store));QVERIFY(!r.append(input(),writer(),true));
  QCOMPARE(r.code(),QString("receipt_write_failed"));store.fail=false;QVERIFY(!r.append(input(),writer(),true));QCOMPARE(store.calls,1);}
 void sessionFactsStayWithinExistingReceiptBounds()
 {
  Storage store;auto s=input();auto d=take();d.session=TakeSessionFacts{2147483647,2147483000,18446744073709551615ULL,5760000,999,32,32,true,true};
  s.diagnostic.hostApiName=QString(64,'h');s.diagnostic.deviceDescription=QString(96,'d');s.diagnostic.libraryVersion=QString(96,'v');
  s.diagnostic.description=QString(160,'e');s.errorCode=QString(96,'e');s.diagnostic.hasHostError=true;s.diagnostic.hostErrorCode=2147483647;
  RecordingTakeReceipt r(d,std::make_unique<Sink>(store));QVERIFY(r.append(s,writer(),true));
  QVERIFY(store.bytes.size()<=RecordingTakeReceipt::MaximumRecordBytes);const auto obj=QJsonDocument::fromJson(store.bytes).object();
  QCOMPARE(obj["session"].toObject()["raw_ready_observed_ms"].toString(),QString("18446744073709551615"));
  QVERIFY(!store.bytes.contains("PRIVATE_PATH"));
 }
 void invalidTakeIdentityCannotCreateDiagnosticEvidence()
 {Storage store;auto d=take();d.takeId="../outside";RecordingTakeReceipt r(d,std::make_unique<Sink>(store));
  QVERIFY(!r.append(input(),writer(),true));QCOMPARE(store.calls,0);}
};
QTEST_GUILESS_MAIN(RecordingTakeReceiptTest)
#include "RecordingTakeReceiptTest.moc"
