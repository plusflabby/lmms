#include <QtTest>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include "AudioDevice.h"
#include "AudioEngine.h"
#include "AutomatableModel.h"
#include "Effect.h"
#include "EffectControls.h"
#include "Engine.h"
#include "EqEffect.h"
#include "Song.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
using namespace lmms;
namespace lmms {extern "C" Plugin* lmms_plugin_main(Model*,void*);}
#ifdef Q_OS_WIN
namespace {
LONG CALLBACK nativeExceptionTrace(EXCEPTION_POINTERS* info)
{
	if(info->ExceptionRecord->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION)return EXCEPTION_CONTINUE_SEARCH;
	const auto base=reinterpret_cast<quintptr>(GetModuleHandleW(nullptr));
	const auto pc=reinterpret_cast<quintptr>(info->ExceptionRecord->ExceptionAddress);
	std::fprintf(stderr,"NATIVE_EXCEPTION_RVA=%llx FAULT_ADDRESS=%llx\n",
		static_cast<unsigned long long>(pc-base),static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[1]));
	std::fflush(stderr);return EXCEPTION_CONTINUE_SEARCH;
}
}
#endif

class AiBridgeRestrictedEqAdapterTest:public QObject
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
	void observerDeletionDoesNotContinueOnFreedModel()
	{
		if(qEnvironmentVariableIsSet("LMMS_RESTRICTED_EQ_DELETE_CHILD"))
		{
#ifdef Q_OS_WIN
			AddVectoredExceptionHandler(1,nativeExceptionTrace);
#endif
			std::unique_ptr<Effect> effect(dynamic_cast<Effect*>(lmms_plugin_main(nullptr,nullptr)));QVERIFY(effect);
			FloatModel* gain=nullptr;
			for(auto* child:effect->controls()->children())if(auto* model=dynamic_cast<FloatModel*>(child))
				if(model->displayName()=="Input gain")gain=model;
			QVERIFY(gain);QPointer<Effect> alive=effect.get();
			void* recycled=nullptr;bool reused=false;
			connect(gain,&FloatModel::dataChanged,this,[&]{
				const auto address=reinterpret_cast<quintptr>(effect.get());
				delete effect.release();
				// Ordinary valid reuse of released native-effect storage. Poison the
				// NEW allocation, never a dangling pointer, to make a stale read deterministic.
				recycled=std::malloc(sizeof(EqEffect));
				reused=reinterpret_cast<quintptr>(recycled)==address;
				if(reused)
				{
					std::memset(recycled,0,sizeof(EqEffect));
					std::fputs("REUSED_EQ_STORAGE\n",stderr);std::fflush(stderr);
				}
			});
			QVERIFY(Engine::getSong()->isStopped());
			const auto exclusion=Engine::audioEngine()->requestChangesGuard();
			gain->setValue(1.0f); // Actual native control-thread setter, unchanged signals/journal semantics.
			QVERIFY(!alive);QVERIFY2(reused,"Native allocation reuse was not obtained; not evidence for this gate");
			std::free(recycled);return;
		}
		QTemporaryDir dir;QVERIFY(dir.isValid());
		QProcess child;auto env=QProcessEnvironment::systemEnvironment();
		env.insert("LMMS_RESTRICTED_EQ_DELETE_CHILD","1");child.setProcessEnvironment(env);
		child.start(QCoreApplication::applicationFilePath(),{"observerDeletionDoesNotContinueOnFreedModel","-o",dir.filePath("child.log")+",txt"});
		QVERIFY(child.waitForFinished(30000));
		const auto diagnostic=child.readAllStandardError();
		qInfo().noquote()<<"Native callback child exit"<<child.exitCode()<<"status"<<int(child.exitStatus())<<diagnostic;
		QVERIFY2(diagnostic.contains("REUSED_EQ_STORAGE"),"The native storage reuse precondition was not observed");
		QCOMPARE(child.exitStatus(),QProcess::NormalExit);
		QCOMPARE(child.exitCode(),0);
	}
};
QTEST_MAIN(AiBridgeRestrictedEqAdapterTest)
#include "AiBridgeRestrictedEqAdapterTest.moc"
