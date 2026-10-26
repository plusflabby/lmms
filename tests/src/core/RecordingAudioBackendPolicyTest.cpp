#include "RecordingAudioBackendPolicy.h"
#include <QtTest>
using namespace lmms::recording;
class RecordingAudioBackendPolicyTest : public QObject
{
 Q_OBJECT
private slots:
 void independentInputRequiresAnEstablishedOutputOnlyBackend()
 {
  const auto allowed=&RecordingAudioBackendPolicy::independentInputOutputAllowed;
  QVERIFY(allowed("Dummy (no sound output)",false,false));QVERIFY(allowed("SDL (Simple DirectMedia Layer)",false,true));QVERIFY(allowed("PortAudio",false,true));
  QVERIFY(!allowed("PortAudio",false,false));QVERIFY(!allowed("SDL (Simple DirectMedia Layer)",false,false));
  QVERIFY(!allowed("SDL (Simple DirectMedia Layer)",true,true));QVERIFY(!allowed("unknown backend",false,true));
 }
 void ordinaryBuildPreservesTheLegacyBackendInputPolicy()
 {QVERIFY(RecordingAudioBackendPolicy::legacyInputAllowed(false));}
 void qualificationBuildSuppressesLegacyInputBeforeLocalArm()
 {QVERIFY(!RecordingAudioBackendPolicy::legacyInputAllowed(true));}
};
QTEST_GUILESS_MAIN(RecordingAudioBackendPolicyTest)
#include "RecordingAudioBackendPolicyTest.moc"
