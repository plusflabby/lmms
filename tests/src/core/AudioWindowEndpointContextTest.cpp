// Endpoint-context investigation only: test-only proposal codec/index oracle
// versus A4-backed native references, plus explicit unresolved native REDs.
// No production codec or renderer is implemented by these tests.
#include <QtTest>
#include <QtEndian>
#include <QCryptographicHash>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <cstring>
#include "AudioImportPolicy.h"
#include "AudioDevice.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "SampleClip.h"
#include "SamplePlayHandle.h"
#include "AudioResampler.h"
#include <samplerate.h>
#include "AudioEndpointContextModel.h"
#include "ProjectJournal.h"
#include "SampleTrack.h"
#include "Song.h"

class WindowProbeDevice final : public lmms::AudioDevice
{
public:
    explicit WindowProbeDevice(int rate):AudioDevice(2,lmms::Engine::audioEngine()){setSampleRate(rate);}
private:
    void startProcessingImpl() override {}
    void stopProcessingImpl() override {}
};
class AudioWindowEndpointContextTest : public QObject
{
	Q_OBJECT
	QString directory, source;
	QByteArray digest;
	QStringList old;
	const QStringList keys{"audio_import_roots","audio_import_max_file_bytes","audio_import_max_decoded_bytes"};
	std::vector<lmms::SampleFrame> reference(lmms::SampleClip& clip,int origin,int end)
	{
		using namespace lmms;clip.setSampleStartFrame(origin);clip.setSamplePlayLength(end);
		SamplePlayHandle handle(&clip);const auto period=Engine::audioEngine()->framesPerPeriod();
		std::vector<SampleFrame> output,block(period);
		for(int base=0;base<end-origin;base+=period){handle.play(block.data());output.insert(output.end(),block.begin(),block.end());}
		output.resize(end-origin);return output;
	}
	lmms::test::EndpointContextProposal proposal(int rate,int originTick,int endTick)
	{
		const int q=rate/100;
		return {{originTick*q,endTick*q,originTick*q,rate*2,rate,125,rate,48,q,endTick-originTick,-originTick},endTick*q};
	}
	void rates()
	{
		QTest::addColumn<int>("sourceRate");QTest::newRow("44100-stereo")<<44100;QTest::newRow("48000-stereo")<<48000;
		QTest::newRow("44100-mono")<<44100;QTest::newRow("48000-mono")<<48000;
	}
	QByteArray fileHash()
	{
		QFile f(source); if(!f.open(QIODevice::ReadOnly)) return {};
		return QCryptographicHash::hash(f.readAll(),QCryptographicHash::Sha256);
	}
private slots:
	void initTestCase() { lmms::Engine::init(true); lmms::Engine::audioEngine()->audioDev()->stopProcessing(); }
	void cleanupTestCase() { lmms::Engine::destroy(); }
	void init()
	{
		using namespace lmms; Engine::getSong()->setTempo(125); QFETCH(int,sourceRate); Engine::audioEngine()->setAudioDevice(new WindowProbeDevice(sourceRate),false);
		for(const auto& key:keys) old<<ConfigManager::inst()->value("aibridge",key,"window-test-absent");
		directory=QCoreApplication::applicationDirPath()+"/window-persistence-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
		QVERIFY(QDir().mkpath(directory)); source=directory+"/original.wav";
		const int channels=QString::fromLatin1(QTest::currentDataTag()).contains("mono")?1:2;
		const int sourceFrames=2*sourceRate,align=channels*2; QByteArray bytes(44+sourceFrames*align,0); std::memcpy(bytes.data(),"RIFF",4); std::memcpy(bytes.data()+8,"WAVEfmt ",8); std::memcpy(bytes.data()+36,"data",4);
		qToLittleEndian<quint32>(36+sourceFrames*align,bytes.data()+4); qToLittleEndian<quint32>(16,bytes.data()+16);
		qToLittleEndian<quint16>(1,bytes.data()+20); qToLittleEndian<quint16>(channels,bytes.data()+22);
		qToLittleEndian<quint32>(sourceRate,bytes.data()+24); qToLittleEndian<quint32>(sourceRate*align,bytes.data()+28);
		qToLittleEndian<quint16>(align,bytes.data()+32); qToLittleEndian<quint16>(16,bytes.data()+34); qToLittleEndian<quint32>(sourceFrames*align,bytes.data()+40);
		for(int i=0;i<sourceFrames;++i) { qToLittleEndian<qint16>((i*7919u)%30001u-15000,bytes.data()+44+i*align); if(channels==2)qToLittleEndian<qint16>((i*3571u)%29009u-14504,bytes.data()+46+i*align); }
		{QFile f(source); QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::NewOnly)); QCOMPARE(f.write(bytes),qint64(bytes.size()));}
		digest=fileHash();
		ConfigManager::inst()->setValue("aibridge",keys[0],QString::fromUtf8(QJsonDocument(QJsonArray{directory}).toJson(QJsonDocument::Compact)));
		ConfigManager::inst()->setValue("aibridge",keys[1],"1000000"); ConfigManager::inst()->setValue("aibridge",keys[2],"2000000");
	}
	void cleanup()
	{
		using namespace lmms;
		QCOMPARE(fileHash(),digest);
		for(int i=0;i<keys.size();++i) {if(old[i]=="window-test-absent") ConfigManager::inst()->deleteValue("aibridge",keys[i]); else ConfigManager::inst()->setValue("aibridge",keys[i],old[i]);}
		old.clear(); AudioImportPolicy::configured().refresh();
	}
    void terminalInputMustNotReadOutsideDeclaredSpan_data()
    {
        QTest::addColumn<int>("sourceRate"); QTest::newRow("linear-terminal")<<44100;
    }
    void terminalInputMustNotReadOutsideDeclaredSpan()
    {
        using namespace lmms;
        auto probe=[](float guard) {
            AudioResampler resampler(AudioResampler::Mode::Linear);
            std::array<SampleFrame,256> history,output;
            std::fill(history.begin(),history.end(),SampleFrame{0.125f,0.125f});
            auto first=resampler.process({&history[0][0],2,history.size()},{&output[0][0],2,output.size()});
            std::array<SampleFrame,3> boundedAllocation{SampleFrame{guard,guard},SampleFrame{0.5f,0.5f},SampleFrame{0.75f,0.75f}};
            output.fill(SampleFrame{-1,-1});
            auto last=resampler.process({&boundedAllocation[1][0],2,1},{&output[0][0],2,output.size()});
            qInfo()<<"TERMINAL_COUNTS"<<"history"<<first.inputFramesUsed<<first.outputFramesGenerated
                <<"guard"<<guard<<"last"<<last.inputFramesUsed<<last.outputFramesGenerated<<"value"<<output[0][0];
            return output[0][0];
        };
        qInfo()<<"SRC_VERSION"<<src_get_version();
        const float a=probe(0.25f),b=probe(0.75f);
        QCOMPARE(a,b);
        QCOMPARE(a,0.125f);
    }
	void declaredInputSpanContract_data()
	{
		QTest::addColumn<int>("sourceRate");
		QTest::addColumn<int>("channels");
		QTest::addColumn<int>("endOfInput");
		for(int channels:{1,2}) for(int end:{0,1})
			QTest::newRow(qPrintable(QString("channels-%1-eof-%2").arg(channels).arg(end)))<<44100<<channels<<end;
	}
	void declaredInputSpanContract()
	{
		QFETCH(int,channels); QFETCH(int,endOfInput);
		// Every access in this diagnostic stays within a live, initialized float
		// allocation. Only the declared SRC_DATA view excludes the prefix. No
		// guard page, invalid pointer, uninitialized data or exploit payload.
		auto probe=[&](float prefix,int offeredFrames) {
			int error=0; auto* state=src_new(SRC_LINEAR,channels,&error);
			if(!state || error) {src_delete(state);return -1000.0f;}
			std::array<float,512> history,output;
			history.fill(0.125f);output.fill(-1.0f);
			SRC_DATA first{};first.data_in=history.data();first.data_out=output.data();
			first.input_frames=256;first.output_frames=256;first.src_ratio=1.0;
			error=src_process(state,&first);
			std::array<float,6> allocation;allocation.fill(0.5f);
			std::fill_n(allocation.begin(),channels,prefix);
			SRC_DATA last{};last.data_in=allocation.data()+channels;last.data_out=output.data();
			last.input_frames=offeredFrames;last.output_frames=256;last.src_ratio=1.0;last.end_of_input=endOfInput;
			if(!error)error=src_process(state,&last);
			qInfo()<<"DECLARED_SPAN"<<"channels"<<channels<<"eof"<<endOfInput
				<<"offered"<<offeredFrames<<"prefix"<<prefix<<"error"<<error
				<<"history"<<first.input_frames_used<<first.output_frames_gen
				<<"last"<<last.input_frames_used<<last.output_frames_gen<<"sample"<<output[0];
			const float value=error?-1000.0f:output[0];src_delete(state);return value;
		};
		// Control: a two-frame input uses owned streaming history, independent
		// of the prefix. Then change only input_frames from two to one.
		QCOMPARE(probe(0.25f,2),0.125f);QCOMPARE(probe(0.75f,2),0.125f);
		const float a=probe(0.25f,1),b=probe(0.75f,1);
		QCOMPARE(a,b); // Contract RED: currently 0.25 versus 0.75, both EOF flags.
		QCOMPARE(a,0.125f);
	}
	void endpointContextDistinguishesAmbiguity_data(){QTest::addColumn<int>("sourceRate");QTest::newRow("44100-ambiguity")<<44100;}
	void endpointContextDistinguishesAmbiguity()
	{
		using namespace lmms;using namespace lmms::test;
		SampleTrack track(Engine::getSong());SampleClip clip(&track);auto prepared=AudioImportPolicy::configured().prepare(source);
		QCOMPARE(clip.adoptAuthorizedAudio(std::move(prepared.audio)),QString("ok"));
		const auto shortReference=reference(clip,0,60417),longReference=reference(clip,0,88200);
		auto a=proposal(44100,0,137),b=subwindow(proposal(44100,0,200),0,60417);
		auto ar=readProposal(writeProposal(a)),br=readProposal(writeProposal(b));QVERIFY(ar && br);
		QCOMPARE(ar->referenceEndFrame,qint64(60417));QCOMPARE(br->referenceEndFrame,qint64(88200));
		QCOMPARE(proposalEligibility(*ar),QString("unrepresentable_audio_boundary"));QCOMPARE(proposalEligibility(*br),QString("ok"));
		QCOMPARE(shortReference.back()[0],0.0f);QCOMPARE(longReference[60416][0],-0.44439697265625f);
		QVERIFY(!proposalSample(*ar,clip.sample(),60416));auto last=proposalSample(*br,clip.sample(),60416);QVERIFY(last);
		QCOMPARE((*last)[0],longReference[60416][0]);
		qInfo()<<"ENDPOINT_PROPOSAL"<<ar->referenceEndFrame<<br->referenceEndFrame<<"short_rejected_long_excerpt"<<(*last)[0];
	}
	void representationSurvivesTrimsSplitsTransportAndReload_data(){rates();}
	void representationSurvivesTrimsSplitsTransportAndReload()
	{
		using namespace lmms;using namespace lmms::test;QFETCH(int,sourceRate);
		SampleTrack track(Engine::getSong());SampleClip clip(&track);auto prepared=AudioImportPolicy::configured().prepare(source);
		QCOMPARE(clip.adoptAuthorizedAudio(std::move(prepared.audio)),QString("ok"));const int q=sourceRate/100;
		int checked=0,rejected=0;
		for(int origin:{0,10}) for(int ticks:{1,2,30,80,100,137,180,190,199,200})
		{
			if(origin+ticks>200)continue;
			auto original=proposal(sourceRate,origin,origin+ticks);
			const auto whole=reference(clip,origin*q,(origin+ticks)*q);
			if(proposalEligibility(original)!="ok"){++rejected;continue;}
			std::vector<EndpointContextProposal> windows{original};
			if(ticks>2)
			{
				windows.push_back(subwindow(original,(origin+1)*q,(origin+ticks)*q)); // left
				windows.push_back(subwindow(original,origin*q,(origin+ticks-1)*q)); // right
				auto trimmed=subwindow(original,(origin+1)*q,(origin+ticks-1)*q);
				const int split=(origin+ticks/2)*q;
				windows.push_back(subwindow(trimmed,trimmed.base.inFrame,split));
				windows.push_back(subwindow(trimmed,split,trimmed.base.outFrame));
			}
			for(auto window:windows)
			{
				auto xml=writeProposal(window);QDomDocument clone;clone.appendChild(clone.importNode(xml,true));
				QDomDocument journal;journal.appendChild(journal.importNode(clone.documentElement(),true));
				auto restored=readProposal(journal.documentElement());QVERIFY(restored);QCOMPARE(restored->referenceEndFrame,original.referenceEndFrame);
				const auto length=window.base.outFrame-window.base.inFrame;
				// Repeated restarts, seek excerpts (including one-frame tail), and loops
				// of the proposed immutable context, against captured native reference.
				for(int restart=0;restart<2;++restart) for(int block:{1,7,32,256})
					for(qint64 at=0;at<length;at+=block)
						for(qint64 k=at;k<std::min(length,at+block);++k)
						{
							auto actual=proposalSample(*restored,clip.sample(),k);QVERIFY(actual);
							const auto expected=whole[window.base.inFrame-original.base.phaseOriginFrame+k];
							QCOMPARE((*actual)[0],expected[0]);QCOMPARE((*actual)[1],expected[1]);
						}
				for(int loop=0;loop<3;++loop)for(qint64 seek:{qint64(0),length/2,length-1})
				{
					auto actual=proposalSample(*restored,clip.sample(),seek);QVERIFY(actual);
					QCOMPARE((*actual)[0],whole[window.base.inFrame-original.base.phaseOriginFrame+seek][0]);
				}
				++checked;
			}
		}
		qInfo()<<"REPRESENTATION_ONLY"<<sourceRate<<"windows"<<checked<<"excluded_references"<<rejected;
		QVERIFY(checked>20);
	}
	void currentTerminalBooleanDoesNotDetermineReferenceEligibility_data(){QTest::addColumn<int>("sourceRate");QTest::newRow("two-continuations")<<44100;}
	void stableNativeReferenceIsChunkInvariant_data(){rates();}
	void stableNativeReferenceIsChunkInvariant()
	{
		using namespace lmms;QFETCH(int,sourceRate);SampleTrack track(Engine::getSong());SampleClip clip(&track);
		auto prepared=AudioImportPolicy::configured().prepare(source);QCOMPARE(clip.adoptAuthorizedAudio(std::move(prepared.audio)),QString("ok"));
		int streams=0;const int q=sourceRate/100;
		for(int origin:{0,10})for(int ticks:{30,80,100,137,190,200})
		{
			if(origin+ticks>200 || (ticks*q)%256==1)continue;
			const auto expected=reference(clip,origin*q,(origin+ticks)*q);
			for(int chunk:{1,7,32,256})
			{
				Sample native(clip.sample().buffer());native.setStartFrame(origin*q);native.setEndFrame((origin+ticks)*q);
				Sample::PlaybackState state;std::vector<SampleFrame> actual(expected.size());
				for(size_t i=0;i<actual.size();i+=chunk)native.play(actual.data()+i,&state,std::min<size_t>(chunk,actual.size()-i));
				for(size_t i=0;i<actual.size();++i){QCOMPARE(actual[i][0],expected[i][0]);QCOMPARE(actual[i][1],expected[i][1]);}
				++streams;
			}
		}
		qInfo()<<"STABLE_NATIVE_CHUNK_REFERENCE"<<sourceRate<<streams;
	}
	void currentTerminalBooleanDoesNotDetermineReferenceEligibility()
	{
		using namespace lmms::test;
		auto a=subwindow(proposal(44100,0,137),0,30*441),b=subwindow(proposal(44100,0,200),0,30*441);
		QVERIFY(a.base.outFrame!=a.referenceEndFrame && b.base.outFrame!=b.referenceEndFrame);
		QCOMPARE(proposalEligibility(a),QString("unrepresentable_audio_boundary"));QCOMPARE(proposalEligibility(b),QString("ok"));
		QVERIFY(readProposal(writeProposal(a))->referenceEndFrame!=readProposal(writeProposal(b))->referenceEndFrame);
	}
	void boundsAndMissingChangedSourcesFailClosed_data(){rates();}
	void boundsAndMissingChangedSourcesFailClosed()
	{
		using namespace lmms::test;QFETCH(int,sourceRate);auto good=proposal(sourceRate,0,100);
		for(const auto& value:QStringList{"-1","0","1","+44100","044100","1e9","9223372036854775808"})
		{auto xml=writeProposal(good);xml.firstChildElement("audio_source_window").setAttribute("reference_end",value);QVERIFY(!readProposal(xml));}
		for(qint64 end:{good.base.outFrame-1,good.base.sourceFrameCount+1})
		{auto bad=good;bad.referenceEndFrame=end;QVERIFY(!readProposal(writeProposal(bad)));}
		for(const auto& code:QStringList{"audio_source_missing","audio_source_changed","import_path_denied"})
		{auto restored=readProposal(writeProposal(good));QVERIFY(restored);QCOMPARE(proposalEligibility(*restored,code),code);QCOMPARE(restored->referenceEndFrame,good.referenceEndFrame);}
		auto one=good;one.base.outFrame=one.base.inFrame+1;QVERIFY(!readProposal(writeProposal(one)));
	}
	void nativeV2AcceptanceStillRequiresImplementation_data()
	{
		QTest::addColumn<int>("sourceRate");QTest::addColumn<QString>("scenario");
		for(auto scenario:QStringList{"original-short","trim-long","trim-left","trim-right","split-after-trim","source-eof","one-frame-tail-seek","clone","save-reload","journal","stop-start","seek","loop","missing-source","changed-source"})
			QTest::newRow(qPrintable(scenario))<<44100<<scenario;
	}
	void nativeV2AcceptanceStillRequiresImplementation()
	{
		using namespace lmms;using namespace lmms::test;QFETCH(QString,scenario);
		auto p=proposal(44100,0,200);
		if(scenario=="original-short")p=proposal(44100,0,137);
		else if(scenario=="trim-long")p=subwindow(p,0,60417);
		else if(scenario=="trim-left")p=subwindow(p,4410,88200);
		else if(scenario=="trim-right")p=subwindow(p,0,44100);
		else if(scenario=="split-after-trim")p=subwindow(p,17640,83790);
		SampleTrack track(Engine::getSong()),target(Engine::getSong());SampleClip legacy(&track);
		auto prepared=AudioImportPolicy::configured().prepare(source);QCOMPARE(legacy.adoptAuthorizedAudio(std::move(prepared.audio)),QString("ok"));
		legacy.setAutoResize(false);legacy.changeLength(p.base.lengthTicks);legacy.setStartTimeOffset(p.base.offsetTicks);
		QDomDocument d;auto node=d.createElement("sampleclip");d.appendChild(node);legacy.saveSettings(d,node);
		node.appendChild(d.importNode(writeProposal(p).firstChildElement("audio_source_window"),true));
		if(scenario=="missing-source")node.setAttribute("src",directory+"/missing.wav");
		if(scenario=="changed-source")node.setAttribute("source_file_id",QString(64,'0'));
		SampleClip actual(&target);actual.loadSettings(node);
		if(scenario=="missing-source" || scenario=="changed-source")
		{
			QCOMPARE(actual.sourceLoadError(),scenario=="missing-source"?QString("audio_source_missing"):QString("audio_source_changed"));
			QDomDocument saved;auto e=saved.createElement("sampleclip");saved.appendChild(e);actual.saveSettings(saved,e);
			QCOMPARE(e.firstChildElement("audio_source_window").attribute("reference_end"),QString::number(p.referenceEndFrame));return;
		}
		std::unique_ptr<SampleClip> copy;
		if(scenario=="clone")
		{copy.reset(actual.cloneToTrack(&target));QVERIFY(copy);QCOMPARE(copy->audioWindowStorage().parseCode,QString("ok"));actual.setMuted(true);}
		else if(scenario=="save-reload")
		{QDomDocument reload;auto e=reload.createElement("sampleclip");reload.appendChild(e);actual.saveSettings(reload,e);actual.loadSettings(e);}
		else if(scenario=="journal")
		{actual.addJournalCheckPoint();actual.movePosition(192);Engine::projectJournal()->undo();}
		if(scenario=="original-short")
		{
			QCOMPARE(actual.audioWindowStatus(),QString("unrepresentable_audio_boundary"));
			QVERIFY(!target.play(actual.startPosition(),1,0,-1));return;
		}
		QCOMPARE(actual.audioWindowStorage().parseCode,QString("ok"));
		auto& playbackClip=copy?*copy:actual;
		QDomDocument persisted;auto persistedNode=persisted.createElement("sampleclip");persisted.appendChild(persistedNode);
		playbackClip.saveSettings(persisted,persistedNode);
		QCOMPARE(persistedNode.firstChildElement("audio_source_window").attribute("reference_end"),QString::number(p.referenceEndFrame));
		// Production-facing assertions deliberately remain behind codec recognition:
		// they must become separate renderer/transport REDs after codec work, not be
		// claimed as passed by the proposal-only oracle above.
		const auto whole=reference(legacy,int(p.base.phaseOriginFrame),int(p.referenceEndFrame));
		legacy.setMuted(true);auto* song=Engine::getSong();auto* engine=Engine::audioEngine();
		const auto period=engine->framesPerPeriod();const qint64 length=p.base.outFrame-p.base.inFrame;
		if(scenario=="one-frame-tail-seek")
		{
			SamplePlayHandle tail(&playbackClip,length-1);
			QCOMPARE(tail.totalFrames(),f_cnt_t(1));
			std::vector<SampleFrame> output(period,SampleFrame{9,9});tail.play(output.data());
			for(int ch=0;ch<2;++ch) QCOMPARE(output[0][ch],whole[p.base.inFrame-p.base.phaseOriginFrame+length-1][ch]);
			for(size_t j=1;j<output.size();++j)for(int ch=0;ch<2;++ch) QCOMPARE(output[j][ch],0.0f);
			return;
		}
		const int seek=scenario=="seek" ? p.base.lengthTicks/2 : 0;
		for(int run=0;run<(scenario=="stop-start" ? 2:1);++run)
		{
			song->stop();engine->clear();engine->renderNextPeriod();engine->renderNextPeriod();
			auto& timeline=song->getTimeline(Song::PlayMode::Song);
			timeline.setLoopPoints(playbackClip.startPosition(),playbackClip.endPosition());timeline.setLoopEnabled(scenario=="loop");
			song->playSong();song->setPlayPos(playbackClip.startPosition().getTicks()+seek);
			const int count=int(((scenario=="loop" ? 3:1)*length+period*3+period-1)/period);
			qint64 mismatch=0;
			for(int part=0;part<count;++part)
			{
				const auto rendered=engine->renderNextPeriod();
				for(size_t j=0;j<rendered.size();++j)
				{
					const qint64 elapsed=qint64(part)*period+j-period;
					qint64 relative=elapsed+qint64(seek)*p.base.framesPerTick;
					SampleFrame expected{};
					if(elapsed>=0)
					{
						if(scenario=="loop") relative%=length;
						if(relative<length)expected=whole[p.base.inFrame-p.base.phaseOriginFrame+relative];
					}
					if(rendered[j][0]!=expected[0] || rendered[j][1]!=expected[1])++mismatch;
				}
			}
			song->stop();timeline.setLoopEnabled(false);engine->renderNextPeriod();engine->renderNextPeriod();
			QCOMPARE(mismatch,qint64(0));
		}
	}
};
QTEST_MAIN(AudioWindowEndpointContextTest)
#include "AudioWindowEndpointContextTest.moc"
