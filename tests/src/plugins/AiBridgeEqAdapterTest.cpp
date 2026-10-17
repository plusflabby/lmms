#include <QtTest>
#include <QDomDocument>
#include "AutomatableModel.h"
#include "AutomationClip.h"
#include "AutomationTrack.h"
#include "AudioDevice.h"
#include "AudioEngine.h"
#include "Effect.h"
#include "EffectControls.h"
#include "Engine.h"
#include "Song.h"
using namespace lmms;
namespace lmms {extern "C" Plugin* lmms_plugin_main(Model*,void*);}

// Native pre-adapter gate: verify the proposed EqControls-only signal mechanism
// before adding accessors/allowlist entries. Child-model inspection is TEST ONLY.
class AiBridgeEqAdapterTest:public QObject
{
	Q_OBJECT
	template<typename T>T* namedModel(EffectControls* controls,const QString& name)
	{
		for(auto* child:controls->children())if(auto* m=dynamic_cast<T*>(child))if(m->displayName()==name)return m;
		return nullptr;
	}
	QDomElement save(EffectControls* c)
	{
		QDomDocument d;auto e=d.createElement("controls");d.appendChild(e);return c->saveState(d,e);
	}
private slots:
	void initTestCase(){Engine::init(true);Engine::audioEngine()->audioDev()->stopProcessing();}
	void cleanupTestCase(){Engine::destroy();}
	void legacyRoundTripWithoutSlopeEditUnchanged()
	{
		std::unique_ptr<Effect> effect(dynamic_cast<Effect*>(lmms_plugin_main(nullptr,nullptr)));QVERIFY(effect);
		auto* controls=effect->controls();QVERIFY(controls);
		auto legacy=save(controls);legacy.setAttribute("HP",0);legacy.setAttribute("HP12",0);legacy.setAttribute("HP24",0);legacy.setAttribute("HP48",1);
		controls->restoreState(legacy);const auto after=save(controls);
		QCOMPARE(after.attribute("HP"),QString("0"));QCOMPARE(after.attribute("HP12"),QString("0"));QCOMPARE(after.attribute("HP48"),QString("1"));
	}
	void headlessSameValueAutomationCannotReachSlopeSync()
	{
		std::unique_ptr<Effect> effect(dynamic_cast<Effect*>(lmms_plugin_main(nullptr,nullptr)));QVERIFY(effect);
		auto* controls=effect->controls();QVERIFY(controls);
		auto legacy=save(controls);legacy.setAttribute("HP",0);legacy.setAttribute("HP12",0);legacy.setAttribute("HP24",0);legacy.setAttribute("HP48",1);
		controls->restoreState(legacy);
		auto* type=namedModel<IntModel>(controls,"High-pass type");auto* hp12=namedModel<BoolModel>(controls,"HP 12");
		auto* hp24=namedModel<BoolModel>(controls,"HP 24");auto* hp48=namedModel<BoolModel>(controls,"HP 48");
		QVERIFY(type && hp12 && hp24 && hp48);QCOMPARE(type->value(),0);QVERIFY(hp48->value());
		QSignalSpy changed(type,&IntModel::dataChanged),unchanged(type,&IntModel::dataUnchanged);
		// The approved helper's mechanism, with both change and unchanged signals,
		// connected directly so Qt thread delivery latency cannot explain a failure.
		const auto synchronize=[=]{hp12->setValue(type->value()==0,true);hp24->setValue(type->value()==1,true);hp48->setValue(type->value()==2,true);};
		auto changeConnection=connect(type,&IntModel::dataChanged,controls,synchronize,Qt::DirectConnection);
		auto unchangedConnection=connect(type,&IntModel::dataUnchanged,controls,synchronize,Qt::DirectConnection);
		// Positive controls: the identical sync mechanism works for real transitions.
		type->setValue(1,true);QVERIFY(!hp12->value() && hp24->value() && !hp48->value());
		type->setValue(2,true);QVERIFY(!hp12->value() && !hp24->value() && hp48->value());
		type->setValue(0,true);QVERIFY(hp12->value() && !hp24->value() && !hp48->value());
		disconnect(changeConnection);disconnect(unchangedConnection);controls->restoreState(legacy);
		QCOMPARE(type->value(),0);QVERIFY(hp48->value());changed.clear();unchanged.clear();
		connect(type,&IntModel::dataChanged,controls,synchronize,Qt::DirectConnection);
		connect(type,&IntModel::dataUnchanged,controls,synchronize,Qt::DirectConnection);
		AutomationTrack automation(Engine::getSong());AutomationClip clip(&automation);QVERIFY(clip.addObject(type,false));
		clip.putValue(0,0,false,false);clip.changeLength(192);
		Engine::getSong()->playSong();Engine::getSong()->setPlayPos(0);
		const auto stop=qScopeGuard([]{Engine::getSong()->stop();});
		Engine::getSong()->processNextBuffer();
		QCOMPARE(type->value(),0);QCOMPARE(changed.count(),0);QCOMPARE(unchanged.count(),0);
		// Feature RED: advertised enum0/12dB must select the actual native stages.
		QVERIFY2(hp12->value() && !hp24->value() && !hp48->value(),
			"Automated enum0 emitted no signal; native DSP still selects the retained HP48 stage. EqControls-only signal sync cannot satisfy this gate.");
	}
};
QTEST_MAIN(AiBridgeEqAdapterTest)
#include "AiBridgeEqAdapterTest.moc"
