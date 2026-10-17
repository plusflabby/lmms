#include <QtTest>
#include <limits>
#include "AiBridgeAudioRangePlanner.h"
using namespace lmms;
class AiBridgeAudioRangePlannerTest : public QObject
{
	Q_OBJECT
	AiBridgeAudioRangeWorld world(int rate=44100)
	{
		AiBridgeAudioRangeWorld w; w.generation=4; w.nativeWorld={125,rate,48,false}; w.fingerprint="tempo-topology";
		AiBridgeAudioClipSnapshot c; c.generation=4; c.sourceVerified=true;
		auto& s=c.state; s.id="clip-a";s.trackId="track-a";s.position=110;s.length=80;s.offset=-10;
		s.sourceFrames=quint64(rate*4);s.sampleRate=rate;s.sourceChannels=2;s.sourceStatus="available";
		s.sourceIdentity="identity";s.sourceDigest="digest";s.sourceReferenceHash="reference";
		s.playbackFrequency=440;s.amplification=1;s.trackVolume=100;
		SampleDecoder::PcmWaveInfo info;info.frames=s.sourceFrames;info.sampleRate=rate;info.channels=2;info.sourceSha256=s.sourceDigest;
		c.verifiedSourceInfo=info;c.endTick=190;c.ticksPerBeat=48;c.ticksPerBar=192;c.stateHash=audioStateHash({s});w.clips={c};return w;
	}
	AiBridgeAudioRangePlan plan(const AiBridgeAudioRangeWorld& w,AiBridgeAudioRangeKind kind=AiBridgeAudioRangeKind::Split,qint64 tick=140)
	{const auto& c=w.clips[0];return planAudioRange({kind,c.state.id,c.stateHash,tick},c,w);}
private slots:
	void literalTrimSplitMapping_data(){QTest::addColumn<int>("rate");QTest::newRow("44.1")<<44100;QTest::newRow("48")<<48000;}
	void literalTrimSplitMapping()
	{
		QFETCH(int,rate);auto w=world(rate);const qint64 q=rate/100;auto p=plan(w);QCOMPARE(p.code,QString("ok"));QCOMPARE(p.steps.size(),size_t(2));
		const auto& a=p.steps[0];const auto& b=p.steps[1];
		QCOMPARE(a.window.base.inFrame,10*q);QCOMPARE(a.window.base.outFrame,40*q);QCOMPARE(a.geometry.lengthTicks,qint64(30));
		QCOMPARE(b.window.base.inFrame,40*q);QCOMPARE(b.window.base.outFrame,90*q);QCOMPARE(b.geometry.positionTicks,qint64(140));QCOMPARE(b.geometry.lengthTicks,qint64(50));QCOMPARE(b.geometry.offsetTicks,qint64(-40));
		for(const auto& step:p.steps){QCOMPARE(step.window.base.phaseOriginFrame,10*q);QCOMPARE(step.window.referenceEndFrame,90*q);QCOMPARE(validateAudioWindowRecord(step.window),QString("ok"));}
		const auto left=plan(w,AiBridgeAudioRangeKind::TrimLeft);QCOMPARE(left.code,QString("ok"));QCOMPARE(left.steps.size(),size_t(1));QCOMPARE(left.steps[0].window.base.inFrame,b.window.base.inFrame);
		const auto right=plan(w,AiBridgeAudioRangeKind::TrimRight);QCOMPARE(right.code,QString("ok"));QCOMPARE(right.steps[0].window.base.outFrame,a.window.base.outFrame);
	}
	void splitAfterTrimRetainsOriginalEndpoint()
	{
		auto w=world();auto right=plan(w,AiBridgeAudioRangeKind::TrimRight,170);QCOMPARE(right.code,QString("ok"));
		auto& c=w.clips[0];c.state.rangeWindowV2=right.steps[0].window;c.state.rangeStatus="ok";c.state.length=60;c.endTick=170;c.stateHash=audioStateHash({c.state});
		auto p=plan(w);QCOMPARE(p.code,QString("ok"));QCOMPARE(p.steps[1].window.base.outFrame,qint64(30870));QCOMPARE(p.steps[1].window.referenceEndFrame,qint64(39690));
	}
	void expectedHashAndGenerationAreMandatory()
	{
		auto w=world();auto c=w.clips[0];QCOMPARE(planAudioRange({AiBridgeAudioRangeKind::Split,c.state.id,"stale",140},c,w).code,QString("clip_state_mismatch"));
		QCOMPARE(planAudioRange({AiBridgeAudioRangeKind::Split,"unknown",c.stateHash,140},c,w).code,QString("stale_id"));
		++w.generation;QCOMPARE(plan(w).code,QString("stale_id"));
	}
	void invalidBoundary_data()
	{
		QTest::addColumn<qint64>("tick");for(qint64 t:{-1LL,109LL,110LL,190LL,191LL,std::numeric_limits<qint64>::max()})QTest::newRow(qPrintable(QString::number(t)))<<t;
	}
	void invalidBoundary(){QFETCH(qint64,tick);auto p=plan(world(),AiBridgeAudioRangeKind::Split,tick);QVERIFY(p.code!="ok");QVERIFY(p.steps.empty());}
	void noEffectOnlyAfterEligibility()
	{
		auto w=world();QCOMPARE(plan(w,AiBridgeAudioRangeKind::TrimLeft,110).code,QString("no_effect"));QCOMPARE(plan(w,AiBridgeAudioRangeKind::TrimRight,190).code,QString("no_effect"));
		w.nativeWorld.tempoBpm=123;QCOMPARE(plan(w,AiBridgeAudioRangeKind::TrimLeft,110).code,QString("unsupported_tempo_map"));
	}
	void unsupportedStates_data()
	{QTest::addColumn<int>("mode");QTest::addColumn<QString>("code");QTest::newRow("automation")<<0<<"unsupported_tempo_map";QTest::newRow("rate")<<1<<"unsupported_sample_rate_mapping";QTest::newRow("reverse")<<2<<"unsupported_audio_operation";QTest::newRow("resize")<<3<<"audio_window_geometry_mismatch";QTest::newRow("missing")<<4<<"audio_source_missing";QTest::newRow("unknown-source")<<5<<"unsupported_audio_operation";QTest::newRow("unsafe-original")<<6<<"unrepresentable_audio_boundary";}
	void unsupportedStates()
	{
		QFETCH(int,mode);QFETCH(QString,code);auto w=world();auto& c=w.clips[0];
		switch(mode){case 0:w.nativeWorld.tempoAutomationPresent=true;break;case 1:w.nativeWorld.outputRate=48000;break;case 2:c.state.reversed=true;break;case 3:c.state.autoResize=true;break;case 4:c.state.sourceStatus="audio_source_missing";break;case 5:c.sourceVerified=false;break;case 6:c.state.length=137;c.endTick=247;break;}
		c.stateHash=audioStateHash({c.state});auto p=plan(w);QCOMPARE(p.code,code);QVERIFY(p.steps.empty());
	}
	void collisionAndWorldFingerprint()
	{
		auto w=world();auto base=plan(w);QCOMPARE(base.code,QString("ok"));QVERIFY(!base.expectedWorldHash.isEmpty());QCOMPARE(base.expectedWorldHash,plan(w).expectedWorldHash);
		auto neighbor=w.clips[0];neighbor.state.id="neighbor";neighbor.state.position=150;neighbor.state.length=10;neighbor.stateHash=audioStateHash({neighbor.state});w.clips.push_back(neighbor);
		QCOMPARE(plan(w).code,QString("destination_conflict"));QVERIFY(audioRangeWorldHash(w)!=base.expectedWorldHash);
		w.clips[1].state.position=300;QCOMPARE(plan(w).code,QString("ok"));QVERIFY(plan(w).expectedWorldHash!=base.expectedWorldHash);
	}
	void metadataAndSourceMismatchFailClosed()
	{
		auto w=world();w.clips[0].state.rangeStatus="unsupported_audio_window_version";w.clips[0].state.invalidRangeMetadataDigest=QByteArray(32,'x');w.clips[0].stateHash=audioStateHash({w.clips[0].state});QCOMPARE(plan(w).code,QString("unsupported_audio_window_version"));
		w=world();w.clips[0].verifiedSourceInfo->frames++;QCOMPARE(plan(w).code,QString("audio_source_changed"));
	}
};
QTEST_GUILESS_MAIN(AiBridgeAudioRangePlannerTest)
#include "AiBridgeAudioRangePlannerTest.moc"
