#include <QtTest>
#include <limits>
#include "AiBridgeVocalTypes.h"
using namespace lmms;

class AiBridgeVocalTypesTest : public QObject
{
	Q_OBJECT
	VocalInputSnapshot snapshot(int count=1, qint64 frames=44100)
	{
		VocalInputSnapshot s; s.generation=7; s.policyRevision=3;
		for(int i=0;i<count;++i)
		{
			VocalSelection selection{QString("clip-%1").arg(i),QString(64,'a'),{0,frames}};
			s.request.selections.push_back(selection);
			VocalSourceTicket t; t.clipId=selection.clipId; t.reference="G:/approved/voice.wav";
			t.roots=QStringList{"G:/approved"}; t.policyRevision=3;
			t.limits={100000000,100000000}; t.expectedIdentity="identity";
			t.expectedInfo={quint64(frames),quint64(frames)*2+44,quint64(frames)*8+4172,1,44100,QByteArray(32,'d')};
			t.sourceRevision="source-revision"; t.sourceStatus="available";
			s.sources.push_back(t);
		}
		return s;
	}
private slots:
	void canonicalFrameStringsAndExactRanges()
	{
		QVERIFY(parseVocalFrame("0").has_value());
		QCOMPARE(parseVocalFrame("0").value(),qint64(0));
		QCOMPARE(parseVocalFrame("9223372036854775807").value(),std::numeric_limits<qint64>::max());
		QCOMPARE(parseVocalFrame("9007199254740993").value(),qint64(9007199254740993LL));
		for(const auto* text:{"", "00", "01", "+1", "-1", "1e3", "1.0", " 1", "1\n", "9223372036854775808"})
			QVERIFY2(!parseVocalFrame(text),text);
		auto r=snapshot().request; QCOMPARE(validateVocalRequest(r),QString("ok"));
		r.selections[0].frames={5,5}; QCOMPARE(validateVocalRequest(r),QString("invalid_parameter"));
		r.selections[0].frames={-1,5}; QCOMPARE(validateVocalRequest(r),QString("invalid_parameter"));
	}
	void budgetCountsAllSelections()
	{
		auto s=snapshot(8,44100*15); QCOMPARE(estimateVocalBudget(s).code,QString("ok"));
		++s.request.selections[0].frames.outFrame; ++s.sources[0].expectedInfo.frames;
		QCOMPARE(estimateVocalBudget(s).code,QString("resource_limit"));
		QCOMPARE(estimateVocalBudget(snapshot(9)).code,QString("resource_limit"));
		s=snapshot(); s.request.selections.push_back(s.request.selections[0]);
		QCOMPARE(validateVocalRequest(s.request),QString("invalid_parameter"));
		s=snapshot(); s.request.selections[0].frames.outFrame=std::numeric_limits<qint64>::max();
		QCOMPARE(estimateVocalBudget(s).code,QString("invalid_parameter"));
		s=snapshot(); s.sources[0].expectedInfo.sampleRate=0;
		QCOMPARE(estimateVocalBudget(s).code,QString("invalid_parameter"));
	}
	void metadataReservationPrecedesAllocation()
	{
		auto s=snapshot(); const auto one=estimateVocalBudget(s); QCOMPARE(one.code,QString("ok"));
		QCOMPARE(one.recordCapacity,quint64(30)); // 20 windows + at most 10 low-level spans.
		QVERIFY(one.sessionBytes>=30*sizeof(VocalAnalysisRecord));
		QVERIFY(one.jobBytes>one.sessionBytes);
		const auto many=estimateVocalBudget(snapshot(8)); QVERIFY(many.sessionBytes>one.sessionBytes);
		s.sources[0].roots.clear(); for(int i=0;i<33;++i)s.sources[0].roots.append(QString(32768,'x'));
		QCOMPARE(estimateVocalBudget(s).code,QString("resource_limit"));
		s=snapshot(); s.sources[0].reference=QString(32769,'x');
		QCOMPARE(estimateVocalBudget(s).code,QString("resource_limit"));
		s=snapshot(); s.sources[0].limits={};
		QCOMPARE(estimateVocalBudget(s).code,QString("audio_resource_limits_not_configured"));
	}
	void trackHashTracksNativeFloatAndLifetime()
	{
		VocalTrackSnapshot s; s.trackId="track-1"; s.generation=4;s.volume=100;s.panning=0;s.name="Vocal";s.membershipRevision="member-a";
		const auto h=vocalTrackHash(s); QCOMPARE(h.size(),64); QCOMPARE(h,vocalTrackHash(s));
		for(int i=0;i<9;++i){auto changed=s;
			switch(i){case 0:changed.volume=100.1;break;case 1:changed.panning=-0.1;break;
			case 2:++changed.generation;break;case 3:changed.trackId="new";break;
			case 4:changed.membershipRevision="member-b";break;case 5:changed.name="Renamed";break;
			case 6:changed.muted=true;break;case 7:changed.solo=true;break;case 8:changed.mixerId="fx-2";break;}
			QVERIFY(h!=vocalTrackHash(changed));}
		s.volume=std::numeric_limits<double>::quiet_NaN(); QVERIFY(vocalTrackHash(s).isEmpty());
	}
	void inputHashBindsPrivateSourceAndSettingsWithoutLoss()
	{
		auto s=snapshot();const auto h=vocalInputHash(s);QCOMPARE(h.size(),64);
		QCOMPARE(h,vocalInputHash(s));
		for(int i=0;i<7;++i){auto c=s;switch(i){case 0:++c.generation;break;case 1:++c.policyRevision;break;
			case 2:c.sources[0].expectedIdentity="other";break;case 3:c.sources[0].expectedInfo.sourceSha256=QByteArray(32,'e');break;
			case 4:c.request.settings.lowLevelDbfs=-50;break;case 5:++c.request.selections[0].frames.inFrame;break;
			case 6:c.sources[0].reference="G:/approved/other.wav";break;}QVERIFY(h!=vocalInputHash(c));}
		s.request.settings.lowLevelDbfs=std::numeric_limits<double>::infinity();QVERIFY(vocalInputHash(s).isEmpty());
	}
};
QTEST_GUILESS_MAIN(AiBridgeVocalTypesTest)
#include "AiBridgeVocalTypesTest.moc"
