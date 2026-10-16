#include <QtTest>
#include "AiBridgeAudioTypes.h"
class AiBridgeAudioTypesTest : public QObject
{
	Q_OBJECT
	lmms::AiBridgeAudioClipState state()
	{
		lmms::AiBridgeAudioClipState s; s.id="clip-1";s.trackId="track-1";s.sourceStatus="available";
		s.sourceReferenceHash="reference";s.sourceIdentity="identity";s.sourceDigest="digest";
		s.position=300;s.length=40;s.offset=-10;s.sourceFrames=337;s.sampleRate=44100;s.sourceChannels=2;
		return s;
	}
private slots:
	void canonicalOrderingIsIndependentOfDiscoveryOrder()
	{
		auto a=state(),b=a;b.id="clip-2";b.position=400;
		const auto hash=lmms::audioStateHash({a,b}); QVERIFY(!hash.isEmpty());
		QCOMPARE(hash,lmms::audioStateHash({b,a})); QVERIFY(hash!=lmms::audioStateHash({a}));
	}
	void everyAuthoritativePropertyParticipatesInTheHash()
	{
		const auto base=state(); const auto hash=lmms::audioStateHash({base}); QVERIFY(!hash.isEmpty());
		std::vector<lmms::AiBridgeAudioClipState> variants;
		auto add=[&](auto change){ auto s=base; change(s);variants.push_back(s); };
		add([](auto& s){s.id="new";});add([](auto& s){s.trackId="new";});add([](auto& s){++s.position;});
		add([](auto& s){++s.length;});add([](auto& s){++s.offset;});add([](auto& s){s.sourceStatus="missing";});
		add([](auto& s){s.sourceReferenceHash="different";});add([](auto& s){s.sourceIdentity="different";});
		add([](auto& s){s.sourceDigest="different";});add([](auto& s){s.muted=true;});add([](auto& s){s.reversed=true;});
		add([](auto& s){s.autoResize=true;});add([](auto& s){s.color="#ffffff";});add([](auto& s){++s.sourceFrames;});
		add([](auto& s){s.sourceChannels.reset();});add([](auto& s){++s.sampleRate;});add([](auto& s){++s.fxChannel;});
		add([](auto& s){s.trackVolume=1;});add([](auto& s){s.trackPanning=1;});
		for(auto& s:variants) QVERIFY(hash!=lmms::audioStateHash({s}));
	}
	void duplicateOrEmptyNativeIdsAreNotHashableSnapshots()
	{
		auto s=state(); QVERIFY(lmms::audioStateHash({s,s}).isEmpty()); s.id.clear(); QVERIFY(lmms::audioStateHash({s}).isEmpty());
	}
};
QTEST_GUILESS_MAIN(AiBridgeAudioTypesTest)
#include "AiBridgeAudioTypesTest.moc"
