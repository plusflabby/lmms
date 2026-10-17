#include <QtTest>
#include <cmath>
#include <limits>
#include "AiBridgeVocalAnalysis.h"
using namespace lmms;
class AiBridgeVocalAnalysisTest:public QObject
{
	Q_OBJECT
	VocalSelectionMetadata meta(qint64 in,qint64 out,int channels=1,int rate=1000)
	{VocalSelectionMetadata m;m.clipId="clip";m.sourceRevision="revision";m.generation=1;m.frames={in,out};m.sampleRate=rate;m.channels=channels;return m;}
	VocalAnalysisResult run(const std::vector<SampleFrame>& input,VocalSelectionMetadata m,int chunk=17)
	{
		VocalAnalysisAccumulator a({},m);
		for(size_t i=0;i<input.size();i+=chunk)
			if(a.consume(i,std::span(input.data()+i,std::min<size_t>(chunk,input.size()-i)))!=PcmWaveVisitDecision::Continue)return {};
		return a.finish();
	}
private slots:
	void silenceDcAndDbfsHaveDefinedValues()
	{
		for(float value:{0.0f,0.5f})
		{
			const auto r=run(std::vector<SampleFrame>(100,{value,value}),meta(0,100));QCOMPARE(r.code,QString("ok"));
			QCOMPARE(r.summaries.size(),size_t(1));const auto c=r.summaries[0].channels[0];
			QCOMPARE(c.count,quint64(100));QCOMPARE(c.dc,double(value));QCOMPARE(c.rms,double(value));QCOMPARE(c.peak,double(value));
			QCOMPARE(c.silence,value==0);
			if(value==0){QVERIFY(!c.rmsDbfs);QVERIFY(!c.peakDbfs);}else{QVERIFY(c.rmsDbfs);QVERIFY(std::abs(*c.rmsDbfs+6.020599913279624)<1e-12);}
		}
	}
	void pcm16EndpointsAreCandidatesNotOverrange()
	{
		const auto r=run({{-1,-1},{32767/32768.0f,32767/32768.0f}},meta(0,2));QCOMPARE(r.code,QString("ok"));
		const auto c=r.summaries[0].channels[0];QCOMPARE(c.count,quint64(2));QCOMPARE(c.fullScaleCandidates,quint64(2));
		QCOMPARE(c.overrange,quint64(0));QCOMPARE(r.summaries[0].channels[1].count,quint64(0));
	}
	void windowChunksAndFinalPartialAreExact()
	{
		std::vector<SampleFrame> input(135,{-1,-1});for(size_t i=5;i<128;++i)input[i]={0.25f,0.5f};
		const auto reference=run(input,meta(5,128,2),1);QCOMPARE(reference.code,QString("ok"));QCOMPARE(reference.records.size(),size_t(3));
		QCOMPARE(reference.records[0].frames.inFrame,qint64(5));QCOMPARE(reference.records[2].frames.outFrame,qint64(128));
		QCOMPARE(reference.records[2].channels[0].count,quint64(23));
		for(int chunk:{17,4096})
		{
			const auto r=run(input,meta(5,128,2),chunk);QCOMPARE(r.code,QString("ok"));QCOMPARE(r.records.size(),reference.records.size());
			for(size_t j=0;j<r.records.size();++j){QCOMPARE(r.records[j].frames.inFrame,reference.records[j].frames.inFrame);
				QCOMPARE(r.records[j].channels[0].rms,0.25);QCOMPARE(r.records[j].channels[1].dc,0.5);}
		}
		const auto unusual=run(std::vector<SampleFrame>(103,{0.5f,0.5f}),meta(0,103,1,1001));
		QCOMPARE(unusual.code,QString("ok"));QCOMPARE(unusual.summaries[0].provenance.windowFrames,qint64(50));
		QCOMPARE(unusual.records.back().channels[0].count,quint64(3));
	}
	void lowLevelRunsUseAllChannelsAndExactDuration()
	{
		auto input=std::vector<SampleFrame>(200,{0,0});
		auto r=run(input,meta(0,200,2));QCOMPARE(r.code,QString("ok"));QCOMPARE(r.records.size(),size_t(5));
		QCOMPARE(r.records.back().kind,VocalAnalysisRecord::Kind::LowLevelSpan);
		QCOMPARE(r.records.back().frames.outFrame,qint64(200));
		for(auto& f:input)f[1]=0.5;
		r=run(input,meta(0,200,2));QCOMPARE(r.code,QString("ok"));QCOMPARE(r.records.size(),size_t(4));
		r=run(std::vector<SampleFrame>(99,{0,0}),meta(0,99));QCOMPARE(r.records.size(),size_t(2));
		r=run(std::vector<SampleFrame>(100,{0,0}),meta(0,100));QCOMPARE(r.records.size(),size_t(3));
	}
	void incompleteNonfiniteAndDiscontinuousStreamsNeverSucceed()
	{
		VocalAnalysisAccumulator a({},meta(0,100));std::array<SampleFrame,1> value{{{0,0}}};
		QCOMPARE(a.consume(0,value),PcmWaveVisitDecision::Continue);QVERIFY(a.finish().code!="ok");
		VocalAnalysisAccumulator b({},meta(0,2));QCOMPARE(b.consume(0,value),PcmWaveVisitDecision::Continue);
		QVERIFY(b.consume(2,value)!=PcmWaveVisitDecision::Continue);QVERIFY(b.finish().code!="ok");
		VocalAnalysisAccumulator c({},meta(0,1));value[0][0]=std::numeric_limits<float>::infinity();
		QVERIFY(c.consume(0,value)!=PcmWaveVisitDecision::Continue);QVERIFY(c.finish().code!="ok");
	}
};
QTEST_GUILESS_MAIN(AiBridgeVocalAnalysisTest)
#include "AiBridgeVocalAnalysisTest.moc"
