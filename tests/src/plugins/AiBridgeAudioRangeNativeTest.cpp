// Phase 3I-B feasibility probe: real Song -> SampleTrack -> SamplePlayHandle
// -> Sample/resampler -> mixer buffers. A failing candidate is NOT supported.
#include <QtTest>
#include <QtEndian>
#include <QCryptographicHash>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>
#include "AudioDevice.h"
#include "AudioEngine.h"
#include "AudioImportPolicy.h"
#include "AutomationClip.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "Timeline.h"

namespace {
// No hardware/thread output: renderNextPeriod is driven synchronously below.
class ProbeDevice final : public lmms::AudioDevice
{
public:
	explicit ProbeDevice(int rate) : AudioDevice(2, lmms::Engine::audioEngine()) { setSampleRate(rate); }
private:
	void startProcessingImpl() override {}
	void stopProcessingImpl() override {}
};
struct Captured
{
	std::vector<lmms::SampleFrame> output;
	QVector<QPair<int,int>> windows;
	bool loaded = false;
};
QDomElement save(lmms::SampleClip& clip)
{
	QDomDocument document;
	auto node = document.createElement("sampleclip"); document.appendChild(node);
	clip.saveSettings(document,node); return node;
}
}

class AiBridgeAudioRangeNativeTest : public QObject
{
	Q_OBJECT
	QString directory;
	QMap<int,QString> sources;
	QMap<int,QByteArray> digests;
	QStringList old;
	const QStringList keys{"audio_import_roots","audio_import_max_file_bytes","audio_import_max_decoded_bytes"};
	QByteArray hash(const QString& path)
	{
		QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {};
		return QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256);
	}
	Captured render(int sourceRate,int outputRate,int bpm,int offset,int boundary,
		bool split,int seek,int mode)
	{
		using namespace lmms;
		auto* song=Engine::getSong(); auto* engine=Engine::audioEngine();
		song->stop(); engine->clear(); engine->renderNextPeriod(); engine->renderNextPeriod();
		engine->setAudioDevice(new ProbeDevice(outputRate),false); song->setTempo(bpm);
		SampleTrack track(song);
		SampleClip left(&track);
		auto prepared=AudioImportPolicy::configured().prepare(sources[sourceRate]);
		Captured result;
		if(prepared.code!="ok" || left.adoptAuthorizedAudio(std::move(prepared.audio))!="ok") return result;
		left.setAutoResize(false); left.movePosition(110); left.changeLength(80); left.setStartTimeOffset(offset);
		std::unique_ptr<SampleClip> right;
		if(split)
		{
			right.reset(left.cloneToTrack(&track));
			right->movePosition(boundary); right->changeLength(190-boundary);
			right->setStartTimeOffset(offset-(boundary-110)); left.changeLength(boundary-110);
		}
		if(mode==1)
		{
			left.loadSettings(save(left)); if(right) right->loadSettings(save(*right));
		}
		if(mode==3)
		{
			auto* automation=AutomationClip::globalAutomationClip(&song->tempoModel());
			automation->putValue(0,bpm,false); automation->putValue(135,bpm+12,false);
		}
		auto& timeline=song->getTimeline(Song::PlayMode::Song);
		timeline.setLoopPoints(110,190); timeline.setLoopEnabled(mode==2);
		song->playSong(); song->setPlayPos(seek,Song::PlayMode::Song);
		const auto periods=int(std::ceil(100*Engine::framesPerTick()/engine->framesPerPeriod()))+6;
		for(int i=0;i<periods*(mode==2?3:1);++i)
		{
			const auto buffer=engine->renderNextPeriod();
			result.output.insert(result.output.end(),buffer.begin(),buffer.end());
		}
		result.windows.push_back({left.sample().startFrame(),left.sample().endFrame()});
		if(right) result.windows.push_back({right->sample().startFrame(),right->sample().endFrame()});
		result.loaded=left.sourceLoadError().isEmpty() && (!right || right->sourceLoadError().isEmpty());
		song->stop(); timeline.setLoopEnabled(false);
		engine->renderNextPeriod(); engine->renderNextPeriod();
		if(mode==3) AutomationClip::globalAutomationClip(&song->tempoModel())->clear();
		return result;
	}
