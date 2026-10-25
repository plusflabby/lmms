#include "RecordingAudioBackendPolicy.h"
#include <QtTest>
using namespace lmms::recording;
class RecordingAudioBackendPolicyTest : public QObject
{
 Q_OBJECT
private slots:
 void ordinaryBuildPreservesTheLegacyBackendInputPolicy()
 {QVERIFY(RecordingAudioBackendPolicy::legacyInputAllowed(false));}
 void qualificationBuildSuppressesLegacyInputBeforeLocalArm()
 {QVERIFY(!RecordingAudioBackendPolicy::legacyInputAllowed(true));}
};
QTEST_GUILESS_MAIN(RecordingAudioBackendPolicyTest)
#include "RecordingAudioBackendPolicyTest.moc"
