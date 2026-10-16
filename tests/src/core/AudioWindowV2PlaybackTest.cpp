// Native V2 view acceptance versus actual A4-backed legacy references.
// Song scheduling/GUI quarantine have independent gates.
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
#include <future>
#include "AudioImportPolicy.h"
#include "AudioDevice.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "SampleClip.h"
#include "SamplePlayHandle.h"
#include "AudioResampler.h"
#include <samplerate.h>
#include "AudioWindowPlayback.h"
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
class AudioWindowV2PlaybackTest : public QObject
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
	lmms::AudioSourceWindowV2 proposal(int rate,int originTick,int endTick)
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
    void profileIdentity_data(){rates();}
    void profileIdentity()
    {
        using namespace lmms;
        const auto digest=QByteArray::fromHex("4f7f9b2ecea84bb8f5b27c8f36e85e8a100663c745ac25e82a512f485b663ac7");
        QVERIFY(audioWindowReferenceProfileMatches(256,digest));
        QVERIFY(!audioWindowReferenceProfileMatches(128,digest));
        QVERIFY(!audioWindowReferenceProfileMatches(256,QByteArray(32,'x')));
        QVERIFY(!audioWindowReferenceProfileMatches(256,{}));
        auto worker=std::async(std::launch::async,[]{return verifyAudioWindowReferenceProfile();});
        QVERIFY(!worker.get()); // Qualification must not do dependency I/O off the control thread.
    }
    void wholeReference_data(){rates();}
    void wholeReference()
    {
        using namespace lmms; QFETCH(int,sourceRate);
        SampleTrack track(Engine::getSong());SampleClip clip(&track);
        auto prepared=AudioImportPolicy::configured().prepare(source);
        QCOMPARE(clip.adoptAuthorizedAudio(std::move(prepared.audio)),QString("ok"));
        QVERIFY2(verifyAudioWindowReferenceProfile(),"Native reference-profile proof is required");
        auto guard=std::make_shared<AudioWindowPlaybackGuard>(); int checked=0,rejected=0;
        for(int origin:{0,10})for(int ticks:{1,30,80,100,137,199,200})
        {
            if(origin+ticks>200)continue;
            auto w=proposal(sourceRate,origin,origin+ticks);
            auto view=makeAudioWindowPlaybackView(clip.sample().buffer(),w,0,1,guard,1);
            if((w.referenceEndFrame-w.base.phaseOriginFrame)%DEFAULT_BUFFER_SIZE==1)
            {QVERIFY(!view);++rejected;continue;}
            QVERIFY(view);const auto expected=reference(clip,int(w.base.phaseOriginFrame),int(w.referenceEndFrame));
            const auto length=w.base.outFrame-w.base.inFrame;
            std::vector<SampleFrame> actual(size_t(length)+13,SampleFrame{9,9});
            renderAudioWindow(*view,0,actual);
            for(qint64 i=0;i<qint64(actual.size());++i)for(int ch=0;ch<2;++ch)
                QCOMPARE(actual[i][ch],i<length?expected[i][ch]:0.0f);
            ++checked;
        }
        qInfo()<<"V2_NATIVE_WHOLE"<<sourceRate<<"checked"<<checked<<"unsafe_rejected"<<rejected;
    }
    void trimSplitSeekLoopAndBlocks_data(){rates();}
    void trimSplitSeekLoopAndBlocks()
    {
        using namespace lmms;QFETCH(int,sourceRate);
        SampleTrack track(Engine::getSong());SampleClip clip(&track);
        auto prepared=AudioImportPolicy::configured().prepare(source);
        QCOMPARE(clip.adoptAuthorizedAudio(std::move(prepared.audio)),QString("ok"));
        auto guard=std::make_shared<AudioWindowPlaybackGuard>();int checked=0;const int q=sourceRate/100;
        for(int origin:{0,10})
        {
            auto original=proposal(sourceRate,origin,200);
            const auto expected=reference(clip,origin*q,200*q);
            std::vector<AudioSourceWindowV2> windows{original};
            auto add=[&](int start,int end){auto w=original;w.base.inFrame=start*q;w.base.outFrame=end*q;
                w.base.offsetTicks=-start;w.base.lengthTicks=end-start;windows.push_back(w);};
            add(origin+1,200);add(origin,137);add(origin+1,100);add(100,199);
            for(auto w:windows)
            {
                const auto length=w.base.outFrame-w.base.inFrame;
                for(int restart=0;restart<2;++restart)for(qint64 seek:{qint64(0),length/2,length-1,length})
                    for(int block:{1,7,32,64,128,256,512})
                    {
                        auto view=makeAudioWindowPlaybackView(clip.sample().buffer(),w,seek,1,guard,1);QVERIFY(view);
                        QCOMPARE(view->outputFrameCount,length-seek);
                        std::vector<SampleFrame> actual(block);
                        for(qint64 at=0;at<length-seek+block;at+=block)
                        {
                            std::fill(actual.begin(),actual.end(),SampleFrame{9,9});
                            renderAudioWindow(*view,at,actual);
                            for(int j=0;j<block;++j)for(int ch=0;ch<2;++ch)
                            {
                                const auto relative=seek+at+j;
                                const auto value=relative<length?expected[w.base.inFrame-original.base.phaseOriginFrame+relative][ch]:0.0f;
                                QCOMPARE(actual[j][ch],value);
                            }
                        }
                    }
                ++checked;
            }
        }
        qInfo()<<"V2_NATIVE_EXCERPTS"<<sourceRate<<"windows"<<checked;
    }
    void sourceBoundsAndEpoch_data(){rates();}
    void sourceBoundsAndEpoch()
    {
        using namespace lmms;QFETCH(int,sourceRate);SampleTrack track(Engine::getSong());SampleClip clip(&track);
        auto prepared=AudioImportPolicy::configured().prepare(source);QCOMPARE(clip.adoptAuthorizedAudio(std::move(prepared.audio)),QString("ok"));
        auto w=proposal(sourceRate,0,200);auto guard=std::make_shared<AudioWindowPlaybackGuard>();
        auto view=makeAudioWindowPlaybackView(clip.sample().buffer(),w,0,1,guard,1);QVERIFY(view);
        for(int mutation=0;mutation<6;++mutation)
        {
            auto bad=w;
            if(mutation==0)bad.base.inFrame=-1;
            if(mutation==1)bad.base.outFrame=bad.referenceEndFrame+1;
            if(mutation==2)bad.referenceEndFrame=bad.base.sourceFrameCount+1;
            if(mutation==3)bad.base.phaseOriginFrame=1;
            if(mutation==4)bad.base.sourceFrameCount++;
            if(mutation==5)bad.base.sourceRate=22050;
            QVERIFY(!makeAudioWindowPlaybackView(clip.sample().buffer(),bad,0,1,guard,1));
        }
        QVERIFY(!makeAudioWindowPlaybackView({},w,0,1,guard,1));
        QVERIFY(!makeAudioWindowPlaybackView(clip.sample().buffer(),w,-1,1,guard,1));
        QVERIFY(!makeAudioWindowPlaybackView(clip.sample().buffer(),w,w.base.outFrame+1,1,guard,1));
        QVERIFY(!makeAudioWindowPlaybackView(clip.sample().buffer(),w,0,2,guard,1));
        QVERIFY(!makeAudioWindowPlaybackView(clip.sample().buffer(),w,0,1,{},1));
        std::array<SampleFrame,7> actual;guard->epoch.fetch_add(1);
        renderAudioWindow(*view,0,actual);
        for(const auto& frame:actual){QCOMPARE(frame[0],0.0f);QCOMPARE(frame[1],0.0f);}
        QVERIFY(!makeAudioWindowPlaybackView(clip.sample().buffer(),w,0,1,guard,1));
        auto current=makeAudioWindowPlaybackView(clip.sample().buffer(),w,0,1,guard,2);QVERIFY(current);
        w.base.inFrame=-100;QCOMPARE(current->window.base.inFrame,qint64(0));
        renderAudioWindow(*current,-1,actual);
        for(const auto& frame:actual){QCOMPARE(frame[0],0.0f);QCOMPARE(frame[1],0.0f);}
    }
};
QTEST_MAIN(AudioWindowV2PlaybackTest)
#include "AudioWindowV2PlaybackTest.moc"
