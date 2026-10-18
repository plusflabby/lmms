#include <QtTest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include "AiBridgeVocalTrackActions.h"
#include "AudioDevice.h"
#include "AudioEngine.h"
#include "Engine.h"
#include "SampleTrack.h"
#include "Song.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
using namespace lmms;

class AiBridgeVocalOperationSafetyAuditTest:public QObject
{
	Q_OBJECT
private slots:
	void initTestCase()
	{
#ifdef Q_OS_WIN
		SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX|SEM_NOOPENFILEERRORBOX);
#endif
		Engine::init(true);Engine::audioEngine()->audioDev()->stopProcessing();
	}
	void cleanupTestCase(){Engine::destroy();}
	void trackSetterMustSurviveSynchronousOwnerDeletion()
	{
		if(qEnvironmentVariableIsSet("LMMS_VOCAL_TRACK_DELETE_CHILD"))
		{
			auto track=std::make_unique<SampleTrack>(Engine::getSong());
			const auto id=AiBridgeProjectContext{}.idForTrack(track.get());
			const auto snapshot=captureVocalTrack(id);QVERIFY(snapshot.value);
			void* recycled=nullptr;bool reused=false;
			connect(track->volumeModel(),&FloatModel::dataChanged,this,[&]{
				const auto address=reinterpret_cast<quintptr>(track.get());delete track.release();
				recycled=std::malloc(sizeof(SampleTrack));reused=reinterpret_cast<quintptr>(recycled)==address;
				if(reused){std::memset(recycled,0,sizeof(SampleTrack));std::fputs("REUSED_SAMPLE_TRACK_STORAGE\n",stderr);std::fflush(stderr);}
			});
			QVERIFY(Engine::getSong()->isStopped());
			const auto exclusion=Engine::audioEngine()->requestChangesGuard();
			const auto result=setAudioTrackProperties({id,snapshot.value->hash,80.0,{},false});
			QVERIFY2(reused,"Native allocation reuse was not obtained; not evidence for this gate");
			QVERIFY(result.code!="ok");std::free(recycled);return;
		}
		QTemporaryDir dir;QVERIFY(dir.isValid());QProcess child;
		auto env=QProcessEnvironment::systemEnvironment();env.insert("LMMS_VOCAL_TRACK_DELETE_CHILD","1");child.setProcessEnvironment(env);
		child.start(QCoreApplication::applicationFilePath(),{"trackSetterMustSurviveSynchronousOwnerDeletion","-o",dir.filePath("child.log")+",txt"});
		QVERIFY(child.waitForFinished(30000));const auto diagnostic=child.readAllStandardError();
		qInfo().noquote()<<"SampleTrack action child exit"<<child.exitCode()<<"status"<<int(child.exitStatus())<<diagnostic;
		QVERIFY2(diagnostic.contains("REUSED_SAMPLE_TRACK_STORAGE"),"The native storage reuse precondition was not observed");
		QCOMPARE(child.exitStatus(),QProcess::NormalExit);QCOMPARE(child.exitCode(),0);
	}
};
QTEST_MAIN(AiBridgeVocalOperationSafetyAuditTest)
#include "AiBridgeVocalOperationSafetyAuditTest.moc"
