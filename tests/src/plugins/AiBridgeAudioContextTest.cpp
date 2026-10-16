#include <QtTest>
#include <cstring>
#include <thread>
#include <limits>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QtEndian>
#include <QDomDocument>
#include "AiBridgeAudioContext.h"
#include "AudioImportPolicy.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "InstrumentTrack.h"
#include "Song.h"
class AiBridgeAudioContextTest : public QObject
{
	Q_OBJECT
	QStringList old;
	const QStringList keys{"audio_import_roots","audio_import_max_file_bytes","audio_import_max_decoded_bytes"};
	std::unique_ptr<QTemporaryDir> dir;
	QString source;
	QByteArray bytes;
	void adopt(lmms::SampleClip& clip)
	{
		auto r=lmms::AudioImportPolicy::configured().prepare(source); QCOMPARE(r.code,QStringLiteral("ok"));
		QCOMPARE(clip.adoptAuthorizedAudio(std::move(r.audio)),QStringLiteral("ok"));
	}
private slots:
	void initTestCase() { lmms::Engine::init(true); }
	void cleanupTestCase() { lmms::Engine::destroy(); }
	void init()
	{
		for(auto& key:keys) old<<lmms::ConfigManager::inst()->value("aibridge",key,"test-absent");
		dir=std::make_unique<QTemporaryDir>(QCoreApplication::applicationDirPath()+"/audio-context-XXXXXX"); QVERIFY(dir->isValid());
		source=dir->filePath("disposable.wav"); bytes=QByteArray(44+128,0);
		std::memcpy(bytes.data(),"RIFF",4);std::memcpy(bytes.data()+8,"WAVEfmt ",8);std::memcpy(bytes.data()+36,"data",4);
		qToLittleEndian<quint32>(164,bytes.data()+4);qToLittleEndian<quint32>(16,bytes.data()+16);
		qToLittleEndian<quint16>(1,bytes.data()+20);qToLittleEndian<quint16>(1,bytes.data()+22);
		qToLittleEndian<quint32>(44100,bytes.data()+24);qToLittleEndian<quint32>(88200,bytes.data()+28);
		qToLittleEndian<quint16>(2,bytes.data()+32);qToLittleEndian<quint16>(16,bytes.data()+34);qToLittleEndian<quint32>(128,bytes.data()+40);
		{QFile f(source);QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::NewOnly));QCOMPARE(f.write(bytes),qint64(bytes.size()));}
		auto c=lmms::ConfigManager::inst(); c->setValue("aibridge",keys[0],QString::fromUtf8(QJsonDocument(QJsonArray{dir->path()}).toJson(QJsonDocument::Compact)));
		c->setValue("aibridge",keys[1],"20000");c->setValue("aibridge",keys[2],"20000");
	}
	void cleanup()
	{
		for(int i=0;i<keys.size();++i){if(old[i]=="test-absent")lmms::ConfigManager::inst()->deleteValue("aibridge",keys[i]);else lmms::ConfigManager::inst()->setValue("aibridge",keys[i],old[i]);}
		old.clear();lmms::AudioImportPolicy::configured().refresh();dir.reset();
	}
	void copiesNativeStateAndMetadataWithoutPathsOrPlaybackCursors()
	{
		using namespace lmms;
		SampleTrack track(Engine::getSong()); SampleClip clip(&track); adopt(clip);
		QDomDocument d;auto settings=d.createElement("sampletrack"); settings.setAttribute("vol",37);settings.setAttribute("pan",-12);track.loadTrackSpecificSettings(settings);
		clip.setAutoResize(false);clip.movePosition(300);clip.changeLength(40);clip.setStartTimeOffset(-10);
		AiBridgeProjectContext ids; const auto id=ids.idForClip(&clip); AiBridgeAudioContext context;
		auto result=context.clip(id);QCOMPARE(result.code,QStringLiteral("ok"));QVERIFY(result.value);
		const auto copied=*result.value;QCOMPARE(copied.state.id,id);QCOMPARE(copied.state.trackId,ids.idForTrack(&track));
		QCOMPARE(copied.state.position,qint64(300));QCOMPARE(copied.state.length,qint64(40));QCOMPARE(copied.state.offset,qint64(-10));QCOMPARE(copied.endTick,qint64(340));
		QCOMPARE(copied.state.sourceFrames,quint64(64));QCOMPARE(copied.state.sampleRate,44100);QVERIFY(copied.state.sourceChannels);QCOMPARE(*copied.state.sourceChannels,1);
		QCOMPARE(copied.state.trackVolume,37.0);QCOMPARE(copied.state.trackPanning,-12.0);QVERIFY(copied.sourceVerified);
		QCOMPARE(copied.stateHash,audioStateHash({copied.state}));QCOMPARE(context.validate(id,copied.stateHash),QStringLiteral("ok"));
		clip.sample().setStartFrame(17);clip.sample().setEndFrame(31);
		QCOMPARE(context.clip(id).value->stateHash,copied.stateHash);
		const auto wire=QJsonDocument(AiBridgeAudioContext::toJson(copied)).toJson(QJsonDocument::Compact);
		QVERIFY(!wire.contains(dir->path().toUtf8()));QVERIFY(!wire.contains("source_digest"));QVERIFY(!wire.contains("source_identity"));QVERIFY(!wire.contains("start_frame"));
		QCOMPARE(AiBridgeAudioContext::toJson(copied).value("source_label").toString(),QStringLiteral("disposable.wav"));
		QFile f(source);QVERIFY(f.open(QIODevice::ReadOnly));QCOMPARE(f.readAll(),bytes);
	}
	void detectsRealSourceRemovalAndReplacementWithoutMutatingTheClip()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);adopt(clip);
		AiBridgeProjectContext ids;AiBridgeAudioContext context;auto id=ids.idForClip(&clip);auto initial=context.clip(id);
		QCOMPARE(initial.code,QStringLiteral("ok"));QVERIFY(QFile::rename(source,dir->filePath("held-original.wav")));
		auto missing=context.clip(id);QCOMPARE(missing.code,QStringLiteral("ok"));QCOMPARE(missing.value->state.sourceStatus,QStringLiteral("audio_source_missing"));
		QCOMPARE(clip.sample().sampleSize(),size_t(64));QCOMPARE(clip.sampleFile(),source);QVERIFY(missing.value->stateHash!=initial.value->stateHash);
		{QFile f(source);QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::NewOnly));f.write(bytes);}
		auto changed=context.clip(id);QCOMPARE(changed.value->state.sourceStatus,QStringLiteral("audio_source_changed"));
		QCOMPARE(context.validate(id,initial.value->stateHash),QStringLiteral("clip_state_mismatch"));
	}
	void independentlyRejectsStalePlacementAndPropertyHashes()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);adopt(clip);
		AiBridgeProjectContext ids;AiBridgeAudioContext context;auto id=ids.idForClip(&clip);auto old=context.clip(id);
		QCOMPARE(old.code,QStringLiteral("ok"));clip.movePosition(400);
		QCOMPARE(context.validate(id,old.value->stateHash),QStringLiteral("clip_state_mismatch"));
		auto current=context.clip(id);clip.setMuted(true);
		QCOMPARE(context.validate(id,current.value->stateHash),QStringLiteral("clip_state_mismatch"));
	}
	void boundsPagesAndRejectsWrongNativeClasses()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());InstrumentTrack other(Engine::getSong());SampleClip clip(&track);
		AiBridgeProjectContext ids;AiBridgeAudioContext context;
		auto page=context.tracks(0,256);QCOMPARE(page.code,QStringLiteral("ok"));QCOMPARE(page.page.total,1);QCOMPARE(page.items[0].id,ids.idForTrack(&track));
		QCOMPARE(context.tracks(0,257).code,QStringLiteral("invalid_argument"));QCOMPARE(context.tracks(-1,1).code,QStringLiteral("invalid_argument"));
		auto clips=context.clips(ids.idForTrack(&track),0,1);QCOMPARE(clips.code,QStringLiteral("ok"));QCOMPARE(clips.page.total,1);QCOMPARE(clips.items[0].state.id,ids.idForClip(&clip));
		QCOMPARE(context.clips(ids.idForTrack(&other),0,1).code,QStringLiteral("unsupported_audio_operation"));
	}
	void quarantinedNativeReferencesCannotBecomeVerifiedContext()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);
		QDomDocument d;auto node=d.createElement("sampleclip");node.setAttribute("src",source);node.setAttribute("bounded_source","invalid");node.setAttribute("len",40);
		clip.loadSettings(node);QCOMPARE(clip.sourceLoadError(),QStringLiteral("unsupported_audio_operation"));
		AiBridgeProjectContext ids;auto result=AiBridgeAudioContext{}.clip(ids.idForClip(&clip));QCOMPARE(result.code,QStringLiteral("ok"));
		QCOMPARE(result.value->state.sourceStatus,QStringLiteral("unsupported_audio_operation"));QVERIFY(!result.value->sourceVerified);
	}
	void nativePlaybackPropertiesAndTrackMuteInvalidateSnapshots_data()
	{
		QTest::addColumn<QString>("property");
		QTest::newRow("track_mute")<<QStringLiteral("track_mute");
		QTest::newRow("frequency")<<QStringLiteral("frequency");
		QTest::newRow("amplification")<<QStringLiteral("amplification");
	}
	void nativePlaybackPropertiesAndTrackMuteInvalidateSnapshots()
	{
		QFETCH(QString,property);
		using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);adopt(clip);
		AiBridgeProjectContext ids;AiBridgeAudioContext context;const auto id=ids.idForClip(&clip);
		auto before=context.clip(id);
		if(property=="track_mute") track.setMuted(true);
		else if(property=="frequency") clip.sample().setFrequency(123);
		else clip.sample().setAmplification(0.5f);
		QCOMPARE(context.validate(id,before.value->stateHash),QStringLiteral("clip_state_mismatch"));
	}
	void legacyChannelsRemainUnknownAndContextDoesNotAuthorizeLegacyPaths()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);clip.setSampleFile(source);
		QCOMPARE(clip.sample().sampleSize(),size_t(64));
		ConfigManager::inst()->setValue("aibridge",keys[0],"[]");QVERIFY(QFile::remove(source));
		AiBridgeProjectContext ids;auto copied=AiBridgeAudioContext{}.clip(ids.idForClip(&clip));
		QCOMPARE(copied.code,QStringLiteral("ok"));QCOMPARE(copied.value->state.sourceStatus,QStringLiteral("unverified_legacy"));
		QVERIFY(!copied.value->sourceVerified);QVERIFY(!copied.value->state.sourceChannels);QCOMPARE(copied.value->state.sourceFrames,quint64(64));
	}
	void wrongThreadAndOversizedCopiedResponsesFailExplicitly()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);AiBridgeProjectContext ids;AiBridgeAudioContext context;
		const auto id=ids.idForClip(&clip);QString code;
		std::thread worker([&]{code=context.clip(id).code;});worker.join();QCOMPARE(code,QStringLiteral("wrong_thread"));
		clip.setName(QString(70000,QChar('x')));QCOMPARE(context.clip(id).code,QStringLiteral("result_too_large"));
		track.setName(QString(70000,QChar('x')));auto tracks=context.tracks(0,1);QCOMPARE(tracks.code,QStringLiteral("result_too_large"));QVERIFY(tracks.items.empty());
	}
	void nonfiniteNativePropertiesCannotProduceAUsableSnapshot()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);AiBridgeProjectContext ids;
		clip.sample().setFrequency(std::numeric_limits<float>::quiet_NaN());
		auto result=AiBridgeAudioContext{}.clip(ids.idForClip(&clip));QCOMPARE(result.code,QStringLiteral("unsupported_audio_operation"));QVERIFY(!result.value);
	}
	void paginationDisclosesTotalsAndNeverReturnsNativeIndexesAsIds()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());SampleClip first(&track),second(&track),third(&track);
		AiBridgeProjectContext ids;AiBridgeAudioContext context;const auto trackId=ids.idForTrack(&track);
		auto page=context.clips(trackId,1,1);QCOMPARE(page.code,QStringLiteral("ok"));QCOMPARE(page.page.total,3);QCOMPARE(page.page.offset,1);QCOMPARE(page.page.limit,1);QVERIFY(page.page.hasMore);
		QCOMPARE(page.items[0].state.id,ids.idForClip(&second));
		page=context.clips(trackId,2,1);QVERIFY(!page.page.hasMore);QCOMPARE(page.items[0].state.id,ids.idForClip(&third));
		page=context.clips(trackId,100,1);QCOMPARE(page.page.offset,3);QVERIFY(page.items.empty());QVERIFY(!page.page.hasMore);
	}
};
QTEST_MAIN(AiBridgeAudioContextTest)
#include "AiBridgeAudioContextTest.moc"
