#include <QtTest>
#include <limits>
#include "AiBridgeVocalTrackActions.h"
#include "AiBridgeVocalContext.h"
#include "Engine.h"
#include "SampleTrack.h"
#include "SampleClip.h"
#include "Song.h"
#include "ProjectJournal.h"
using namespace lmms;
class AiBridgeVocalTrackActionsTest:public QObject
{
	Q_OBJECT
	QString id(SampleTrack& track){return AiBridgeProjectContext{}.idForTrack(&track);}
private slots:
	void initTestCase(){Engine::init(true);}
	void cleanupTestCase(){Engine::destroy();}
	void preservesFloatNativeValues()
	{
		SampleTrack track(Engine::getSong());SampleClip clip(&track);const auto key=id(track);
		const auto before=captureVocalTrack(key);QCOMPARE(before.code,QString("ok"));QVERIFY(before.value);
		auto result=setAudioTrackProperties({key,before.value->hash,100.1,-0.1,false});QCOMPARE(result.code,QString("ok"));
		QCOMPARE(track.volumeModel()->value(),100.1f);QCOMPARE(track.panningModel()->value(),-0.1f);
		QVERIFY(result.after);QVERIFY(result.after->hash!=before.value->hash);QCOMPARE(result.affectedClips.size(),size_t(1));
		for(double bad:{-0.1,200.1,100.01,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
			QCOMPARE(setAudioTrackProperties({key,result.after->hash,bad,{},false}).code,QString("invalid_parameter"));
		QCOMPARE(setAudioTrackProperties({key,result.after->hash,{},{},false}).code,QString("invalid_parameter"));
	}
	void dryRunInspectionNeverWritesAndNativeJournalUndoWorks()
	{
		SampleTrack track(Engine::getSong());const auto key=id(track);const auto original=track.volumeModel()->value();
		Engine::projectJournal()->clearJournal();const bool modifiedBefore=Engine::getSong()->isModified();
		const auto preview=setAudioTrackProperties({key,{},88.8,{},true});QCOMPARE(preview.code,QString("ok"));QVERIFY(preview.before);
		QCOMPARE(track.volumeModel()->value(),original);QCOMPARE(Engine::getSong()->isModified(),modifiedBefore);QVERIFY(!Engine::projectJournal()->canUndo());
		QCOMPARE(setAudioTrackProperties({key,{},88.8,{},false}).code,QString("track_state_mismatch"));
		QCOMPARE(setAudioTrackProperties({key,QString(64,'f'),88.8,{},true}).code,QString("track_state_mismatch"));
		Engine::projectJournal()->setJournalling(true);
		const auto applied=setAudioTrackProperties({key,preview.before->hash,88.8,{},false});QCOMPARE(applied.code,QString("ok"));
		QVERIFY(Engine::getSong()->isModified());QVERIFY(Engine::projectJournal()->canUndo());
		Engine::projectJournal()->undo();QCOMPARE(track.volumeModel()->value(),original);
	}
	void deletedRecreatedAndMembershipChangedAreStale()
	{
		auto track=std::make_unique<SampleTrack>(Engine::getSong());const auto key=id(*track);
		const auto before=captureVocalTrack(key);QVERIFY(before.value);
		auto* clip=new SampleClip(track.get());
		QCOMPARE(setAudioTrackProperties({key,before.value->hash,80.0,{},false}).code,QString("track_state_mismatch"));
		delete clip;track.reset();SampleTrack replacement(Engine::getSong());QVERIFY(id(replacement)!=key);
		QCOMPARE(setAudioTrackProperties({key,before.value->hash,80.0,{},false}).code,QString("stale_id"));
	}
	void largeTrackFailsBeforeWrite()
	{
		SampleTrack track(Engine::getSong());for(int i=0;i<129;++i)new SampleClip(&track);
		const auto original=track.volumeModel()->value();
		QCOMPARE(setAudioTrackProperties({id(track),{},50.0,{},true}).code,QString("resource_limit"));
		QCOMPARE(track.volumeModel()->value(),original);
	}
	void failureAfterOneModelReportsObservedPartial()
	{
		SampleTrack track(Engine::getSong());const auto key=id(track);const auto before=captureVocalTrack(key);QVERIFY(before.value);
		// Real native observer changes the second model's admissible range after the first write.
		connect(track.volumeModel(),&FloatModel::dataChanged,track.panningModel(),[&]{track.panningModel()->setRange(0,0,0.1f);});
		const auto result=setAudioTrackProperties({key,before.value->hash,90.0,-1.0,false});
		QCOMPARE(result.code,QString("partial_failure"));QVERIFY(result.applied.contains("volume"));QVERIFY(result.failed.contains("panning"));
		QCOMPARE(track.volumeModel()->value(),90.0f);QCOMPARE(track.panningModel()->value(),0.0f);
	}
};
QTEST_MAIN(AiBridgeVocalTrackActionsTest)
#include "AiBridgeVocalTrackActionsTest.moc"
