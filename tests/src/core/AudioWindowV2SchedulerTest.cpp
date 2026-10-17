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
class AudioWindowV2SchedulerTest : public QObject
{
	Q_OBJECT
	QString directory, source;
	QByteArray digest;
	QStringList old;
	QString oldDeviceBuffer;
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
	void initTestCase()
	{
		using namespace lmms;auto* config=ConfigManager::inst();oldDeviceBuffer=config->value("audioengine","framesperaudiobuffer","scheduler-absent");
		bool ok=false;const auto buffer=qEnvironmentVariableIntValue("LMMS_V2_TEST_BUFFER",&ok);
		if(ok) config->setValue("audioengine","framesperaudiobuffer",QString::number(buffer));
		Engine::init(true);Engine::audioEngine()->audioDev()->stopProcessing();
		qInfo()<<"V2_SCHEDULER_PERIOD"<<Engine::audioEngine()->framesPerPeriod();
	}
	void cleanupTestCase()
	{
		using namespace lmms;auto* config=ConfigManager::inst();
		if(oldDeviceBuffer=="scheduler-absent") config->deleteValue("audioengine","framesperaudiobuffer");
		else config->setValue("audioengine","framesperaudiobuffer",oldDeviceBuffer);
		Engine::destroy();
	}
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
    void nativeWindowPlaybackAndLifecycle_data()
    {
        QTest::addColumn<int>("sourceRate");QTest::addColumn<QString>("scenario");
        for(int rate:{44100,48000})for(const auto& channel:QStringList{"mono","stereo"})
            for(const auto& scenario:QStringList{"original-short","trim-long","trim-left","trim-right","split-after-trim","source-eof","one-frame-tail-seek","clone","save-reload","journal","stop-start","seek","loop","missing-source","changed-source"})
            {
                if(scenario=="original-short" && rate!=44100)continue;
                QTest::newRow(qPrintable(QString::number(rate)+"-"+channel+"-"+scenario))<<rate<<scenario;
            }
    }	void nativeWindowPlaybackAndLifecycle()
	{
		using namespace lmms;using namespace lmms::test;QFETCH(QString,scenario);QFETCH(int,sourceRate);const int q=sourceRate/100;
		auto p=proposal(sourceRate,0,200);
		if(scenario=="original-short")p=proposal(sourceRate,0,137);
		else if(scenario=="trim-long")p=subwindow(p,0,137*q);
		else if(scenario=="trim-left")p=subwindow(p,10*q,200*q);
		else if(scenario=="trim-right")p=subwindow(p,0,100*q);
		else if(scenario=="split-after-trim")p=subwindow(p,40*q,190*q);
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
					if(std::memcmp(&rendered[j],&expected,sizeof(SampleFrame))!=0)++mismatch;
				}
			}
			song->stop();timeline.setLoopEnabled(false);engine->renderNextPeriod();engine->renderNextPeriod();
			QCOMPARE(mismatch,qint64(0));
		}
	}
    void adjacentWindowsRecomposeTrimmedLegacySong_data(){rates();}
    void adjacentWindowsRecomposeTrimmedLegacySong()
    {
        using namespace lmms;QFETCH(int,sourceRate);const int q=sourceRate/100;
        auto* song=Engine::getSong();auto* engine=Engine::audioEngine();const int period=engine->framesPerPeriod();
        SampleTrack legacyTrack(song),versionedTrack(song);SampleClip legacy(&legacyTrack);
        auto prepared=AudioImportPolicy::configured().prepare(source);QCOMPARE(legacy.adoptAuthorizedAudio(std::move(prepared.audio)),QString("ok"));
        legacy.setAutoResize(false);legacy.movePosition(110);legacy.changeLength(80);legacy.setStartTimeOffset(-10);
        const auto capture=[&] {
            song->stop();engine->clear();engine->renderNextPeriod();engine->renderNextPeriod();
            auto& timeline=song->getTimeline(Song::PlayMode::Song);timeline.setLoopEnabled(false);
            song->playSong();song->setPlayPos(110);
            std::vector<SampleFrame> result;
            for(int base=0;base<80*q+3*period;base+=period){const auto block=engine->renderNextPeriod();result.insert(result.end(),block.begin(),block.end());}
            song->stop();return result;
        };
        const auto original=capture();legacyTrack.setMuted(true);
        QDomDocument doc;auto base=doc.createElement("sampleclip");doc.appendChild(base);legacy.saveSettings(doc,base);
        const auto make=[&](int position,int first,int last) {
            auto p=proposal(sourceRate,10,90);p.base.inFrame=first*q;p.base.outFrame=last*q;
            p.base.lengthTicks=last-first;p.base.offsetTicks=-first;
            auto node=base.cloneNode(true).toElement();node.setAttribute("pos",position);node.setAttribute("len",p.base.lengthTicks);node.setAttribute("off",p.base.offsetTicks);
            node.appendChild(doc.importNode(lmms::test::writeProposal(p).firstChildElement("audio_source_window"),true));
            return node;
        };
        SampleClip left(&versionedTrack),right(&versionedTrack);
        left.loadSettings(make(111,11,40));right.loadSettings(make(140,40,89));
        QCOMPARE(left.audioWindowStatus(),QString("ok"));QCOMPARE(right.audioWindowStatus(),QString("ok"));
        const auto split=capture();QCOMPARE(split.size(),original.size());
        for(size_t i=0;i<split.size();++i)
        {
            const auto n=qint64(i)-period;
            const SampleFrame expected=n>=q && n<79*q?original[i]:SampleFrame{};
            QCOMPARE(std::memcmp(&split[i],&expected,sizeof(SampleFrame)),0);
        }
    }
};
QTEST_MAIN(AudioWindowV2SchedulerTest)
#include "AudioWindowV2SchedulerTest.moc"
