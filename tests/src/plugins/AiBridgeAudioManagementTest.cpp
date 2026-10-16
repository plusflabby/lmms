#include <QtTest>
#include <QtEndian>
#include <QTemporaryDir>
#include <QFile>
#include <QDomDocument>
#include <QJsonDocument>
#include <cstring>
#include <stdexcept>
#include "AiBridgeAudioActions.h"
#include "AudioImportPolicy.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include "ProjectJournal.h"
class FailingAudioCopy final : public lmms::SampleClip
{
public:
	using SampleClip::SampleClip;
	mutable int attempts=0;
	bool throws=false;
	lmms::SampleClip* cloneToTrack(lmms::SampleTrack*) const override {++attempts;if(throws)throw std::runtime_error("private G:/private-source.wav");return nullptr;}
};
class ThrowAfterAudioMove final : public lmms::SampleClip
{
public:
	using SampleClip::SampleClip;
	bool throws=false;
	void movePosition(const lmms::TimePos& position) override {SampleClip::movePosition(position);if(throws)throw std::runtime_error("native move failed");}
};
class AiBridgeAudioManagementTest : public QObject
{
	Q_OBJECT
	QStringList old;const QStringList keys{"audio_import_roots","audio_import_max_file_bytes","audio_import_max_decoded_bytes"};
	std::unique_ptr<QTemporaryDir> dir;QString source;QByteArray bytes;int oldTempo;bool oldJournalling;
	lmms::SampleClip* make(lmms::SampleTrack& track,int position)
	{
		lmms::AiBridgeProjectContext ids;auto r=lmms::AiBridgeAudioActions{}.importFile({ids.idForTrack(&track),source,position,false});
		if(r.code!="ok")return nullptr;auto* clip=static_cast<lmms::SampleClip*>(ids.resolveClip(r.clipId));clip->setAutoResize(false);clip->changeLength(40);clip->setStartTimeOffset(-10);return clip;
	}
	lmms::AiBridgeAudioEdit request(lmms::AiBridgeAudioEditKind kind,lmms::SampleClip* clip,std::optional<qint64> position={},const QString& target={})
	{
		lmms::AiBridgeProjectContext ids;auto copied=lmms::AiBridgeAudioContext{}.clip(ids.idForClip(clip));
		return {kind,ids.idForClip(clip),copied.value ? copied.value->stateHash : QString{},target,position};
	}
private slots:
	void initTestCase(){lmms::Engine::init(true);}
	void cleanupTestCase(){lmms::Engine::destroy();}
	void init()
	{
		using namespace lmms;oldTempo=Engine::getSong()->getTempo();Engine::getSong()->setTempo(120);
		oldJournalling=Engine::projectJournal()->isJournalling();Engine::projectJournal()->setJournalling(true);
		for(auto& key:keys)old<<ConfigManager::inst()->value("aibridge",key,"test-absent");
		dir=std::make_unique<QTemporaryDir>(QCoreApplication::applicationDirPath()+"/audio-management-XXXXXX");QVERIFY(dir->isValid());source=dir->filePath("vocals.wav");
		bytes=QByteArray(88244,0);std::memcpy(bytes.data(),"RIFF",4);std::memcpy(bytes.data()+8,"WAVEfmt ",8);std::memcpy(bytes.data()+36,"data",4);
		qToLittleEndian<quint32>(88236,bytes.data()+4);qToLittleEndian<quint32>(16,bytes.data()+16);qToLittleEndian<quint16>(1,bytes.data()+20);qToLittleEndian<quint16>(1,bytes.data()+22);
		qToLittleEndian<quint32>(44100,bytes.data()+24);qToLittleEndian<quint32>(88200,bytes.data()+28);qToLittleEndian<quint16>(2,bytes.data()+32);qToLittleEndian<quint16>(16,bytes.data()+34);qToLittleEndian<quint32>(88200,bytes.data()+40);
		{QFile f(source);QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::NewOnly));QCOMPARE(f.write(bytes),qint64(bytes.size()));}
		ConfigManager::inst()->setValue("aibridge",keys[0],QString::fromUtf8(QJsonDocument(QJsonArray{dir->path()}).toJson(QJsonDocument::Compact)));
		ConfigManager::inst()->setValue("aibridge",keys[1],"1000000");ConfigManager::inst()->setValue("aibridge",keys[2],"1000000");
	}
	void cleanup()
	{
		using namespace lmms;for(int i=0;i<keys.size();++i){if(old[i]=="test-absent")ConfigManager::inst()->deleteValue("aibridge",keys[i]);else ConfigManager::inst()->setValue("aibridge",keys[i],old[i]);}
		old.clear();AudioImportPolicy::configured().refresh();dir.reset();Engine::getSong()->setTempo(oldTempo);
		Engine::projectJournal()->setJournalling(oldJournalling);
	}
	void singleMoveAndCrossTrackDuplicateUseTheSamePreviewAndNativeState()
	{
		using namespace lmms;SampleTrack a(Engine::getSong()),b(Engine::getSong());auto* clip=make(a,100);QVERIFY(clip);clip->setMuted(true);clip->sample().setReversed(true);clip->setColor(QColor("#123456"));
		AiBridgeProjectContext ids;AiBridgeAudioActions actions;const auto originalId=ids.idForClip(clip);auto move=request(AiBridgeAudioEditKind::Move,clip,400);
		auto preview=actions.editClip(move,true);QCOMPARE(preview.code,QStringLiteral("ok"));QCOMPARE(clip->startPosition().getTicks(),100);
		auto moved=actions.editClip(move,false);QCOMPARE(moved.code,QStringLiteral("ok"));QCOMPARE(moved.preview,preview.preview);QCOMPARE(ids.idForClip(clip),originalId);QCOMPARE(clip->startPosition().getTicks(),400);QCOMPARE(clip->length().getTicks(),40);QCOMPARE(clip->startTimeOffset().getTicks(),-10);
		auto duplicate=request(AiBridgeAudioEditKind::Duplicate,clip,100,ids.idForTrack(&b));auto copyPreview=actions.editClip(duplicate,true);QCOMPARE(copyPreview.code,QStringLiteral("ok"));QCOMPARE(b.numOfClips(),0);
		auto copied=actions.editClip(duplicate,false);QCOMPARE(copied.code,QStringLiteral("ok"));QCOMPARE(copied.preview,copyPreview.preview);QCOMPARE(copied.createdClipIds.size(),1);
		auto* copy=static_cast<SampleClip*>(ids.resolveClip(copied.createdClipIds[0]));QVERIFY(copy);QCOMPARE(copy->getTrack(),static_cast<Track*>(&b));QCOMPARE(copy->parentModel(),static_cast<Model*>(&b));QVERIFY(copied.createdClipIds[0]!=originalId);
		QCOMPARE(copy->sample().buffer(),clip->sample().buffer());QCOMPARE(copy->sampleFile(),clip->sampleFile());QCOMPARE(copy->sourceFileIdentity(),clip->sourceFileIdentity());QCOMPARE(copy->expectedSourceDigest(),clip->expectedSourceDigest());
		QCOMPARE(copy->length().getTicks(),40);QCOMPARE(copy->startTimeOffset().getTicks(),-10);QVERIFY(copy->isMuted());QVERIFY(copy->sample().reversed());QCOMPARE(copy->color(),clip->color());
		QDomDocument d;auto node=d.createElement("sampleclip");d.appendChild(node);copy->saveSettings(d,node);SampleClip restored(&b);restored.loadSettings(node);QCOMPARE(restored.startTimeOffset().getTicks(),-10);QCOMPARE(restored.length().getTicks(),40);QCOMPARE(restored.sourceFileIdentity(),copy->sourceFileIdentity());
		QFile f(source);QVERIFY(f.open(QIODevice::ReadOnly));QCOMPARE(f.readAll(),bytes);QVERIFY(!QJsonDocument(copied.toJson()).toJson().contains(dir->path().toUtf8()));
	}
	void batchAlignmentProjectsAllStepsAndDeletionRetiresOnlyTheClip()
	{
		using namespace lmms;SampleTrack a(Engine::getSong()),b(Engine::getSong());auto* first=make(a,100);auto* second=make(a,200);auto* third=make(a,300);QVERIFY(first&&second&&third);AiBridgeProjectContext ids;
		const auto deletedId=ids.idForClip(third);std::vector<AiBridgeAudioEdit> edits{request(AiBridgeAudioEditKind::Move,first,500),request(AiBridgeAudioEditKind::Duplicate,second,100,ids.idForTrack(&b)),request(AiBridgeAudioEditKind::Delete,third)};
		AiBridgeAudioActions actions;auto preview=actions.batchEdit(edits,true);QCOMPARE(preview.code,QStringLiteral("ok"));QCOMPARE(first->startPosition().getTicks(),100);QCOMPARE(a.numOfClips(),3);QCOMPARE(b.numOfClips(),0);
		auto result=actions.batchEdit(edits,false);QCOMPARE(result.code,QStringLiteral("ok"));QCOMPARE(result.preview,preview.preview);QCOMPARE(result.applied,(std::vector<int>{0,1,2}));QVERIFY(result.failed.empty());QVERIFY(result.unapplied.empty());
		QCOMPARE(first->startPosition().getTicks(),500);QCOMPARE(a.numOfClips(),2);QCOMPARE(b.numOfClips(),1);QVERIFY(!ids.resolveClip(deletedId));QVERIFY(QFileInfo::exists(source));
	}
	void laterInvalidInputAndStaleHashHaveZeroNativeWrites()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());auto* a=make(track,100);auto* b=make(track,200);QVERIFY(a&&b);auto move=request(AiBridgeAudioEditKind::Move,a,500);auto bad=request(AiBridgeAudioEditKind::Delete,b);bad.expectedAudioStateHash="stale";
		auto result=AiBridgeAudioActions{}.batchEdit({move,bad},false);QCOMPARE(result.code,QStringLiteral("clip_state_mismatch"));QCOMPARE(a->startPosition().getTicks(),100);QCOMPARE(track.numOfClips(),2);QVERIFY(result.applied.empty());
		a->movePosition(400);QCOMPARE(AiBridgeAudioActions{}.editClip(move,false).code,QStringLiteral("clip_state_mismatch"));QCOMPARE(a->startPosition().getTicks(),400);
	}
	void nativeFailureReturnsExactDisjointSlicesAndNeverRetries()
	{
		using namespace lmms;SampleTrack track(Engine::getSong()),target(Engine::getSong());auto* first=make(track,100);QVERIFY(first);FailingAudioCopy failure(&track);auto prepared=AudioImportPolicy::configured().prepare(source);QCOMPARE(failure.adoptAuthorizedAudio(std::move(prepared.audio)),QStringLiteral("ok"));failure.setAutoResize(false);failure.movePosition(300);failure.changeLength(40);failure.setStartTimeOffset(-10);auto* last=make(track,500);QVERIFY(last);
		AiBridgeProjectContext ids;auto result=AiBridgeAudioActions{}.batchEdit({request(AiBridgeAudioEditKind::Move,first,700),request(AiBridgeAudioEditKind::Duplicate,&failure,100,ids.idForTrack(&target)),request(AiBridgeAudioEditKind::Delete,last)},false);
		QCOMPARE(result.code,QStringLiteral("partial_failure"));QCOMPARE(result.applied,(std::vector<int>{0}));QCOMPARE(result.failed,(std::vector<int>{1}));QCOMPARE(result.unapplied,(std::vector<int>{2}));QCOMPARE(failure.attempts,1);QCOMPARE(first->startPosition().getTicks(),700);QVERIFY(ids.resolveClip(ids.idForClip(last)));QCOMPARE(target.numOfClips(),0);
	}
	void nativePostMismatchStopsBeforeLaterStepsWithoutCompensation()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());auto* first=make(track,100);auto* last=make(track,300);QVERIFY(first&&last);
		connect(first,&Clip::positionChanged,first,[first]{first->setMuted(true);});
		auto result=AiBridgeAudioActions{}.batchEdit({request(AiBridgeAudioEditKind::Move,first,500),request(AiBridgeAudioEditKind::Delete,last)},false);
		QCOMPARE(result.code,QStringLiteral("post_verification_failed"));QCOMPARE(result.failed,(std::vector<int>{0}));QCOMPARE(result.unapplied,(std::vector<int>{1}));QVERIFY(result.applied.empty());QCOMPARE(first->startPosition().getTicks(),500);QCOMPARE(track.numOfClips(),2);QVERIFY(first->isMuted());
	}
	void nativeNameAndSoloCannotEvadeExpectedStateGuards_data()
	{QTest::addColumn<QString>("property");QTest::newRow("name")<<QStringLiteral("name");QTest::newRow("solo")<<QStringLiteral("solo");}
	void nativeNameAndSoloCannotEvadeExpectedStateGuards()
	{
		QFETCH(QString,property);using namespace lmms;SampleTrack track(Engine::getSong());auto* clip=make(track,100);QVERIFY(clip);AiBridgeProjectContext ids;AiBridgeAudioContext context;auto id=ids.idForClip(clip);auto initial=context.clip(id);
		if(property=="name")clip->setName("transient native name");else clip->setSolo(true);
		QCOMPARE(context.validate(id,initial.value->stateHash),QStringLiteral("clip_state_mismatch"));
		QCOMPARE(AiBridgeAudioActions{}.editClip(request(AiBridgeAudioEditKind::Move,clip,500),false).code,QStringLiteral("unsupported_audio_operation"));QCOMPARE(clip->startPosition().getTicks(),100);
	}
	void nativeThrownFailureIsRedactedAndDoesNotEscapeOrReplay()
	{
		using namespace lmms;SampleTrack track(Engine::getSong()),target(Engine::getSong());auto* first=make(track,100);QVERIFY(first);FailingAudioCopy failure(&track);failure.throws=true;
		auto prepared=AudioImportPolicy::configured().prepare(source);QCOMPARE(failure.adoptAuthorizedAudio(std::move(prepared.audio)),QStringLiteral("ok"));failure.setAutoResize(false);failure.movePosition(300);failure.changeLength(40);failure.setStartTimeOffset(-10);
		AiBridgeProjectContext ids;AiBridgeAudioEditResult result;bool escaped=false;
		try {result=AiBridgeAudioActions{}.batchEdit({request(AiBridgeAudioEditKind::Move,first,700),request(AiBridgeAudioEditKind::Duplicate,&failure,100,ids.idForTrack(&target))},false);}catch(...){escaped=true;}
		QVERIFY2(!escaped,"Native copy failure escaped instead of returning a redacted partial result");QCOMPARE(result.code,QStringLiteral("partial_failure"));QCOMPARE(result.failureCode,QStringLiteral("native_operation_failed"));
		QCOMPARE(result.applied,(std::vector<int>{0}));QCOMPARE(result.failed,(std::vector<int>{1}));QCOMPARE(failure.attempts,1);QCOMPARE(first->startPosition().getTicks(),700);QCOMPARE(target.numOfClips(),0);QVERIFY(!QJsonDocument(result.toJson()).toJson().contains("private-source.wav"));
	}
	void missingSourceRejectsCopyButDeletionPreservesTheRecording()
	{
		using namespace lmms;SampleTrack track(Engine::getSong()),target(Engine::getSong());auto* clip=make(track,100);QVERIFY(clip);AiBridgeProjectContext ids;const auto id=ids.idForClip(clip);const auto reference=clip->sampleFile();
		QVERIFY(QFile::rename(source,dir->filePath("retained-original.wav")));
		auto result=AiBridgeAudioActions{}.editClip(request(AiBridgeAudioEditKind::Duplicate,clip,100,ids.idForTrack(&target)),false);QCOMPARE(result.code,QStringLiteral("audio_source_missing"));QCOMPARE(target.numOfClips(),0);QCOMPARE(clip->sampleFile(),reference);
		result=AiBridgeAudioActions{}.editClip(request(AiBridgeAudioEditKind::Delete,clip),false);QCOMPARE(result.code,QStringLiteral("ok"));QVERIFY(!ids.resolveClip(id));QFile f(dir->filePath("retained-original.wav"));QVERIFY(f.open(QIODevice::ReadOnly));QCOMPARE(f.readAll(),bytes);
	}
	void noEffectAndLaterCollisionLeaveJournalAndNativeDataUnchanged()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());auto* a=make(track,100);auto* b=make(track,300);QVERIFY(a&&b);
		Engine::projectJournal()->clearJournal();auto zero=AiBridgeAudioActions{}.editClip(request(AiBridgeAudioEditKind::Move,a,100),false);QCOMPARE(zero.code,QStringLiteral("no_effect"));QCOMPARE(zero.noEffect,(std::vector<int>{0}));QVERIFY(!Engine::projectJournal()->canUndo());
		auto conflict=AiBridgeAudioActions{}.batchEdit({request(AiBridgeAudioEditKind::Move,a,500),request(AiBridgeAudioEditKind::Move,b,510)},false);QCOMPARE(conflict.code,QStringLiteral("destination_conflict"));QCOMPARE(a->startPosition().getTicks(),100);QCOMPARE(b->startPosition().getTicks(),300);QVERIFY(!Engine::projectJournal()->canUndo());
	}
	void repeatedClipStepsBoundPreviewAndRetainCompleteExecutionSlices()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());auto* clip=make(track,100);QVERIFY(clip);auto base=request(AiBridgeAudioEditKind::Move,clip,300);std::vector<AiBridgeAudioEdit> edits;
		for(int i=0;i<129;++i){auto r=base;r.position=300+i*50;edits.push_back(r);}
		auto result=AiBridgeAudioActions{}.batchEdit(edits,false);QCOMPARE(result.code,QStringLiteral("ok"));QVERIFY(result.previewHasMore);QVERIFY(result.preview.size()<=128);QCOMPARE(result.applied.size(),size_t(129));QCOMPARE(result.affectedClipIds.size(),1);QCOMPARE(clip->startPosition().getTicks(),6700);QVERIFY(QJsonDocument(result.toJson()).toJson(QJsonDocument::Compact).size()<64*1024);
	}
	void collisionAfterFirstPageStillRejectsBeforeWrites()
	{
		using namespace lmms;SampleTrack track(Engine::getSong());auto* selected=make(track,100);QVERIFY(selected);
		for(int i=0;i<260;++i){auto* blocker=new SampleClip(&track);blocker->setAutoResize(false);blocker->movePosition(1000+i*50);blocker->changeLength(40);}
		auto result=AiBridgeAudioActions{}.editClip(request(AiBridgeAudioEditKind::Move,selected,13950),false);QCOMPARE(result.code,QStringLiteral("destination_conflict"));QCOMPARE(selected->startPosition().getTicks(),100);
	}
	void anExceptionAfterNativeMovementKeepsTheProjectDirty()
	{
		using namespace lmms;QTemporaryDir projectDirectory(QCoreApplication::applicationDirPath()+"/management-project-XXXXXX");QVERIFY(projectDirectory.isValid());SampleTrack track(Engine::getSong());ThrowAfterAudioMove clip(&track);auto prepared=AudioImportPolicy::configured().prepare(source);QCOMPARE(clip.adoptAuthorizedAudio(std::move(prepared.audio)),QStringLiteral("ok"));clip.setAutoResize(false);clip.movePosition(100);clip.changeLength(40);clip.setStartTimeOffset(-10);
		QVERIFY(Engine::getSong()->guiSaveProjectAs(projectDirectory.filePath("before-native-error.mmp")));QVERIFY(!Engine::getSong()->isModified());clip.throws=true;
		auto result=AiBridgeAudioActions{}.editClip(request(AiBridgeAudioEditKind::Move,&clip,500),false);QCOMPARE(result.code,QStringLiteral("partial_failure"));QCOMPARE(clip.startPosition().getTicks(),500);QVERIFY(Engine::getSong()->isModified());QCOMPARE(result.failed,(std::vector<int>{0}));QVERIFY(result.applied.empty());
		clip.throws=false;
	}
	void maximumAffectedObjectsHaveBoundedResultsAndNoPartialOversizeWrites()
	{
		using namespace lmms;SampleTrack track(Engine::getSong()),target(Engine::getSong());auto* sourceClip=make(track,100);QVERIFY(sourceClip);AiBridgeProjectContext ids;auto base=request(AiBridgeAudioEditKind::Duplicate,sourceClip,1000,ids.idForTrack(&target));std::vector<AiBridgeAudioEdit> edits;
		for(int i=0;i<32;++i){auto r=base;r.position=1000+i*50;edits.push_back(r);}
		QCOMPARE(AiBridgeAudioActions{}.batchEdit(edits,false).code,QStringLiteral("invalid_argument"));QCOMPARE(target.numOfClips(),0);edits.pop_back();
		auto result=AiBridgeAudioActions{}.batchEdit(edits,false);QCOMPARE(result.code,QStringLiteral("ok"));QCOMPARE(target.numOfClips(),31);QCOMPARE(result.createdClipIds.size(),31);QVERIFY(QJsonDocument(result.toJson()).toJson(QJsonDocument::Compact).size()<64*1024);
	}
	void equalSizedNativeBufferReplacementInvalidatesExpectedAudioState_data()
	{QTest::addColumn<bool>("legacy");QTest::newRow("embedded")<<false;QTest::newRow("legacy-file")<<true;}
	void equalSizedNativeBufferReplacementInvalidatesExpectedAudioState()
	{
		QFETCH(bool,legacy);using namespace lmms;SampleTrack track(Engine::getSong());SampleClip clip(&track);AiBridgeProjectContext ids;AiBridgeAudioContext context;
		if(legacy)clip.setSampleFile(source);else clip.setSampleBuffer(std::make_shared<const SampleBuffer>(std::vector<SampleFrame>(64),44100));
		const auto id=ids.idForClip(&clip);auto before=context.clip(id);QCOMPARE(before.code,QStringLiteral("ok"));
		if(legacy){auto changed=bytes;qToLittleEndian<qint16>(12000,changed.data()+44);QFile f(source);QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::Truncate));f.write(changed);f.close();clip.setSampleFile(source);}
		else {std::vector<SampleFrame> changed(64);changed[0]={0.5f,0.5f};clip.setSampleBuffer(std::make_shared<const SampleBuffer>(std::move(changed),44100));}
		auto after=context.clip(id);QCOMPARE(after.code,QStringLiteral("ok"));QCOMPARE(after.value->state.sourceFrames,before.value->state.sourceFrames);QCOMPARE(after.value->state.sampleRate,before.value->state.sampleRate);
		QVERIFY(after.value->stateHash!=before.value->stateHash);
		auto stale=AiBridgeAudioEdit{AiBridgeAudioEditKind::Move,id,before.value->stateHash,{},1000};QCOMPARE(AiBridgeAudioActions{}.editClip(stale,false).code,QStringLiteral("clip_state_mismatch"));QCOMPARE(clip.startPosition().getTicks(),0);
	}
};
QTEST_MAIN(AiBridgeAudioManagementTest)
#include "AiBridgeAudioManagementTest.moc"
