#include <QtTest>
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QDomDocument>
#include "AiBridgeAutomationActions.h"
#include "AiBridgeAutomationReconciler.h"
#include "AiBridgeProjectContext.h"
#include "AudioDevice.h"
#include "AudioEngine.h"
#include "AudioBusHandle.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "SampleTrack.h"
#include "PlayHandle.h"
#include "Song.h"
#include <algorithm>
using namespace lmms;
class AiBridgeSampleTrackAutomationTest:public QObject
{
	Q_OBJECT
	void reconcile(){AiBridgeProjectContext ids;AiBridgeAutomationReconciler(ids,aiBridgeAutomationRegistry(),aiBridgeAutomationTargets()).reconcile();}
	QVector<AiBridgeAutomationTargetSnapshot> owned(Track* track)
	{reconcile();return aiBridgeAutomationTargets().targetsForOwner(AiBridgeProjectContext{}.idForTrack(track));}
private slots:
	void initTestCase(){Engine::init(true);Engine::audioEngine()->audioDev()->stopProcessing();}
	void cleanupTestCase(){Engine::destroy();}
	void sampleTrackTargetsUseFloatModelsAndRetireExactOwner()
	{
		auto track=std::make_unique<SampleTrack>(Engine::getSong());auto first=owned(track.get());QCOMPARE(first.size(),2);
		for(const auto& t:first){QCOMPARE(t.valueType,QString("float"));QCOMPARE(t.step,0.1f);
			QCOMPARE(aiBridgeAutomationTargets().bindingFor(t.automationTargetId),t.parameterId=="volume" ? track->volumeModel() : track->panningModel());}
		const auto key=AiBridgeProjectContext{}.idForTrack(track.get());const auto stable=owned(track.get());
		QCOMPARE(first[0].automationTargetId,stable[0].automationTargetId);
		track.reset();reconcile();QCOMPARE(aiBridgeAutomationTargets().targetsForOwner(key).size(),0);
		SampleTrack replacement(Engine::getSong());QCOMPARE(owned(&replacement).size(),2);
		QVERIFY(owned(&replacement)[0].automationTargetId!=first[0].automationTargetId);
	}
	void instrumentTargetsRemainStable()
	{
		InstrumentTrack instrument(Engine::getSong());const auto before=owned(&instrument);QCOMPARE(before.size(),2);
		SampleTrack sample(Engine::getSong());QCOMPARE(owned(&sample).size(),2);Engine::getSong()->moveTrack(&sample,0);const auto after=owned(&instrument);
		QCOMPARE(before[0].automationTargetId,after[0].automationTargetId);
		for(const auto& t:after){QCOMPARE(t.valueType,QString("integer"));QCOMPARE(t.step,1.0f);}
	}
	void nativeAutomationActuallyChangesSampleBus()
	{
		SampleTrack track(Engine::getSong());const auto targets=owned(&track);QCOMPARE(targets.size(),2);
		const auto automation=AiBridgeAutomationActions::createTrack("disposable vocal automation");QVERIFY(automation.success);
		const auto cleanup=qScopeGuard([&]{Engine::getSong()->stop();Engine::audioEngine()->removePlayHandlesOfTypes(&track,PlayHandle::Type::SamplePlayHandle);AiBridgeAutomationActions::deleteTrack(automation.automationTrackId);});
		QStringList clipIds;
		for(const auto& t:targets)
		{
			const auto clip=AiBridgeAutomationActions::createClip(automation.automationTrackId,0,192,t.automationTargetId);QVERIFY(clip.success);
			clipIds.append(clip.automationClipId);
			QVERIFY(AiBridgeAutomationActions::setClipTarget(clip.automationClipId,t.automationTargetId).success);
			AiBridgeAutomationPointMutation point;point.tick=0;point.value=t.parameterId=="volume" ? 80.1f : 25.1f;point.curve="discrete";
			AiBridgeAutomationClipSnapshot nativeClip;AiBridgeAutomationPointPage nativePoints;
			QVERIFY(AiBridgeAutomationContext{}.getClip(clip.automationClipId,0,256,nativeClip,nativePoints));
			for(const auto& existing:nativePoints.items)if(existing.tick==0){point.kind=AiBridgeAutomationPointMutation::Kind::Update;point.pointId=existing.pointId;}
			const auto edited=AiBridgeAutomationActions::editPoints(clip.automationClipId,{point});QVERIFY2(edited.success,qPrintable(edited.code));
		}
		// Deterministic test-only signal, through the real SampleTrack bus; no legacy sample resampler.
		class ConstantHandle:public PlayHandle
		{
			SampleTrack* m_track;
		public:
			explicit ConstantHandle(SampleTrack* t):PlayHandle(Type::SamplePlayHandle),m_track(t){setAudioBusHandle(t->audioBusHandle());}
			void play(SampleFrame* buffer)override{std::fill_n(buffer,Engine::audioEngine()->framesPerPeriod(),SampleFrame{0.25f,0.25f});}
			bool isFinished()const override{return false;}
			bool isFromTrack(const Track* t)const override{return t==m_track;}
		};
		Engine::getSong()->playSong();Engine::getSong()->setPlayPos(0);
		QVERIFY(Engine::audioEngine()->addPlayHandle(new ConstantHandle(&track)));
		std::vector<SampleFrame> rendered;
		for(int i=0;i<8;++i){const auto output=Engine::audioEngine()->renderNextPeriod();rendered.assign(output.begin(),output.end());}
		QCOMPARE(track.volumeModel()->value(),80.1f);QCOMPARE(track.panningModel()->value(),25.1f);QVERIFY(!rendered.empty());
		QVERIFY(std::abs(rendered.back()[0]-0.14998725f)<1e-5f);QVERIFY(std::abs(rendered.back()[1]-0.20025f)<1e-5f);
		QVERIFY(AiBridgeAutomationActions::deleteClip(clipIds[0]).success);QCOMPARE(owned(&track).size(),2);
		QVERIFY(!aiBridgeAutomationRegistry().resolveClipToken(clipIds[0]));
	}
	void saveReloadRestartRediscovery()
	{
		const auto from=qEnvironmentVariable("LMMS_VOCAL_AUTOMATION_RELOAD_FIXTURE");
		if(!from.isEmpty())
		{
			QFile f(from);QVERIFY(f.open(QIODevice::ReadOnly));QDomDocument d;QVERIFY(d.setContent(f.readAll()));
			SampleTrack track(Engine::getSong());track.loadTrackSpecificSettings(d.documentElement());
			QCOMPARE(track.volumeModel()->value(),88.8f);QCOMPARE(track.panningModel()->value(),-0.1f);
			QCOMPARE(owned(&track).size(),2);for(const auto& t:owned(&track))QCOMPARE(t.valueType,QString("float"));
			for(const auto& t:owned(&track))QVERIFY(t.automationTargetId!=qEnvironmentVariable("LMMS_VOCAL_OLD_AUTOMATION_TARGET"));
			return;
		}
		QTemporaryDir dir;QVERIFY(dir.isValid());SampleTrack track(Engine::getSong());track.volumeModel()->setValue(88.8f);track.panningModel()->setValue(-0.1f);
		QDomDocument d;auto root=d.createElement("sampletrack");d.appendChild(root);track.saveTrackSpecificSettings(d,root,false);
		const auto path=dir.filePath("track.xml");{QFile f(path);QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::NewOnly));QVERIFY(f.write(d.toByteArray())>0);}
		QProcess child;auto env=QProcessEnvironment::systemEnvironment();env.insert("LMMS_VOCAL_AUTOMATION_RELOAD_FIXTURE",path);
		env.insert("LMMS_VOCAL_OLD_AUTOMATION_TARGET",owned(&track).first().automationTargetId);child.setProcessEnvironment(env);
		child.start(QCoreApplication::applicationFilePath(),{"saveReloadRestartRediscovery","-o",dir.filePath("child.log")+",txt"});
		QVERIFY(child.waitForFinished(30000));QCOMPARE(child.exitStatus(),QProcess::NormalExit);QCOMPARE(child.exitCode(),0);
	}
};
QTEST_MAIN(AiBridgeSampleTrackAutomationTest)
#include "AiBridgeSampleTrackAutomationTest.moc"
