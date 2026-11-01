#include "RecordingSessionManager.h"
#include "Engine.h"
#include "AudioEngine.h"
#include "AudioDevice.h"
#include "Song.h"
#include <QtTest>
using namespace lmms;
using namespace lmms::recording;
class RecordingTransportControlTest : public QObject
{
 Q_OBJECT
private slots:
 void initTestCase(){Engine::init(true);Engine::audioEngine()->audioDev()->stopProcessing();}
 void cleanupTestCase(){Engine::destroy();}
 void init(){Engine::getSong()->stop();Engine::getSong()->clearProject();Engine::getSong()->setTempo(120);Engine::getSong()->metronome().setActive(false);}
 void nativeSnapshotUsesActualMeterAndRevisions()
 {
  auto* song=Engine::getSong();song->getTimeSigModel().setNumerator(3);song->getTimeSigModel().setDenominator(4);
  auto control=createNativeRecordingTransport(*song);const auto first=control->snapshot();QVERIFY(first.available);QCOMPARE(first.ticksPerBar,144);QCOMPARE(first.numerator,3);
  song->setPlayPos(321,Song::PlayMode::Song);const auto seek=control->snapshot();QVERIFY(seek.seekRevision>first.seekRevision);QCOMPARE(seek.songTick,qint64(321));
  song->setTempo(137);const auto tempo=control->snapshot();QVERIFY(tempo.contextRevision>seek.contextRevision);QCOMPARE(tempo.tempo,137);
 }
 void normalSongStartStopDoesNotClaimADeviceOrRenderOwner()
 {
  auto* song=Engine::getSong();auto control=createNativeRecordingTransport(*song);QVERIFY(control->seekSong(576).ok);
  const auto started=control->playSong();QVERIFY(started.ok);QVERIFY(song->isPlaying());QCOMPARE(song->playMode(),Song::PlayMode::Song);
  QVERIFY(control->stopOwned(started.ownership).ok);QVERIFY(song->isStopped());
  QVERIFY(!Engine::recordingServiceHost()->snapshot().input.driverInitialized);
 }
 void ownershipDoesNotStopSubsequentManualPlayback()
 {
  auto* song=Engine::getSong();auto control=createNativeRecordingTransport(*song);QVERIFY(control->seekSong(192).ok);const auto first=control->playSong();QVERIFY(first.ok);
  song->stop();song->playSong();QVERIFY(control->stopOwned(first.ownership).ok);QVERIFY(song->isPlaying());song->stop();
 }
 void projectAndLoopChangesInvalidateCopiedPreparation()
 {
  auto* song=Engine::getSong();auto control=createNativeRecordingTransport(*song);const auto before=control->snapshot();song->getTimeline(Song::PlayMode::Song).setLoopEnabled(true);
  const auto loop=control->snapshot();QVERIFY(loop.contextRevision>before.contextRevision);QVERIFY(loop.looping);
  song->clearProject();const auto after=control->snapshot();QVERIFY(after.projectGeneration>before.projectGeneration);QVERIFY(after.contextRevision>loop.contextRevision);
 }
};
QTEST_GUILESS_MAIN(RecordingTransportControlTest)
#include "RecordingTransportControlTest.moc"
