// Task1 prerequisite RED: approved phase law versus the real A4-backed legacy
// reference. No new-mode renderer is implemented. Passing rows characterize
// the reference; they are not functional new-mode acceptance.
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

class WindowProbeDevice final : public lmms::AudioDevice
{
public:
    explicit WindowProbeDevice(int rate):AudioDevice(2,lmms::Engine::audioEngine()){setSampleRate(rate);}
private:
    void startProcessingImpl() override {}
    void stopProcessingImpl() override {}
};
class AudioWindowPlaybackTest : public QObject
{
	Q_OBJECT
	QString directory, source;
	QByteArray digest;
	QStringList old;
	const QStringList keys{"audio_import_roots","audio_import_max_file_bytes","audio_import_max_decoded_bytes"};
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
		const int sourceFrames=2*sourceRate; QByteArray bytes(44+sourceFrames*4,0); std::memcpy(bytes.data(),"RIFF",4); std::memcpy(bytes.data()+8,"WAVEfmt ",8); std::memcpy(bytes.data()+36,"data",4);
		qToLittleEndian<quint32>(36+sourceFrames*4,bytes.data()+4); qToLittleEndian<quint32>(16,bytes.data()+16);
		qToLittleEndian<quint16>(1,bytes.data()+20); qToLittleEndian<quint16>(2,bytes.data()+22);
		qToLittleEndian<quint32>(sourceRate,bytes.data()+24); qToLittleEndian<quint32>(sourceRate*4,bytes.data()+28);
		qToLittleEndian<quint16>(4,bytes.data()+32); qToLittleEndian<quint16>(16,bytes.data()+34); qToLittleEndian<quint32>(sourceFrames*4,bytes.data()+40);
		for(int i=0;i<sourceFrames;++i) { qToLittleEndian<qint16>((i*7919u)%30001u-15000,bytes.data()+44+i*4); qToLittleEndian<qint16>((i*3571u)%29009u-14504,bytes.data()+46+i*4); }
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
	void approvedPhaseMustMatchWholeLegacy_data()
    {
        QTest::addColumn<int>("sourceRate"); QTest::addColumn<int>("ticks");
        for(int rate:{44100,48000}) for(int length:{1,30,80,100,137,199,200})
            QTest::newRow(qPrintable(QString("%1-ticks%2").arg(rate).arg(length)))<<rate<<length;
    }
    void approvedPhaseMustMatchWholeLegacy()
    {
        using namespace lmms; QFETCH(int,sourceRate); QFETCH(int,ticks);
        SampleTrack track(Engine::getSong()); SampleClip legacy(&track);
        auto prepared=AudioImportPolicy::configured().prepare(source); QCOMPARE(prepared.code,QString("ok"));
        QCOMPARE(legacy.adoptAuthorizedAudio(std::move(prepared.audio)),QString("ok"));
        const int q=sourceRate/100, frames=ticks*q, period=Engine::audioEngine()->framesPerPeriod();
        const AudioSourceWindowV1 proposed{0,frames,0,sourceRate*2,sourceRate,125,sourceRate,48,q,ticks,0};
        QCOMPARE(validateAudioWindowRecord(proposed),QString("ok"));
        legacy.setAutoResize(false); legacy.changeLength(ticks); legacy.setStartTimeOffset(0);
        legacy.setSampleStartFrame(0); legacy.setSamplePlayLength(frames);
        SamplePlayHandle handle(&legacy); std::vector<SampleFrame> buffer(period);
        int mismatch=0,first=-1;
        float lastActual=0,lastExpected=0;
        for(int base=0;base<frames+period*2;base+=period)
        {
            handle.play(buffer.data());
            for(int j=0;j<period;++j)
            {
                const int n=base+j;
                const auto expected=n<frames ? legacy.sample().data()[std::max(0,n-1)] : SampleFrame{};
                if(buffer[j][0]!=expected[0] || buffer[j][1]!=expected[1]) {if(first<0)first=n;++mismatch;}
                if(n==frames-1) {lastActual=buffer[j][0];lastExpected=expected[0];}
            }
        }
        qInfo()<<"APPROVED_PHASE_GATE"<<sourceRate<<ticks<<"frames"<<frames<<"native_input_chunk"<<DEFAULT_BUFFER_SIZE
            <<"first"<<first<<"mismatch"<<mismatch<<"last_actual"<<lastActual<<"last_phase_law"<<lastExpected;
        QCOMPARE(mismatch,0);
    }
	void identicalWindowsMustRetainBothLegacyReferences_data()
	{
		QTest::addColumn<int>("sourceRate"); QTest::newRow("44100-terminal-history")<<44100;
	}
	void identicalWindowsMustRetainBothLegacyReferences()
	{
		using namespace lmms; QFETCH(int,sourceRate);
		SampleTrack track(Engine::getSong()); SampleClip legacy(&track);
		auto prepared=AudioImportPolicy::configured().prepare(source); QCOMPARE(prepared.code,QString("ok"));
		QCOMPARE(legacy.adoptAuthorizedAudio(std::move(prepared.audio)),QString("ok"));
		const int period=Engine::audioEngine()->framesPerPeriod(),q=sourceRate/100;
		auto reference=[&](int ticks) {
			legacy.setAutoResize(false); legacy.changeLength(ticks); legacy.setStartTimeOffset(0);
			legacy.setSampleStartFrame(0); legacy.setSamplePlayLength(ticks*q);
			SamplePlayHandle handle(&legacy); std::vector<SampleFrame> output,block(period);
			for(int base=0;base<ticks*q;base+=period) {handle.play(block.data());output.insert(output.end(),block.begin(),block.end());}
			output.resize(ticks*q); return output;
		};
		const auto shortStream=reference(137),longStream=reference(200);
		const AudioSourceWindowV1 convertedShort{0,137*q,0,2*sourceRate,sourceRate,125,sourceRate,48,q,137,0};
		// Spec right trim of the 200-tick original has these SAME thirteen fields.
		AudioSourceWindowV1 trimmedLong{0,200*q,0,2*sourceRate,sourceRate,125,sourceRate,48,q,200,0};
		trimmedLong.outFrame=137*q; trimmedLong.lengthTicks=137;
		QCOMPARE(validateAudioWindowRecord(convertedShort),QString("ok"));
		QCOMPARE(validateAudioWindowRecord(trimmedLong),QString("ok"));
		AudioWindowStorage a,b; a.kind=b.kind=AudioWindowStorageKind::V1; a.value=convertedShort;b.value=trimmedLong;
		QDomDocument da,db; auto na=da.createElement("sampleclip"),nb=db.createElement("sampleclip"); da.appendChild(na);db.appendChild(nb);
		writeAudioWindow(a,da,na);writeAudioWindow(b,db,nb); QCOMPARE(da.toString(),db.toString());
		const int last=137*q-1;
		qInfo().noquote()<<"IDENTICAL_SERIALIZED_WINDOWS_DIFFERENT_REFERENCES"<<"frame"<<last
			<<"converted_short"<<QString::number(shortStream[last][0],'g',17)
			<<"trimmed_long_excerpt"<<QString::number(longStream[last][0],'g',17);
		QCOMPARE(shortStream[last][0],longStream[last][0]);
	}
};
QTEST_MAIN(AudioWindowPlaybackTest)
#include "AudioWindowPlaybackTest.moc"