private slots:
	void initTestCase()
	{
		using namespace lmms; Engine::init(true); Engine::audioEngine()->audioDev()->stopProcessing();
		for(const auto& key:keys) old<<ConfigManager::inst()->value("aibridge",key,"probe-absent");
		directory=QCoreApplication::applicationDirPath()+"/phase3ib-range-"+QUuid::createUuid().toString(QUuid::WithoutBraces);
		QVERIFY(QDir().mkpath(directory));
		for(const auto rate:{44100,48000})
		{
			const int frames=rate*2, dataBytes=frames*4;
			QByteArray bytes(44+dataBytes,0);
			std::memcpy(bytes.data(),"RIFF",4); std::memcpy(bytes.data()+8,"WAVEfmt ",8); std::memcpy(bytes.data()+36,"data",4);
			qToLittleEndian<quint32>(36+dataBytes,bytes.data()+4); qToLittleEndian<quint32>(16,bytes.data()+16);
			qToLittleEndian<quint16>(1,bytes.data()+20); qToLittleEndian<quint16>(2,bytes.data()+22);
			qToLittleEndian<quint32>(rate,bytes.data()+24); qToLittleEndian<quint32>(rate*4,bytes.data()+28);
			qToLittleEndian<quint16>(4,bytes.data()+32); qToLittleEndian<quint16>(16,bytes.data()+34);
			qToLittleEndian<quint32>(dataBytes,bytes.data()+40);
			for(int frame=0;frame<frames;++frame)
			{
				// Transient-rich, deterministic, independent channels; no silent seam mask.
				qToLittleEndian<qint16>(qint16((frame*7919u+1237u)%30001u-15000),bytes.data()+44+frame*4);
				qToLittleEndian<qint16>(qint16((frame*3571u+2347u)%29009u-14504),bytes.data()+46+frame*4);
			}
			const auto path=directory+QString("/stereo-%1.wav").arg(rate);
			QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::NewOnly));
			QCOMPARE(file.write(bytes),qint64(bytes.size())); file.close(); sources[rate]=path; digests[rate]=hash(path);
			qInfo().noquote()<<"FIXTURE"<<path<<digests[rate].toHex();
		}
		ConfigManager::inst()->setValue("aibridge",keys[0],QString::fromUtf8(QJsonDocument(QJsonArray{directory}).toJson(QJsonDocument::Compact)));
		ConfigManager::inst()->setValue("aibridge",keys[1],"1000000");
		ConfigManager::inst()->setValue("aibridge",keys[2],"2000000");
	}
	void cleanupTestCase()
	{
		using namespace lmms;
		for(const auto rate:{44100,48000}) QCOMPARE(hash(sources[rate]),digests[rate]);
		for(int i=0;i<keys.size();++i)
		{
			if(old[i]=="probe-absent") ConfigManager::inst()->deleteValue("aibridge",keys[i]);
			else ConfigManager::inst()->setValue("aibridge",keys[i],old[i]);
		}
		AudioImportPolicy::configured().refresh(); Engine::destroy();
	}
	void candidateSplitContinuity_data()
	{
		QTest::addColumn<int>("sourceRate"); QTest::addColumn<int>("outputRate"); QTest::addColumn<int>("bpm");
		QTest::addColumn<int>("offset"); QTest::addColumn<int>("boundary"); QTest::addColumn<int>("seek"); QTest::addColumn<int>("mode");
		for(int source:{44100,48000}) for(int output:{44100,48000}) for(int tempo:{125,123})
			for(int off:{0,-10}) for(int boundary:{111,140,189})
		{
			const auto name=QString("%1-%2-bpm%3-off%4-at%5").arg(source).arg(output).arg(tempo).arg(off).arg(boundary).toLatin1();
			QTest::newRow(name.constData())<<source<<output<<tempo<<off<<boundary<<110<<0;
		}
		for(int mode:{0,1,2,3}) for(int seek:{120,140,150})
		{
			const auto name=QString("equal44100-mode%1-seek%2").arg(mode).arg(seek).toLatin1();
			QTest::newRow(name.constData())<<44100<<44100<<125<<-10<<140<<seek<<mode;
		}
	}
	void candidateSplitContinuity()
	{
		QFETCH(int,sourceRate); QFETCH(int,outputRate); QFETCH(int,bpm); QFETCH(int,offset);
		QFETCH(int,boundary); QFETCH(int,seek); QFETCH(int,mode);
		const auto whole=render(sourceRate,outputRate,bpm,offset,boundary,false,seek,mode);
		const auto halves=render(sourceRate,outputRate,bpm,offset,boundary,true,seek,mode);
		QVERIFY(whole.loaded && halves.loaded); QCOMPARE(whole.output.size(),halves.output.size());
		if(mode==0 && seek==110)
		{
			QCOMPARE(halves.windows[0].first,whole.windows[0].first);
			QCOMPARE(halves.windows[0].second,halves.windows[1].first);
			QCOMPARE(halves.windows[1].second,whole.windows[0].second);
			qInfo()<<"SOURCE_PARTITION_EXACT";
		}
		qint64 mismatches=0,first=-1; int nonzero=0;
		for(size_t i=0;i<whole.output.size();++i)
		{
			if(whole.output[i][0]!=0 || whole.output[i][1]!=0) ++nonzero;
			if(whole.output[i][0]!=halves.output[i][0] || whole.output[i][1]!=halves.output[i][1])
			{ if(first<0) first=i; ++mismatches; }
		}
		qInfo()<<"BUFFER_PROBE"<<"nonzero"<<nonzero<<"first_mismatch"<<first<<"mismatches"<<mismatches
			<<"whole_window"<<whole.windows<<"split_windows"<<halves.windows;
		if(first>=0) qInfo()<<"FIRST_VALUES"<<whole.output[first][0]<<whole.output[first][1]<<halves.output[first][0]<<halves.output[first][1];
		QVERIFY2(nonzero>100,"Probe must render actual non-silent Song output");
		QCOMPARE(mismatches,qint64(0));
	}
	void trimmedWindowSurvivesLaterTempoChange()
	{
		using namespace lmms; auto* song=Engine::getSong();
		Engine::audioEngine()->setAudioDevice(new ProbeDevice(44100),false); song->setTempo(125);
		SampleTrack track(song); SampleClip clip(&track);
		auto prepared=AudioImportPolicy::configured().prepare(sources[44100]); QCOMPARE(prepared.code,QString("ok"));
		QCOMPARE(clip.adoptAuthorizedAudio(std::move(prepared.audio)),QString("ok"));
		clip.setAutoResize(false); clip.movePosition(110); clip.changeLength(80); clip.setStartTimeOffset(-10);
		const auto before=save(clip); song->setTempo(123); const auto after=save(clip);
		// 44,100 * 60 / (48 * 125) = 441 exactly: [4410,39690).
		QVERIFY(track.play(110,1,0,-1)); Engine::audioEngine()->renderNextPeriod();
		const int in=clip.sample().startFrame();
		const int out=clip.sample().endFrame();
		qInfo()<<"TEMPO_RANGE"<<"before"<<4410<<39690<<"after"<<in<<out<<"offset"<<after.attribute("off")
			<<"before_offset"<<before.attribute("off");
		Engine::audioEngine()->clear(); Engine::audioEngine()->renderNextPeriod();
		QCOMPARE(in,4410); QCOMPARE(out,39690);
	}
	void independentEqualRatePlaybackStatesPreserveSeam()
	{
		using namespace lmms;
		Engine::audioEngine()->setAudioDevice(new ProbeDevice(44100),false);
		std::vector<SampleFrame> data(64);
		for(int i=0;i<64;++i) data[i]=SampleFrame{float(i+1)/128,float(64-i)/128};
		const auto buffer=std::make_shared<const SampleBuffer>(data,44100);
		Sample whole(buffer),left(buffer),right(buffer);
		left.setEndFrame(32); right.setStartFrame(32);
		Sample::PlaybackState one,a,b;
		std::vector<SampleFrame> original(64),joined(64);
		QVERIFY(whole.play(original.data(),&one,64));
		QVERIFY(left.play(joined.data(),&a,32)); QVERIFY(right.play(joined.data()+32,&b,32));
		qInfo()<<"STATE_RESET_SEAM"<<"source31"<<data[31][0]<<"source32"<<data[32][0]
			<<"unsplit32"<<original[32][0]<<"split32"<<joined[32][0]
			<<"unsplit33"<<original[33][0]<<"split33"<<joined[33][0];
		QCOMPARE(joined[32][0],original[32][0]);
	}
};
QTEST_MAIN(AiBridgeAudioRangeNativeTest)
#include "AiBridgeAudioRangeNativeTest.moc"
