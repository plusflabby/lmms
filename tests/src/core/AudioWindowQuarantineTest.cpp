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
#include "SampleTrack.h"
#include "Song.h"
#include "ProjectJournal.h"
#include "AutomationTrack.h"
#include "AutomationClip.h"
#include "ControllerConnection.h"

class AudioWindowQuarantineTest : public QObject
{
	Q_OBJECT
	QString directory, source;
	QByteArray digest;
	QStringList old;
	const QStringList keys{"audio_import_roots","audio_import_max_file_bytes","audio_import_max_decoded_bytes"};
	QDomElement save(lmms::SampleClip& clip)
	{
		QDomDocument doc; auto element=doc.createElement("sampleclip"); doc.appendChild(element);
		clip.saveSettings(doc,element); return element;
	}
	QDomElement window()
	{
		QDomDocument doc;
		doc.setContent(QStringLiteral("<audio_source_window version='2' phase='linear_equal_ref256_v2' reference_end='88200' in='4410' out='39690' origin='4410' source_frames='88200' source_rate='44100' reference_bpm='125' reference_output_rate='44100' ticks_per_beat='48' frames_per_tick='441' length_ticks='80' offset_ticks='-10'/>"));
		return doc.documentElement();
	}
	QDomElement state()
	{
		lmms::SampleTrack track(lmms::Engine::getSong()); lmms::SampleClip clip(&track);
		auto prepared=lmms::AudioImportPolicy::configured().prepare(source);
		if(prepared.code!="ok" || clip.adoptAuthorizedAudio(std::move(prepared.audio))!="ok") return {};
		clip.setAutoResize(false); clip.movePosition(110); clip.changeLength(80); clip.setStartTimeOffset(-10);
		auto node=save(clip); node.appendChild(node.ownerDocument().importNode(window(),true)); return node;
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
		using namespace lmms; Engine::getSong()->setTempo(125);
		for(const auto& key:keys) old<<ConfigManager::inst()->value("aibridge",key,"window-test-absent");
		directory=QCoreApplication::applicationDirPath()+"/window-persistence-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
		QVERIFY(QDir().mkpath(directory)); source=directory+"/original.wav";
		QByteArray bytes(352844,0); std::memcpy(bytes.data(),"RIFF",4); std::memcpy(bytes.data()+8,"WAVEfmt ",8); std::memcpy(bytes.data()+36,"data",4);
		qToLittleEndian<quint32>(352836,bytes.data()+4); qToLittleEndian<quint32>(16,bytes.data()+16);
		qToLittleEndian<quint16>(1,bytes.data()+20); qToLittleEndian<quint16>(2,bytes.data()+22);
		qToLittleEndian<quint32>(44100,bytes.data()+24); qToLittleEndian<quint32>(176400,bytes.data()+28);
		qToLittleEndian<quint16>(4,bytes.data()+32); qToLittleEndian<quint16>(16,bytes.data()+34); qToLittleEndian<quint32>(352800,bytes.data()+40);
		for(int i=0;i<88200;++i) { qToLittleEndian<qint16>((i*7919u)%30001u-15000,bytes.data()+44+i*4); qToLittleEndian<qint16>((i*3571u)%29009u-14504,bytes.data()+46+i*4); }
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
    void nativeHandleUsesAuthorizedWindow()
    {
        using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);clip.loadSettings(state());
        QCOMPARE(clip.audioWindowStatus(),QString("ok"));
        SamplePlayHandle handle(&clip);QCOMPARE(handle.totalFrames(),f_cnt_t(35280));
        const int period=Engine::audioEngine()->framesPerPeriod();std::vector<SampleFrame> output(period);
        for(int base=0;base<35280+period;base+=period){handle.play(output.data());for(int j=0;j<period;++j)for(int ch=0;ch<2;++ch)
            QCOMPARE(output[j][ch],base+j<35280?clip.sample().data()[std::max(4410,4410+base+j-1)][ch]:0.0f);}
    }
    void tempoQuarantineInvalidatesExistingHandleAndPreservesReload()
    {
        using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);clip.loadSettings(state());
        QCOMPARE(clip.audioWindowStatus(),QString("ok"));SamplePlayHandle oldHandle(&clip);
        Engine::getSong()->setTempo(123);QCOMPARE(clip.audioWindowStatus(),QString("unsupported_tempo_map"));
        std::vector<SampleFrame> output(Engine::audioEngine()->framesPerPeriod(),SampleFrame{9,9});oldHandle.play(output.data());
        for(const auto& frame:output){QCOMPARE(frame[0],0.0f);QCOMPARE(frame[1],0.0f);}
        QVERIFY(oldHandle.isFinished());
        auto saved=save(clip);QCOMPARE(saved.firstChildElement("audio_source_window").attribute("reference_end"),QString("88200"));
        clip.loadSettings(saved);Engine::getSong()->setTempo(130);QCOMPARE(clip.audioWindowStatus(),QString("unsupported_tempo_map"));
        Engine::getSong()->setTempo(125);QCOMPARE(clip.audioWindowStatus(),QString("ok"));
        oldHandle.play(output.data());for(const auto& frame:output){QCOMPARE(frame[0],0.0f);QCOMPARE(frame[1],0.0f);}
    }
    void genericEditsCannotRewriteVersionedWindow()
    {
        using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);clip.loadSettings(state());
        clip.changeLength(20);QCOMPARE(clip.length().getTicks(),80);
        clip.setStartTimeOffset(-20);QCOMPARE(clip.startTimeOffset().getTicks(),-10);
        Clip* generic=&clip;generic->setAutoResize(true);QVERIFY(!clip.getAutoResize());
        generic->setName("unsupported");QVERIFY(clip.name().isEmpty());
        generic->setSolo(true);QVERIFY(!clip.isSolo());
        clip.sample().setAmplification(2);QCOMPARE(clip.sample().amplification(),1.0f);
        clip.sample().setFrequency(123);QCOMPARE(clip.sample().frequency(),float(DefaultBaseFreq));
        clip.sample().setReversed(true);QVERIFY(!clip.sample().reversed());
        const auto oldBuffer=clip.sample().buffer();clip.sample()=Sample{};QCOMPARE(clip.sample().buffer(),oldBuffer);
        QCOMPARE(save(clip).firstChildElement("audio_source_window").attribute("reference_end"),QString("88200"));
    }
    void unsafeOriginalReferenceIsObservable()
    {
        using namespace lmms;auto node=state();auto range=node.firstChildElement("audio_source_window");
        range.setAttribute("origin","0");range.setAttribute("reference_end","60417");
        SampleTrack track(Engine::getSong());SampleClip clip(&track);clip.loadSettings(node);
        QCOMPARE(clip.audioWindowStatus(),QString("unrepresentable_audio_boundary"));
        QVERIFY(!track.play(110,1,0,-1));SamplePlayHandle handle(&clip);QCOMPARE(handle.totalFrames(),f_cnt_t(0));
    }
    void strictCopiedEligibilityRejectsUnpersistableColor()
    {
        using namespace lmms;const auto storage=parseAudioWindow(state());
        AudioWindowGeometry geometry{110,80,-10,false,false,false,{},"#ffwrong",DefaultBaseFreq,1};
        AudioWindowWorld world{125,44100,48,false};SampleDecoder::PcmWaveInfo info;
        info.frames=88200;info.sampleRate=44100;info.channels=2;
        QCOMPARE(evaluateAudioWindow(storage,geometry,world,"ok",info).code,QString("unsupported_audio_operation"));
    }
    void outputRateQuarantinePreservesSourceWindow()
    {
        using namespace lmms;
        class Device final: public AudioDevice { public: explicit Device(int rate):AudioDevice(2,Engine::audioEngine()){setSampleRate(rate);} private:void startProcessingImpl()override{} void stopProcessingImpl()override{} };
        auto* engine=Engine::audioEngine();engine->setAudioDevice(new Device(44100),false);
        SampleTrack track(Engine::getSong());SampleClip clip(&track);clip.loadSettings(state());
        QCOMPARE(clip.audioWindowStatus(),QString("ok"));SamplePlayHandle oldHandle(&clip);
        engine->setAudioDevice(new Device(48000),false);
        QCOMPARE(clip.audioWindowStatus(),QString("unsupported_sample_rate_mapping"));QVERIFY(oldHandle.isFinished());
        QCOMPARE(save(clip).firstChildElement("audio_source_window").attribute("reference_end"),QString("88200"));
        engine->setAudioDevice(new Device(44100),false);QCOMPARE(clip.audioWindowStatus(),QString("ok"));
        SamplePlayHandle restored(&clip);QCOMPARE(restored.totalFrames(),f_cnt_t(35280));
    }
    void constantTempoAutomationInvalidatesPublishedView()
    {
        using namespace lmms;auto* song=Engine::getSong();SampleTrack track(song);SampleClip clip(&track);clip.loadSettings(state());
        QCOMPARE(clip.audioWindowStatus(),QString("ok"));QVERIFY(clip.audioWindowPlaybackView());SamplePlayHandle oldHandle(&clip);
        AutomationTrack automation(song);
        {
            AutomationClip tempo(&automation);QVERIFY(tempo.addObject(&song->tempoModel()));
            tempo.putValue(0,125);tempo.putValue(480,125);
            QCOMPARE(clip.audioWindowStatus(),QString("unsupported_tempo_map"));
            QVERIFY(!clip.audioWindowPlaybackView());QVERIFY(oldHandle.isFinished());
        }
        QCoreApplication::processEvents();
        QCOMPARE(clip.audioWindowStatus(),QString("ok"));QVERIFY(clip.audioWindowPlaybackView());
    }
    void tempoControllerLifecycleInvalidatesPublishedView()
    {
        using namespace lmms;auto* song=Engine::getSong();SampleTrack track(song);SampleClip clip(&track);clip.loadSettings(state());
        SamplePlayHandle oldHandle(&clip);QVERIFY(clip.audioWindowPlaybackView());
        ControllerConnection controller(nullptr);song->tempoModel().setControllerConnection(&controller);
        QCOMPARE(clip.audioWindowStatus(),QString("unsupported_tempo_map"));QVERIFY(!clip.audioWindowPlaybackView());QVERIFY(oldHandle.isFinished());
        song->tempoModel().unlinkControllerConnection();QCoreApplication::processEvents();
        QCOMPARE(clip.audioWindowStatus(),QString("ok"));QVERIFY(clip.audioWindowPlaybackView());
    }
    void cloneGuardsAndDeletedClipLifetimeAreIndependent()
    {
        using namespace lmms;SampleTrack track(Engine::getSong()),target(Engine::getSong());
        auto original=std::make_unique<SampleClip>(&track);original->loadSettings(state());
        std::unique_ptr<SampleClip> clone(original->cloneToTrack(&target));QVERIFY(clone);
        SamplePlayHandle before(original.get()),copy(clone.get());
        original->movePosition(192);QVERIFY(before.isFinished());QVERIFY(!copy.isFinished());
        SamplePlayHandle moved(original.get());original.reset();QVERIFY(moved.isFinished());
        std::vector<SampleFrame> output(Engine::audioEngine()->framesPerPeriod(),SampleFrame{9,9});moved.play(output.data());
        for(const auto& frame:output){QCOMPARE(frame[0],0.0f);QCOMPARE(frame[1],0.0f);}
        copy.play(output.data());QCOMPARE(output[0][0],clone->sample().data()[4410][0]);
    }
    void clonedWindowObservesTickBasisChanges()
    {
        using namespace lmms;auto* song=Engine::getSong();SampleTrack track(song),target(song);SampleClip original(&track);original.loadSettings(state());
        std::unique_ptr<SampleClip> clone(original.cloneToTrack(&target));QVERIFY(clone);SamplePlayHandle handle(clone.get());
        struct Restore { MeterModel& model;int value;~Restore(){model.setDenominator(value);} } restore{song->getTimeSigModel(),song->getTimeSigModel().getDenominator()};
        song->getTimeSigModel().setDenominator(8);
        QCOMPARE(clone->audioWindowStatus(),QString("unsupported_sample_rate_mapping"));
        QVERIFY(handle.isFinished());QVERIFY(!clone->audioWindowPlaybackView());
    }
    void rawLegacySamplePlaybackCannotBypassWindow()
    {
        using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);clip.loadSettings(state());
        std::vector<SampleFrame> output(Engine::audioEngine()->framesPerPeriod(),SampleFrame{9,9});Sample::PlaybackState legacy;
        QVERIFY(!clip.sample().play(output.data(),&legacy,output.size()));
        for(const auto& frame:output){QCOMPARE(frame[0],0.0f);QCOMPARE(frame[1],0.0f);}
        QCOMPARE(clip.audioWindowStatus(),QString("ok")); // Clip-aware V2 playback remains eligible.
    }
};
QTEST_MAIN(AudioWindowQuarantineTest)
#include "AudioWindowQuarantineTest.moc"
