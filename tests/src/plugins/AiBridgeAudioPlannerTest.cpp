#include <QtTest>
#include <limits>
#include "AiBridgeAudioPlanner.h"
class AiBridgeAudioPlannerTest : public QObject
{
	Q_OBJECT
	lmms::AiBridgeAudioWorld world()
	{
		lmms::AiBridgeAudioWorld w;w.generation=4;w.tempo=120;w.ticksPerBar=192;w.outputFramesPerTick=459.375;
		w.tracks={{"track-a","lead",100,0,false,0,2},{"track-b","double",73,-10,false,0,0}};
		lmms::AiBridgeAudioClipState a;a.id="clip-a";a.trackId="track-a";a.position=100;a.length=40;a.offset=-10;
		a.sourceStatus="available";a.sourceFrames=44100;a.sampleRate=44100;a.sourceChannels=1;a.sourceIdentity="identity";a.sourceDigest="digest";a.sourceReferenceHash="reference";a.trackVolume=100;a.playbackFrequency=440;a.amplification=1;
		auto b=a;b.id="clip-b";b.position=200;w.clips={a,b};w.selected={a.id,b.id};return w;
	}
	lmms::AiBridgeAudioEdit edit(lmms::AiBridgeAudioEditKind kind,const lmms::AiBridgeAudioClipState& source,std::optional<qint64> position={},const QString& target={})
	{return {kind,source.id,lmms::audioStateHash({source}),target,position};}
private slots:
	void preciseMoveAndDuplicatePreserveNativeSourceWindow()
	{
		using namespace lmms;auto w=world();auto a=w.clips[0];
		auto plan=AiBridgeAudioPlanner::plan(w,{edit(AiBridgeAudioEditKind::Move,a,300),edit(AiBridgeAudioEditKind::Duplicate,a,500,"track-b")});
		QCOMPARE(plan.code,QStringLiteral("ok"));QCOMPARE(plan.steps.size(),size_t(2));
		QCOMPARE(plan.steps[0].after->position,qint64(300));QCOMPARE(plan.steps[0].after->offset,qint64(-10));QCOMPARE(plan.steps[0].after->length,qint64(40));
		QCOMPARE(plan.steps[1].before.position,qint64(300));QCOMPARE(plan.steps[1].after->position,qint64(500));QCOMPARE(plan.steps[1].after->trackId,QStringLiteral("track-b"));
		QCOMPARE(plan.steps[1].after->sourceIdentity,a.sourceIdentity);QCOMPARE(plan.steps[1].after->sourceDigest,a.sourceDigest);QCOMPARE(plan.steps[1].after->trackVolume,73.0);
		QCOMPARE(plan.affectedClips,2);QVERIFY(!plan.steps[0].beforeWorldHash.isEmpty());
		QCOMPARE(plan.preview,AiBridgeAudioPlanner::plan(w,{edit(AiBridgeAudioEditKind::Move,a,300),edit(AiBridgeAudioEditKind::Duplicate,a,500,"track-b")}).preview);
	}
	void completeInputPreflightRejectsLaterStaleValuesAndBadShapes()
	{
		using namespace lmms;auto w=world();auto a=w.clips[0],b=w.clips[1];auto stale=edit(AiBridgeAudioEditKind::Delete,b);stale.expectedAudioStateHash="old";
		auto plan=AiBridgeAudioPlanner::plan(w,{edit(AiBridgeAudioEditKind::Move,a,300),stale});QCOMPARE(plan.code,QStringLiteral("clip_state_mismatch"));QVERIFY(plan.steps.empty());
		QCOMPARE(AiBridgeAudioPlanner::plan(w,{edit(AiBridgeAudioEditKind::Move,a,-1)}).code,QStringLiteral("invalid_argument"));
		QCOMPARE(AiBridgeAudioPlanner::plan(w,{edit(AiBridgeAudioEditKind::Move,a,2147483647)}).code,QStringLiteral("invalid_argument"));
		QCOMPARE(AiBridgeAudioPlanner::plan(w,{edit(AiBridgeAudioEditKind::Delete,a,0)}).code,QStringLiteral("invalid_argument"));
		QCOMPARE(AiBridgeAudioPlanner::plan(w,{edit(static_cast<AiBridgeAudioEditKind>(99),a)}).code,QStringLiteral("invalid_argument"));
	}
	void projectedCollisionsAndSeparateTrackLayersAreDeterministic()
	{
		using namespace lmms;auto w=world();auto a=w.clips[0],b=w.clips[1];
		QCOMPARE(AiBridgeAudioPlanner::plan(w,{edit(AiBridgeAudioEditKind::Move,a,210)}).code,QStringLiteral("destination_conflict"));
		QCOMPARE(AiBridgeAudioPlanner::plan(w,{edit(AiBridgeAudioEditKind::Duplicate,a,100,"track-b")}).code,QStringLiteral("ok"));
		QCOMPARE(AiBridgeAudioPlanner::plan(w,{edit(AiBridgeAudioEditKind::Move,b,400),edit(AiBridgeAudioEditKind::Move,a,200)}).code,QStringLiteral("ok"));
		QCOMPARE(AiBridgeAudioPlanner::plan(w,{edit(AiBridgeAudioEditKind::Delete,a),edit(AiBridgeAudioEditKind::Duplicate,b,100,"track-a")}).code,QStringLiteral("ok"));
		QCOMPARE(AiBridgeAudioPlanner::plan(w,{edit(AiBridgeAudioEditKind::Move,a,300,"track-b")}).code,QStringLiteral("unsupported_audio_operation"));
	}
	void missingSourcesAreExplicitAndDeletionDoesNotRequireSourceAccess()
	{
		using namespace lmms;auto w=world();w.clips[0].sourceStatus="audio_source_missing";auto a=w.clips[0];
		QCOMPARE(AiBridgeAudioPlanner::plan(w,{edit(AiBridgeAudioEditKind::Move,a,300)}).code,QStringLiteral("audio_source_missing"));
		QCOMPARE(AiBridgeAudioPlanner::plan(w,{edit(AiBridgeAudioEditKind::Duplicate,a,300,"track-b")}).code,QStringLiteral("audio_source_missing"));
		QCOMPARE(AiBridgeAudioPlanner::plan(w,{edit(AiBridgeAudioEditKind::Delete,a)}).code,QStringLiteral("ok"));
	}
	void noEffectBoundsAndPreviewTruncationAreObservable()
	{
		using namespace lmms;auto w=world();auto a=w.clips[0];
		auto zero=AiBridgeAudioPlanner::plan(w,{edit(AiBridgeAudioEditKind::Move,a,100)});QCOMPARE(zero.code,QStringLiteral("no_effect"));QVERIFY(!zero.steps[0].changed);
		std::vector<AiBridgeAudioEdit> edits;for(int i=0;i<129;++i)edits.push_back(edit(AiBridgeAudioEditKind::Move,a,300+i*50));
		auto many=AiBridgeAudioPlanner::plan(w,edits);QCOMPARE(many.code,QStringLiteral("ok"));QCOMPARE(many.preview.size(),128);QVERIFY(many.previewHasMore);QCOMPARE(many.affectedClips,1);
		edits.assign(257,edit(AiBridgeAudioEditKind::Move,a,100));QCOMPARE(AiBridgeAudioPlanner::plan(w,edits).code,QStringLiteral("invalid_argument"));
		edits.clear();for(int i=0;i<32;++i)edits.push_back(edit(AiBridgeAudioEditKind::Duplicate,a,1000+i*50,"track-b"));
		QCOMPARE(AiBridgeAudioPlanner::plan(w,edits).code,QStringLiteral("invalid_argument"));
	}
	void nonpersistentNativeOverridesAndUnverifiedFileCopiesRejectBeforeWrites()
	{
		using namespace lmms;
		for(int variant=0;variant<6;++variant)
		{
			auto w=world();auto& s=w.clips[0];if(variant==0)s.playbackFrequency=330;else if(variant==1)s.amplification=0.5;else if(variant==2)s.solo=true;else if(variant==3)s.nativeName="not serialized by SampleClip";else if(variant==4)s.sourceStatus="unverified_legacy";else s.color="#80123456";
			QCOMPARE(AiBridgeAudioPlanner::plan(w,{edit(AiBridgeAudioEditKind::Duplicate,s,500,"track-b")}).code,QStringLiteral("unsupported_audio_operation"));
		}
	}
	void byteBoundedPreviewIsAlwaysAnOrderedPrefix()
	{
		using namespace lmms;
		for(int size=100;size<=128;++size)
		{
			auto w=world();w.tracks[0].id=QString(size,QChar('t'));w.clips[0].id=QString(size,QChar('c'));w.clips[0].trackId=w.tracks[0].id;w.clips[1].trackId=w.tracks[0].id;w.selected.insert(w.clips[0].id);
			const auto s=w.clips[0];std::vector<AiBridgeAudioEdit> edits;for(int i=0;i<129;++i)edits.push_back(edit(AiBridgeAudioEditKind::Move,s,300+i*50));edits.push_back(edit(AiBridgeAudioEditKind::Delete,s));
			const auto plan=AiBridgeAudioPlanner::plan(w,edits);QCOMPARE(plan.code,QStringLiteral("ok"));QVERIFY(plan.previewHasMore);
			for(int i=0;i<plan.preview.size();++i)QCOMPARE(plan.preview[i].toObject().value("step").toInt(),i);
		}
	}
	void invalidCopiedGeometryOrDestinationPropertiesReject_data()
	{QTest::addColumn<int>("variant");for(int i=0;i<4;++i)QTest::newRow(qPrintable(QString::number(i)))<<i;}
	void invalidCopiedGeometryOrDestinationPropertiesReject()
	{
		QFETCH(int,variant);using namespace lmms;auto w=world();if(variant==0)w.clips[0].position=-1;else if(variant==1)w.clips[0].length=0;else if(variant==2)w.clips[0].sampleRate=0;else w.tracks[1].volume=std::numeric_limits<double>::quiet_NaN();
		QCOMPARE(AiBridgeAudioPlanner::plan(w,{edit(AiBridgeAudioEditKind::Duplicate,w.clips[0],500,"track-b")}).code,QStringLiteral("unsupported_audio_operation"));
	}
	void projectedGeometryBudgetIsCheckedBeforeAnyExecution()
	{
		using namespace lmms;auto w=world();while(w.clips.size()<AiBridgeAudioPlanner::MaxCollisionRows){auto row=w.clips[1];row.id=QStringLiteral("collider-%1").arg(w.clips.size());row.position=1000+qint64(w.clips.size())*50;row.sourceStatus="collision_only";w.clips.push_back(row);}
		const auto duplicate=edit(AiBridgeAudioEditKind::Duplicate,w.clips[0],500,"track-b");QCOMPARE(AiBridgeAudioPlanner::plan(w,{duplicate}).code,QStringLiteral("result_too_large"));
		QCOMPARE(AiBridgeAudioPlanner::plan(w,{edit(AiBridgeAudioEditKind::Delete,w.clips[1]),duplicate}).code,QStringLiteral("ok"));
	}
};
QTEST_GUILESS_MAIN(AiBridgeAudioPlannerTest)
#include "AiBridgeAudioPlannerTest.moc"
