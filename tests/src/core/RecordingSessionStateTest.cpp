#include "RecordingSessionState.h"
#include <QtTest>
#include <QUuid>
#include <memory>
using namespace lmms::recording;
class RecordingSessionStateTest : public QObject
{
 Q_OBJECT
 LocalArmContext context{QStringLiteral("process-one"),QStringLiteral("project-one"),QStringLiteral("device-one"),QStringLiteral("root-one"),1};
 static QString requestId(){return QUuid::createUuid().toString(QUuid::WithoutBraces);}
 PreparedTake metadata() const
 {
  PreparedTake p;p.context=context;p.startTick=3840;p.countInBars=2;p.label="Hook";p.transportHash=QString(64,'a');p.sampleRate=48000;return p;
 }
 QString begin(RecordingSessionState& state)
 {
  if(!state.arm(context).ok)return {};
  const auto prepared=state.prepare(metadata());if(!prepared.ok)return {};
  const auto started=state.start(prepared.status.preparedId,requestId(),1);
  return started.ok?started.status.takeId:QString{};
 }
 Result stopAndQuiesce(RecordingSessionState& state,const QString& take)
 {
  const auto stopped=state.stop(take);
  return state.status().phase==Phase::Quiescing?state.quiesced(take):stopped;
 }
private slots:
 void deliberateArmAndFullLifecycle()
 {
  RecordingSessionState s(context);QCOMPARE(s.status().phase,Phase::Disarmed);QVERIFY(!s.status().armed);
  const auto take=begin(s);QVERIFY(!take.isEmpty());QCOMPARE(s.status().phase,Phase::Priming);
  QVERIFY(s.primed(take).ok);QCOMPARE(s.status().phase,Phase::Preroll);
  QVERIFY(s.captureStarted(take).ok);QCOMPARE(s.status().phase,Phase::Recording);
  QVERIFY(s.stop(take).ok);QCOMPARE(s.status().phase,Phase::Quiescing);
  QVERIFY(s.quiesced(take).ok);QCOMPARE(s.status().phase,Phase::Finalizing);
  const auto result=s.finish(take,TakeOutcome::Complete);QVERIFY(result.ok);QCOMPARE(result.code,QString("take_complete"));QCOMPARE(result.status.phase,Phase::Armed);
  QVERIFY(result.status.armed);QCOMPARE(result.status.outcome,TakeOutcome::Complete);
 }
 void preparationWhileDisarmedIsInert()
 {
  RecordingSessionState s(context);const auto p=s.prepare(metadata());QVERIFY(p.ok);QVERIFY(!p.status.armed);
  QCOMPARE(s.start(p.status.preparedId,requestId(),1).code,QString("not_armed"));
  QVERIFY(s.arm(context).ok);QVERIFY(s.start(p.status.preparedId,requestId(),1).ok);
 }
 void prerollAndPrimingStopNeverFinalize()
 {
  for(bool afterPrime:{false,true}){RecordingSessionState s(context);const auto take=begin(s);QVERIFY(!take.isEmpty());if(afterPrime)QVERIFY(s.primed(take).ok);
   const auto stop=stopAndQuiesce(s,take);QVERIFY(stop.ok);QCOMPARE(stop.code,QString("no_take_created"));QCOMPARE(stop.status.phase,Phase::Armed);QCOMPARE(s.stop(take).code,stop.code);
   QCOMPARE(s.finish(take,TakeOutcome::Complete).code,QString("invalid_state"));}
 }
 void preparedStopHasStableCandidateIdentity()
 {
  RecordingSessionState s(context);QVERIFY(s.arm(context).ok);const auto p=s.prepare(metadata());QVERIFY(p.ok);
  QVERIFY(!p.status.takeId.isEmpty());QCOMPARE(s.stop(p.status.takeId).code,QString("no_take_created"));
  QCOMPARE(s.start(p.status.preparedId,requestId(),1).code,QString("stale_preparation"));
 }
 void duplicateStartReconcilesWithoutAnotherTake()
 {
  RecordingSessionState s(context);QVERIFY(s.arm(context).ok);const auto p=s.prepare(metadata());QVERIFY(p.ok);const auto request=requestId();
  const auto first=s.start(p.status.preparedId,request,1);QVERIFY(first.ok);
  const auto duplicate=s.start(p.status.preparedId,request,1);QVERIFY(duplicate.ok);QCOMPARE(duplicate.status.takeId,first.status.takeId);
  QCOMPARE(s.start(p.status.preparedId,requestId(),1).code,QString("recording_busy"));
  QCOMPARE(s.start(requestId(),request,1).code,QString("conflicting_start"));
  QVERIFY(s.primed(first.status.takeId).ok);QVERIFY(s.captureStarted(first.status.takeId).ok);QVERIFY(stopAndQuiesce(s,first.status.takeId).ok);
  QVERIFY(s.finish(first.status.takeId,TakeOutcome::Complete).ok);
  const auto terminal=s.start(p.status.preparedId,request,1);QVERIFY(terminal.ok);QCOMPARE(terminal.code,QString("take_complete"));QCOMPARE(terminal.status.takeId,first.status.takeId);
  QCOMPARE(s.status().phase,Phase::Armed);
 }
 void finalizingExcludesStartsAndStopIsIdempotent()
 {
  RecordingSessionState s(context);const auto take=begin(s);QVERIFY(!take.isEmpty());QVERIFY(s.primed(take).ok);QVERIFY(s.captureStarted(take).ok);
  QVERIFY(stopAndQuiesce(s,take).ok);const auto snapshot=s.status();QCOMPARE(s.stop(take).status.phase,snapshot.phase);
  QCOMPARE(s.prepare(metadata()).code,QString("recording_busy"));QCOMPARE(s.start(snapshot.preparedId,requestId(),1).code,QString("recording_busy"));
  QVERIFY(s.finish(take,TakeOutcome::SavedUnattached).ok);QCOMPARE(s.stop(take).code,QString("take_saved_unattached"));
 }
 void disarmRevokesAndNeverAutoRearms()
 {
  RecordingSessionState s(context);const auto take=begin(s);QVERIFY(!take.isEmpty());QVERIFY(s.primed(take).ok);QVERIFY(s.captureStarted(take).ok);
  QVERIFY(s.disarm().ok);QVERIFY(!s.status().armed);QCOMPARE(s.status().phase,Phase::Quiescing);
  QVERIFY(s.quiesced(take).ok);QCOMPARE(s.status().phase,Phase::Finalizing);
  QVERIFY(s.finish(take,TakeOutcome::Incomplete).ok);QCOMPARE(s.status().phase,Phase::Disarmed);
  const auto p=s.prepare(metadata());QVERIFY(p.ok);QCOMPARE(s.start(p.status.preparedId,requestId(),1).code,QString("not_armed"));
 }
 void everyLeaseIdentityChangeRevokesBeforeFutureStart()
 {
  for(int field=0;field<5;++field){RecordingSessionState s(context);const auto take=begin(s);QVERIFY(!take.isEmpty());auto changed=context;
   if(field==0)changed.processId="new-process";
   if(field==1)changed.projectId="new-project";
   if(field==2)changed.deviceId="new-device";
   if(field==3)changed.libraryRootId="new-root";
   if(field==4)changed.projectGeneration=2;
   s.invalidate(changed);QVERIFY(!s.status().armed);QCOMPARE(s.status().phase,Phase::Quiescing);
   QVERIFY(s.quiesced(take).ok);QCOMPARE(s.status().phase,Phase::Disarmed);
   QCOMPARE(s.arm(context).code,QString("arm_expired"));}
 }
 void invalidationDuringCaptureCannotReportComplete()
 {
  RecordingSessionState s(context);const auto take=begin(s);QVERIFY(!take.isEmpty());QVERIFY(s.primed(take).ok);QVERIFY(s.captureStarted(take).ok);
  auto changed=context;changed.projectGeneration=2;s.invalidate(changed);
  QCOMPARE(s.status().phase,Phase::Quiescing);QVERIFY(s.status().incomplete);QVERIFY(s.quiesced(take).ok);QCOMPARE(s.finish(take,TakeOutcome::Complete).code,QString("invalid_state"));
  QVERIFY(s.finish(take,TakeOutcome::Incomplete).ok);QCOMPARE(s.status().phase,Phase::Disarmed);
 }
 void staleOrMalformedRequestsDoNotChangeState()
 {
  RecordingSessionState s(context);QVERIFY(s.arm(context).ok);const auto p=s.prepare(metadata());QVERIFY(p.ok);
  QCOMPARE(s.start(p.status.preparedId,requestId(),2).code,QString("stale_project"));QCOMPARE(s.start(requestId(),requestId(),1).code,QString("stale_preparation"));
  QCOMPARE(s.start(p.status.preparedId,"bad/request",1).code,QString("invalid_request"));QCOMPARE(s.stop(requestId()).code,QString("stale_take"));
  QCOMPARE(s.status().phase,Phase::Prepared);
  auto bad=metadata();bad.countInBars=3;QVERIFY(!s.prepare(bad).ok);bad=metadata();bad.startTick=-1;QVERIFY(!s.prepare(bad).ok);
  bad=metadata();bad.label=QString(257,'x');QVERIFY(!s.prepare(bad).ok);bad=metadata();bad.transportHash="bad";QVERIFY(!s.prepare(bad).ok);
  bad=metadata();bad.sampleRate=96000;QVERIFY(!s.prepare(bad).ok);QCOMPARE(s.status().preparedId,p.status.preparedId);
 }
 void staleStopCannotStopNewTake()
 {
  RecordingSessionState s(context);const auto old=begin(s);QVERIFY(!old.isEmpty());QVERIFY(stopAndQuiesce(s,old).ok);
  const auto second=begin(s);QVERIFY(!second.isEmpty());QVERIFY(second!=old);
  QCOMPARE(s.stop(old).code,QString("no_take_created"));QCOMPARE(s.status().takeId,second);QCOMPARE(s.status().phase,Phase::Priming);
 }
 void ownerDestructionLeavesOnlyCopiedInertStatus()
 {
  Status copied;{auto s=std::make_unique<RecordingSessionState>(context);QVERIFY(s->arm(context).ok);copied=s->status();}
  QVERIFY(copied.armed);RecordingSessionState replacement(context);QVERIFY(!replacement.status().armed);QCOMPARE(replacement.status().phase,Phase::Disarmed);
 }
 void boundedRequestLedgerDoesNotEvictReplaySafety()
 {
  RecordingSessionState s(context);QVERIFY(s.arm(context).ok);
  for(int i=0;i<64;++i){const auto p=s.prepare(metadata());QVERIFY(p.ok);const auto start=s.start(p.status.preparedId,requestId(),1);QVERIFY(start.ok);QVERIFY(stopAndQuiesce(s,start.status.takeId).ok);}
  const auto p=s.prepare(metadata());QVERIFY(p.ok);QCOMPARE(s.start(p.status.preparedId,requestId(),1).code,QString("resource_limit"));
 }
 void primingFailureDoesNotFinalizeOrReplayStart()
 {
  RecordingSessionState s(context);QVERIFY(s.arm(context).ok);const auto p=s.prepare(metadata());QVERIFY(p.ok);const auto request=requestId();
  const auto first=s.start(p.status.preparedId,request,1);QVERIFY(first.ok);
  const auto failed=s.failPriming(first.status.takeId);QVERIFY(!failed.ok);QCOMPARE(failed.code,QString("render_unavailable"));
  QCOMPARE(failed.status.phase,Phase::Quiescing);QVERIFY(!s.quiesced(first.status.takeId).ok);
  QCOMPARE(s.status().outcome,TakeOutcome::NoTakeCreated);QCOMPARE(s.status().phase,Phase::Armed);
  QCOMPARE(s.start(p.status.preparedId,request,1).code,QString("render_unavailable"));
  QCOMPARE(s.status().phase,Phase::Armed);
 }
 void terminalRepliesNeverResurrectRevokedArm()
 {
  RecordingSessionState s(context);QVERIFY(s.arm(context).ok);const auto p=s.prepare(metadata());QVERIFY(p.ok);const auto request=requestId();
  const auto first=s.start(p.status.preparedId,request,1);QVERIFY(first.ok);QVERIFY(stopAndQuiesce(s,first.status.takeId).ok);QVERIFY(s.disarm().ok);
  QVERIFY(!s.stop(first.status.takeId).status.armed);QVERIFY(!s.start(p.status.preparedId,request,1).status.armed);QVERIFY(!s.status().armed);
 }
 void repeatedNativeEventsCannotAdvanceOrFinishTwice()
 {
  RecordingSessionState s(context);const auto take=begin(s);QVERIFY(!take.isEmpty());QVERIFY(s.primed(take).ok);
  QCOMPARE(s.primed(take).code,QString("invalid_state"));QVERIFY(s.captureStarted(take).ok);QCOMPARE(s.captureStarted(take).code,QString("invalid_state"));
  QVERIFY(stopAndQuiesce(s,take).ok);QVERIFY(s.finish(take,TakeOutcome::Incomplete).ok);QCOMPARE(s.finish(take,TakeOutcome::Complete).code,QString("invalid_state"));
  QCOMPARE(s.stop(take).code,QString("take_incomplete"));
 }
 void quiescingRetainsExclusiveChargeUntilAcknowledged()
 {
  RecordingSessionState s(context);const auto take=begin(s);QVERIFY(!take.isEmpty());QVERIFY(s.stop(take).ok);
  QCOMPARE(s.status().phase,Phase::Quiescing);QCOMPARE(s.prepare(metadata()).code,QString("recording_busy"));
  QCOMPARE(s.start(s.status().preparedId,requestId(),1).code,QString("recording_busy"));
  QCOMPARE(s.finish(take,TakeOutcome::Complete).code,QString("invalid_state"));QVERIFY(s.quiesced(take).ok);
  QCOMPARE(s.status().phase,Phase::Armed);QCOMPARE(s.quiesced(take).code,QString("invalid_state"));
 }
};
QTEST_APPLESS_MAIN(RecordingSessionStateTest)
#include "RecordingSessionStateTest.moc"
