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
#include "AiBridgeAudioContext.h"
#include "AiBridgeAudioActions.h"

class AiBridgeAudioWindowCompatibilityTest : public QObject
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
    void referenceEndpointOnlyChangeStalesCopiedHash()
    {
        using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);auto node=state();clip.loadSettings(node);
        AiBridgeProjectContext ids;AiBridgeAudioContext context;const auto id=ids.idForClip(&clip);
        const auto before=context.clip(id);QCOMPARE(before.code,QString("ok"));QVERIFY(before.value);
        QVERIFY(before.value->state.rangeWindowV2);QCOMPARE(before.value->state.rangeWindowV2->referenceEndFrame,qint64(88200));
        QVERIFY(before.value->verifiedSourceInfo);QCOMPARE(before.value->verifiedSourceInfo->frames,quint64(88200));
        node.firstChildElement("audio_source_window").setAttribute("reference_end","83790");clip.loadSettings(node);
        const auto after=context.clip(id);QCOMPARE(after.code,QString("ok"));QVERIFY(after.value);
        QVERIFY(before.value->stateHash!=after.value->stateHash);
        QCOMPARE(context.validate(id,before.value->stateHash),QString("clip_state_mismatch"));
    }
    void quarantineStatusParticipatesWithoutChangingSourceOrWindow()
    {
        using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);clip.loadSettings(state());
        AiBridgeProjectContext ids;AiBridgeAudioContext context;const auto id=ids.idForClip(&clip);const auto before=context.clip(id);
        QCOMPARE(before.code,QString("ok"));Engine::getSong()->setTempo(123);const auto after=context.clip(id);QCOMPARE(after.code,QString("ok"));
        QVERIFY(before.value->stateHash!=after.value->stateHash);
        QCOMPARE(save(clip).firstChildElement("audio_source_window").attribute("reference_end"),QString("88200"));
    }
    void legacyGoldenHashRemainsByteIdentical()
    {
        using namespace lmms;AiBridgeAudioClipState s;s.id="clip-1";s.trackId="track-1";s.sourceStatus="available";
        s.sourceReferenceHash="reference";s.sourceIdentity="identity";s.sourceDigest="digest";
        s.position=300;s.length=40;s.offset=-10;s.sourceFrames=337;s.sampleRate=44100;s.sourceChannels=2;
        QCOMPARE(audioStateHash({s}),QString("295cc28727a3672156a48dc800ae8994074d50cc242919ff34808f8c84869c86"));
    }
    void existingActionsPreserveNativeWindowAndRetireDeletedId()
    {
        using namespace lmms;SampleTrack track(Engine::getSong()),target(Engine::getSong());SampleClip clip(&track);clip.loadSettings(state());
        AiBridgeProjectContext ids;AiBridgeAudioContext context;AiBridgeAudioActions actions;const auto id=ids.idForClip(&clip);
        AiBridgeAudioEdit move;move.kind=AiBridgeAudioEditKind::Move;move.clipId=id;move.expectedAudioStateHash=context.clip(id).value->stateHash;move.position=300;
        const auto dry=actions.editClip(move,true);QCOMPARE(dry.code,QString("ok"));QCOMPARE(clip.startPosition().getTicks(),110);
        const auto moved=actions.editClip(move,false);QCOMPARE(moved.code,QString("ok"));QCOMPARE(moved.preview,dry.preview);QCOMPARE(clip.startPosition().getTicks(),300);
        AiBridgeAudioEdit duplicate;duplicate.kind=AiBridgeAudioEditKind::Duplicate;duplicate.clipId=id;duplicate.expectedAudioStateHash=context.clip(id).value->stateHash;
        duplicate.position=500;duplicate.destinationTrackId=ids.idForTrack(&target);const auto result=actions.editClip(duplicate,false);QCOMPARE(result.code,QString("ok"));QCOMPARE(result.createdClipIds.size(),1);
        const auto copyId=result.createdClipIds.front();auto* copy=dynamic_cast<SampleClip*>(ids.resolveClip(copyId));QVERIFY(copy);QVERIFY(copyId!=id);
        QCOMPARE(save(*copy).firstChildElement("audio_source_window").attribute("reference_end"),QString("88200"));QCOMPARE(copy->sourceFileIdentity(),clip.sourceFileIdentity());
        AiBridgeAudioEdit erase;erase.kind=AiBridgeAudioEditKind::Delete;erase.clipId=copyId;erase.expectedAudioStateHash=context.clip(copyId).value->stateHash;
        QCOMPARE(actions.editClip(erase,false).code,QString("ok"));QVERIFY(!ids.resolveClip(copyId));QCOMPARE(context.clip(id).code,QString("ok"));
    }
    void invalidMetadataDigestIsCanonicalAndPrivate()
    {
        using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);auto node=state();
        auto range=node.firstChildElement("audio_source_window");range.setAttribute("future","alpha");clip.loadSettings(node);
        AiBridgeProjectContext ids;AiBridgeAudioContext context;const auto id=ids.idForClip(&clip);const auto before=context.clip(id);QCOMPARE(before.code,QString("ok"));
        QCOMPARE(before.value->state.invalidRangeMetadataDigest.size(),32);
        range.removeAttribute("future");range.setAttribute("future","alpha");clip.loadSettings(node);
        QCOMPARE(context.clip(id).value->stateHash,before.value->stateHash);
        range.setAttribute("future","beta");clip.loadSettings(node);QVERIFY(context.clip(id).value->stateHash!=before.value->stateHash);
        const auto wire=QJsonDocument(AiBridgeAudioContext::toJson(*context.clip(id).value)).toJson(QJsonDocument::Compact);
        QVERIFY(!wire.contains("future"));QVERIFY(!wire.contains("alpha"));QVERIFY(!wire.contains("invalid_range_digest"));QVERIFY(!wire.contains(directory.toUtf8()));
    }
    void missingReferencePreservesPreciseNativeRangeError()
    {
        using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);auto node=state();node.setAttribute("src","");clip.loadSettings(node);
        AiBridgeProjectContext ids;const auto snapshot=AiBridgeAudioContext{}.clip(ids.idForClip(&clip));QCOMPARE(snapshot.code,QString("ok"));
        QCOMPARE(snapshot.value->state.rangeStatus,QString("audio_source_missing"));
    }
    void genericTimeGeometryPredicateKeepsLegacySeparate()
    {
        using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);
        QVERIFY(clip.allowsTimeGeometryEdits());clip.loadSettings(state());QVERIFY(!clip.allowsTimeGeometryEdits());
    }
    void compatibilityWarningIsOncePerProjectAndAbsentForLegacy()
    {
        using namespace lmms;auto* song=Engine::getSong();QSignalSpy warning(song,&Song::audioWindowCompatibilityWarning);
        SampleTrack track(song);SampleClip clip(&track);song->warnAudioWindowCompatibility();QCOMPARE(warning.count(),0);
        clip.loadSettings(state());song->warnAudioWindowCompatibility();song->warnAudioWindowCompatibility();QCOMPARE(warning.count(),1);
        QVERIFY(warning.front().front().toString().contains("Older builds may play different audio"));
    }
};
QTEST_MAIN(AiBridgeAudioWindowCompatibilityTest)
#include "AiBridgeAudioWindowCompatibilityTest.moc"
