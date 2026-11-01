#include "RecordingPlacement.h"
#include "AudioDevice.h"
#include "Engine.h"
#include "Song.h"
#include <QtTest>
#include <limits>
using namespace lmms::recording;
// Only this test-local stand-in of the future local view can mint an approval.
// No production factory or remote constructor is introduced.
namespace lmms::recording {
class RecordingRecorderView
{
public:
 static ManualPlacementApproval approve(const QString& take,quint64 generation,quint64 revision,qint64 sourceIn,qint64 target)
 {ManualPlacementApproval a;a.takeId=take;a.expectedProjectGeneration=generation;a.placementRevision=revision;a.sourceInTicks=sourceIn;a.targetTick=target;return a;}
};
}
namespace {
RawTakeFacts source()
 {return {"00000000-0000-4000-8000-000000000001","00000000-0000-4000-8000-000000000002",48000,96137,{1,2},QByteArray(32,'x')};}
NativeGeometrySnapshot geometry(const RawTakeFacts& source,int tempo=125)
 {
  NativeGeometrySnapshot g;g.current.available=true;g.current.projectId=source.projectId;g.current.projectGeneration=3;g.current.contextRevision=7;g.current.tempo=tempo;g.current.sampleRate=source.sampleRate;
  g.reviewed=g.current;g.reviewedSource=source;g.placementRevision=5;
  lmms::Engine::getSong()->setTempo(tempo);g.sourceFramesPerTick=lmms::Engine::framesPerTick(source.sampleRate);return g;
 }
ManualPlacementApproval approved(qint64 q=13,qint64 p=384,quint64 revision=5)
 {return RecordingRecorderView::approve(source().takeId,3,revision,q,p);}
class Transport final : public RecordingTransportControl
 {
 public:RecordingTransportSnapshot world;
 RecordingTransportSnapshot snapshot()const override{return world;}
 RecordingTransportResult seekSong(qint64)override{return {false,"forbidden"};}
 RecordingTransportResult playSong()override{return {false,"forbidden"};}
 RecordingTransportResult stopOwned(RecordingPlaybackOwnership)override{return {false,"forbidden"};}
 };
}
class RecordingPlacementTest : public QObject
{
 Q_OBJECT
private slots:
 void initTestCase(){lmms::Engine::init(true);lmms::Engine::audioEngine()->audioDev()->stopProcessing();}
 void cleanupTestCase(){lmms::Engine::destroy();}
 void manualApprovalRequired()
 {const auto s=source();QCOMPARE(RecordingPlacement::plan(s,{},geometry(s)).code,QString("manual_approval_required"));}
 void negativeNativeOffsetSkipsPrefix()
 {const auto s=source();const auto result=RecordingPlacement::plan(s,approved(),geometry(s));QVERIFY(result.plan);QCOMPARE(result.plan->offsetTicks,qint64(-13));QCOMPARE(result.plan->positionTicks,qint64(384));
  QCOMPARE(result.plan->lengthTicks,qint64(188));QCOMPARE(result.plan->effectiveSourceInFrame,quint64(6240));QCOMPARE(result.plan->nativeEndFrame,quint64(96480));QCOMPARE(result.plan->rawEndFrame,s.rawFrames);QCOMPARE(result.plan->placementRevision,quint64(5));
  QCOMPARE(result.plan->placementQuality,QString("uncalibrated"));QCOMPARE(result.plan->driftQuality,QString("unmeasured"));QVERIFY(!result.plan->sampleAccuracyVerified);}
 void sourceInAtOrAfterEofRefusedBeforeMutation_data()
 {QTest::addColumn<qint64>("q");QTest::newRow("eof")<<qint64(200);QTest::newRow("beyond")<<qint64(201);}
 void sourceInAtOrAfterEofRefusedBeforeMutation()
 {QFETCH(qint64,q);auto s=source();s.rawFrames=96000;const auto g=geometry(s);const auto before=lmms::Engine::getSong()->tracks().size();const auto result=RecordingPlacement::plan(s,approved(q),g);QVERIFY(!result.plan);QCOMPARE(result.code,QString("source_in_at_or_after_eof"));QCOMPARE(lmms::Engine::getSong()->tracks().size(),before);}
 void shortCountinOnlyTakeUnattached()
 {auto s=source();s.rawFrames=6000;const auto result=RecordingPlacement::plan(s,approved(),geometry(s));QCOMPARE(result.code,QString("source_in_at_or_after_eof"));QVERIFY(!result.plan);}
 void fractionalFramesPerTickChecked_data()
 {QTest::addColumn<int>("rate");QTest::addColumn<int>("tempo");for(int r:{44100,48000})for(int t:{100,125,137})QTest::newRow(qPrintable(QString::number(r)+"-"+QString::number(t)))<<r<<t;}
 void fractionalFramesPerTickChecked()
 {QFETCH(int,rate);QFETCH(int,tempo);auto s=source();s.sampleRate=rate;s.rawFrames=quint64(rate)*2+137;const auto g=geometry(s,tempo);const auto result=RecordingPlacement::plan(s,approved(),g);QVERIFY(result.plan);
  const auto& p=*result.plan;QCOMPARE(p.effectiveSourceInFrame,quint64(g.sourceFramesPerTick*13));QVERIFY(p.effectiveSourceInFrame<s.rawFrames);
  QCOMPARE(p.nativeEndFrame,quint64(g.sourceFramesPerTick*float(13+p.lengthTicks)));QVERIFY(p.nativeEndFrame>=s.rawFrames);if(p.lengthTicks>1)QVERIFY(quint64(g.sourceFramesPerTick*float(12+p.lengthTicks))<s.rawFrames);}
 void zeroCountinStillNeedsOriginReview()
 {const auto s=source();const auto g=geometry(s);QVERIFY(!RecordingPlacement::plan(s,{},g).plan);const auto result=RecordingPlacement::plan(s,approved(0),g);QVERIFY(result.plan);QCOMPARE(result.plan->offsetTicks,qint64(0));QCOMPARE(result.plan->effectiveSourceInFrame,quint64(0));}
 void overflowAndStaleRevisionRefused_data()
 {QTest::addColumn<int>("change");for(int i=0;i<16;++i)QTest::newRow(qPrintable(QString::number(i)))<<i;}
 void overflowAndStaleRevisionRefused()
 {QFETCH(int,change);auto s=source();auto g=geometry(s);auto a=approved();
  switch(change){case 0:a=approved(-1);break;case 1:a=approved(13,std::numeric_limits<qint64>::max());break;case 2:a=approved(13,std::numeric_limits<int>::max());break;
   case 3:a=approved(std::numeric_limits<qint64>::max());break;case 4:a=approved(13,384,4);break;case 5:++g.current.projectGeneration;break;case 6:++g.current.tempo;break;
   case 7:++g.current.contextRevision;break;case 8:++s.rawFrames;break;case 9:s.sourceDigest=QByteArray(32,'y');break;case 10:++s.sourceIdentity.index;break;
   case 11:g.current.sampleRate=44100;break;case 12:g.sourceFramesPerTick=std::numeric_limits<float>::infinity();break;case 13:g.sourceFramesPerTick=std::numeric_limits<float>::quiet_NaN();break;
   case 14:s.rawFrames=0;g.reviewedSource=s;break;case 15:s.rawFrames=5760001;g.reviewedSource=s;break;}
  const QStringList codes{"invalid_source_in","invalid_position","tick_overflow","invalid_source_in","stale_placement","stale_project","stale_geometry","stale_geometry","stale_source","stale_source","stale_source","unsupported_rate","invalid_geometry","invalid_geometry","empty_source","source_limit"};
  const auto result=RecordingPlacement::plan(s,a,g);QVERIFY(!result.plan);QCOMPARE(result.code,codes[change]);}
 void transportAndSourceEligibilityRefused_data()
 {QTest::addColumn<int>("change");for(int i=0;i<10;++i)QTest::newRow(qPrintable(QString::number(i)))<<i;}
 void transportAndSourceEligibilityRefused()
 {QFETCH(int,change);auto s=source();auto g=geometry(s);switch(change){case 0:g.current.playing=true;break;case 1:g.current.paused=true;break;case 2:g.current.looping=true;break;case 3:g.current.exporting=true;break;case 4:g.current.constantTempo=false;break;case 5:g.current.stableMeter=false;break;case 6:g.current.available=false;break;case 7:g.reviewedSource.sourceDigest.clear();break;case 8:g.current.ticksPerBar=144;break;case 9:g.current.projectId="00000000-0000-4000-8000-000000000003";break;}
  const QStringList codes{"transport_busy","transport_busy","unsupported_loop","transport_busy","unsupported_transport_automation","unsupported_transport_automation","transport_unavailable","stale_source","stale_geometry","stale_project"};
  const auto result=RecordingPlacement::plan(s,approved(),g);QVERIFY(!result.plan);QCOMPARE(result.code,codes[change]);}
 void geometryCaptureCopiesActualNativeValues()
 {auto s=source();auto g=geometry(s,137);s.sampleRate=lmms::Engine::audioEngine()->outputSampleRate();g.current.sampleRate=s.sampleRate;Transport transport;transport.world=g.current;
  const auto captured=RecordingPlacement::captureGeometry(transport,s,g.current,5);QVERIFY(captured.current.available);QCOMPARE(captured.sourceFramesPerTick,lmms::Engine::framesPerTick(s.sampleRate));QCOMPARE(captured.current.tempo,137);QCOMPARE(captured.placementRevision,quint64(5));}
 void revalidatedPlanRejectsStaleGeometryAndTamperedMetadata()
 {const auto s=source();auto g=geometry(s);const auto original=RecordingPlacement::plan(s,approved(),g);QVERIFY(original.plan);
  const auto valid=RecordingPlacement::revalidatePlan(s,*original.plan,g);QVERIFY(valid.plan);QCOMPARE(valid.plan->offsetTicks,original.plan->offsetTicks);
  auto stale=g;++stale.current.tempo;stale.reviewed=stale.current;lmms::Engine::getSong()->setTempo(stale.current.tempo);stale.sourceFramesPerTick=lmms::Engine::framesPerTick(s.sampleRate);
  const auto changedGeometry=RecordingPlacement::revalidatePlan(s,*original.plan,stale);QVERIFY(!changedGeometry.plan);QCOMPARE(changedGeometry.code,QStringLiteral("placement_review_stale"));
  auto forged=*original.plan;forged.lengthTicks+=1;const auto changedLength=RecordingPlacement::revalidatePlan(s,forged,g);QVERIFY(!changedLength.plan);QCOMPARE(changedLength.code,QStringLiteral("placement_review_stale"));}
 void persistedClipGeometryReconstructsOnlyWhenStillNativeEquivalent()
 {const auto s=source();const auto g=geometry(s);const auto original=RecordingPlacement::plan(s,approved(),g);QVERIFY(original.plan);
  const auto restored=RecordingPlacement::restorePersistedPlan(s,original.plan->positionTicks,original.plan->offsetTicks,original.plan->lengthTicks,original.plan->placementRevision,g);
  QVERIFY(restored.plan);QCOMPARE(restored.plan->positionTicks,original.plan->positionTicks);QCOMPARE(restored.plan->offsetTicks,original.plan->offsetTicks);
  auto changed=g;++changed.current.tempo;changed.reviewed=changed.current;lmms::Engine::getSong()->setTempo(changed.current.tempo);changed.sourceFramesPerTick=lmms::Engine::framesPerTick(s.sampleRate);
  const auto stale=RecordingPlacement::restorePersistedPlan(s,original.plan->positionTicks,original.plan->offsetTicks,original.plan->lengthTicks,original.plan->placementRevision,changed);
  QVERIFY(!stale.plan);QCOMPARE(stale.code,QStringLiteral("persisted_geometry_mismatch"));}
};
QTEST_GUILESS_MAIN(RecordingPlacementTest)
#include "RecordingPlacementTest.moc"
