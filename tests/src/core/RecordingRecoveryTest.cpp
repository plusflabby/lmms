#include "RecordingRecovery.h"
#include "RecordingTakeJournal.h"
#include "RecordingInputController.h"
#include <QJsonDocument>
#include <QJsonObject>
#ifdef Q_OS_WIN
#include <windows.h>
#include <winioctl.h>
#endif
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QCryptographicHash>
#include <QUuid>
#include <QSet>
#include <cstring>
#include <algorithm>
using namespace lmms::recording;
namespace {
QString uuid(){return QUuid::createUuid().toString(QUuid::WithoutBraces);}
QByteArray contents(const QString& path){QFile file(path);if(!file.open(QIODevice::ReadOnly))return {};return file.readAll();}
bool saveNew(const QString& path,const QByteArray& bytes){QFile file(path);return file.open(QIODevice::WriteOnly|QIODevice::NewOnly) && file.write(bytes)==bytes.size();}
#ifdef Q_OS_WIN
struct JunctionCleanup {QString path;~JunctionCleanup(){if(!path.isEmpty())RemoveDirectoryW(reinterpret_cast<LPCWSTR>(path.utf16()));}};
bool junction(const QString& link,const QString& target)
{
 if(!CreateDirectoryW(reinterpret_cast<LPCWSTR>(link.utf16()),nullptr))return false;
 const auto handle=CreateFileW(reinterpret_cast<LPCWSTR>(link.utf16()),GENERIC_READ|GENERIC_WRITE|FILE_WRITE_ATTRIBUTES,
  FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
 if(handle==INVALID_HANDLE_VALUE){RemoveDirectoryW(reinterpret_cast<LPCWSTR>(link.utf16()));return false;}
 auto dos=target;dos.replace('/','\\');const auto substitute=QStringLiteral("\\??\\")+dos;
 const USHORT a=USHORT(substitute.size()*2),b=USHORT(dos.size()*2),dataLength=USHORT(8+a+2+b+2);
 std::vector<BYTE> bytes(std::size_t(dataLength)+8,0);const ULONG tag=IO_REPARSE_TAG_MOUNT_POINT;
 const USHORT zero=0,printOffset=USHORT(a+2);std::memcpy(bytes.data(),&tag,4);std::memcpy(bytes.data()+4,&dataLength,2);
 std::memcpy(bytes.data()+8,&zero,2);std::memcpy(bytes.data()+10,&a,2);std::memcpy(bytes.data()+12,&printOffset,2);std::memcpy(bytes.data()+14,&b,2);
 std::memcpy(bytes.data()+16,substitute.utf16(),a);std::memcpy(bytes.data()+16+a+2,dos.utf16(),b);
 DWORD returned=0;const bool ok=DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,bytes.data(),DWORD(bytes.size()),nullptr,0,&returned,nullptr)!=FALSE;
 CloseHandle(handle);if(!ok)RemoveDirectoryW(reinterpret_cast<LPCWSTR>(link.utf16()));return ok;
}
#endif
struct ReceiptStorage {QByteArray bytes;};
class ReceiptSink final : public RecordingReceiptSink
{
public:
 explicit ReceiptSink(ReceiptStorage& s):storage(s){}
 bool appendAndFlush(const QByteArray& bytes) noexcept override{storage.bytes+=bytes;return true;}
 ReceiptStorage& storage;
};
struct Fixture
{
 QTemporaryDir directory;
 RecordingLibrary::Approval approval;
 ProjectIdentity project{uuid(),1};
 TakeIdentity take{uuid(),"Immutable prefix"};
 TakeDescriptor descriptor;
 Fixture():approval(RecordingLibrary::authorizeLocalRoot(directory.path()))
 {descriptor.projectId=project.id;descriptor.takeId=take.id;descriptor.label=take.label;descriptor.epoch={7,3};descriptor.projectGeneration=7;descriptor.sampleRate=48000;descriptor.maxRawFrames=5760000;descriptor.targetTick=384;}
 QString stem() const{return directory.path()+"/"+project.id+"/take-"+take.id;}
 bool partial(unsigned checkpoint=4,unsigned frames=8,bool seal=false,bool complete=false,bool begin=true)
 {
  if(!approval.library)return false;
  auto files=approval.library->createUniqueTake(project,take,descriptor.sampleRate,descriptor.maxRawFrames);if(!files.sink)return false;
  if(!begin)return true;
  if(files.sink->beginTake(descriptor)!=WriterIoCode::Ok)return false;
  QByteArray pcm;for(unsigned i=0;i<frames;++i){pcm.append(char(i+1));pcm.append(char(0));}
  const auto result=files.sink->appendPayload({reinterpret_cast<const std::uint8_t*>(pcm.constData()),std::size_t(pcm.size())});
  if(result.code!=WriterIoCode::Ok || result.bytesWritten!=std::uint32_t(pcm.size()))return false;
  TakeProgress p;p.sequence=1;p.receivedFrames=checkpoint;p.acceptedFrames=checkpoint;p.writtenFrames=checkpoint;p.timestampQuality=InputTimestampQuality::Discontinuous;
  if(checkpoint && files.sink->appendCheckpoint(p)!=WriterIoCode::Ok)return false;
  if(seal)
  {
   ++p.sequence;if(files.sink->appendSealIntent(p)!=WriterIoCode::Ok || files.sink->sealPcm16(descriptor.sampleRate,frames)!=WriterIoCode::Ok)return false;
   if(complete){++p.sequence;p.sourceDigestKnown=true;const auto digest=files.sink->sealedAudioSha256();std::memcpy(p.sourceDigest.data(),digest.constData(),32);
    if(files.sink->appendTerminal(TakeTerminal::Complete,p)!=WriterIoCode::Ok)return false;}
  }
  files.sink->preservePartial();return true;
 }
};
}
class RecordingRecoveryTest : public QObject
{
 Q_OBJECT
private slots:
 void recoverOnlyVerifiedFrameAlignedPrefix()
 {
  Fixture f;QVERIFY(f.partial());RecordingRecovery recovery;
  const auto page=recovery.scan(*f.approval.library);QCOMPARE(page.code,QString("ok"));QCOMPARE(page.takes.size(),std::size_t(1));
  const auto& m=page.takes[0];QCOMPARE(m.state,QString("recoverable"));QCOMPARE(m.durableFrames,quint64(4));
  QCOMPARE(m.recoverableFrames,quint64(4));QCOMPARE(m.unknownTailBytes,quint64(8));QCOMPARE(m.source.rawFrames,quint64(8));
  QCOMPARE(m.timestampQuality,InputTimestampQuality::Discontinuous);QCOMPARE(m.placementQuality,QString("uncalibrated"));QCOMPARE(m.driftQuality,QString("unmeasured"));
 }
 void recoveredCopyHasNewIdOriginalPartialUnchanged()
 {
  Fixture f;QVERIFY(f.partial());const auto original=contents(f.stem()+".partial"),journal=contents(f.stem()+".journal");
  const auto newId=uuid();RecordingRecovery recovery([newId]{return newId;});QVERIFY(recovery.scan(*f.approval.library).code=="ok");
  const auto result=recovery.recover(f.take.id);QCOMPARE(result.code,QString("recovered"));QVERIFY(result.token);
  QCOMPARE(result.token->facts().takeId,newId);QCOMPARE(result.token->facts().rawFrames,quint64(4));
  QCOMPARE(result.manifest.recoveredFromTakeId,f.take.id);QCOMPARE(result.manifest.recoveredFromProjectId,f.project.id);
  QCOMPARE(result.manifest.recoveredFromDurableFrames,quint64(4));QCOMPARE(result.manifest.recoveredFromUnknownTailBytes,quint64(8));
  QCOMPARE(result.manifest.recoveredFromReadableFrames,quint64(8));
  QCOMPARE(result.manifest.placementState,QString("saved_unattached"));QCOMPARE(result.manifest.placementQuality,QString("uncalibrated"));
  const auto target=f.directory.path()+"/"+f.project.id+"/take-"+newId;
  const auto bytes=contents(target+".wav");QCOMPARE(bytes.size(),52);QCOMPARE(bytes.mid(44),original.mid(44,8));
  QCOMPARE(contents(f.stem()+".partial"),original);QCOMPARE(contents(f.stem()+".journal"),journal);
  QCOMPARE(result.token->facts().sourceDigest,QCryptographicHash::hash(bytes,QCryptographicHash::Sha256));
  QVERIFY(QFile::exists(target+".recovery.json"));QVERIFY(QFile::exists(target+".journal"));
 }
 void crashAtEveryHeaderAndSealBoundary_data()
 {
  QTest::addColumn<int>("stage");QTest::addColumn<QString>("state");QTest::addColumn<quint64>("frames");
  QTest::newRow("before-headers")<<0<<QString("incomplete")<<quint64(0);
  QTest::newRow("headers-ready")<<1<<QString("incomplete")<<quint64(0);
  QTest::newRow("uncheckpointed-audio")<<2<<QString("incomplete")<<quint64(0);
  QTest::newRow("after-checkpoint")<<3<<QString("recoverable")<<quint64(4);
  QTest::newRow("torn-journal-tail")<<4<<QString("recoverable")<<quint64(4);
  QTest::newRow("torn-owned-length-rewrite")<<5<<QString("recoverable")<<quint64(4);
  QTest::newRow("seal-intent-before-rename")<<6<<QString("recoverable")<<quint64(8);
  QTest::newRow("rename-before-terminal")<<7<<QString("saved_unattached")<<quint64(8);
  QTest::newRow("terminal-durable-before-exit")<<8<<QString("complete")<<quint64(8);
  QTest::newRow("torn-initial-manifest")<<9<<QString("quarantined")<<quint64(0);
 }
 void crashAtEveryHeaderAndSealBoundary()
 {
  QFETCH(int,stage);QFETCH(QString,state);QFETCH(quint64,frames);Fixture f;
  if(stage==0)QVERIFY(f.partial(0,0,false,false,false));
  else if(stage==1 || stage==9)QVERIFY(f.partial(0,0));
  else if(stage==2)QVERIFY(f.partial(0,8));
  else if(stage>=6)QVERIFY(f.partial(8,8,stage>=7,stage==8));
  else QVERIFY(f.partial());
  if(stage==4){QFile journal(f.stem()+".journal");QVERIFY(journal.open(QIODevice::Append));QCOMPARE(journal.write(QByteArray::fromHex("0201740000")),qint64(5));}
  if(stage==5){QFile partial(f.stem()+".partial");QVERIFY(partial.open(QIODevice::ReadWrite));QVERIFY(partial.seek(4));QCOMPARE(partial.write(QByteArray::fromHex("ffffffff")),qint64(4));}
  if(stage==6){TakeProgress p;p.sequence=2;p.receivedFrames=p.acceptedFrames=p.writtenFrames=8;
   const auto r=RecordingTakeJournalCodec::encodeSealIntent(p);QFile journal(f.stem()+".journal");QVERIFY(journal.open(QIODevice::Append));QCOMPARE(journal.write(reinterpret_cast<const char*>(r.bytes.data()),r.size),qint64(r.size));}
  if(stage==9){QFile journal(f.stem()+".journal");QVERIFY(journal.open(QIODevice::ReadWrite));QVERIFY(journal.resize(11));}
  const auto audioName=f.stem()+(stage==7 || stage==8?".wav":".partial");
  const auto beforeAudio=contents(audioName),beforeJournal=contents(f.stem()+".journal");
  f.approval.library.reset();f.approval=RecordingLibrary::authorizeLocalRoot(f.directory.path());QVERIFY(f.approval.library);
  RecordingRecovery recovery;const auto page=recovery.scan(*f.approval.library);QCOMPARE(page.code,QString("ok"));QCOMPARE(page.takes.size(),std::size_t(1));
  QCOMPARE(page.takes[0].state,state);QCOMPARE(page.takes[0].recoverableFrames,frames);
  if(stage==4)QVERIFY(page.takes[0].tornJournalTail);
  if(frames && stage!=8){const auto recovered=recovery.recover(f.take.id);QCOMPARE(recovered.code,QString("recovered"));QVERIFY(recovered.token);QCOMPARE(recovered.token->facts().rawFrames,frames);}
  QCOMPARE(contents(audioName),beforeAudio);QCOMPARE(contents(f.stem()+".journal"),beforeJournal);
 }
 void tamperedOversizedManifestQuarantined_data()
 {
  QTest::addColumn<QString>("damage");for(const auto name:{"crc","oversized","unsupported_schema","false_frames","missing_manifest"})QTest::newRow(name)<<QString(name);
 }
 void tamperedOversizedManifestQuarantined()
 {
  QFETCH(QString,damage);Fixture f;QVERIFY(f.partial(damage=="false_frames"?9:4,8));
  const auto original=contents(f.stem()+".partial");
  if(damage=="missing_manifest"){QVERIFY(QFile::rename(f.stem()+".journal",f.stem()+".hidden"));}
  else{QFile journal(f.stem()+".journal");QVERIFY(journal.open(QIODevice::ReadWrite));
   if(damage=="oversized"){QVERIFY(journal.resize(65537));}
   else if(damage=="unsupported_schema"){QVERIFY(journal.seek(1));QCOMPARE(journal.write(QByteArray(1,char(2))),qint64(1));}
   else if(damage=="crc"){QVERIFY(journal.seek(journal.size()-1));QCOMPARE(journal.write(QByteArray(1,char(0x7f))),qint64(1));}}
  RecordingRecovery recovery;const auto page=recovery.scan(*f.approval.library);QCOMPARE(page.code,QString("ok"));QCOMPARE(page.takes.size(),std::size_t(1));
  QCOMPARE(page.takes[0].state,QString("quarantined"));QVERIFY(!recovery.recover(f.take.id).token);QCOMPARE(contents(f.stem()+".partial"),original);
 }
 void oddTrailingByteIsUnknownAndNeverCopied()
 {
  Fixture f;QVERIFY(f.partial());{QFile file(f.stem()+".partial");QVERIFY(file.open(QIODevice::Append));QCOMPARE(file.write("x",1),qint64(1));}
  RecordingRecovery recovery;const auto page=recovery.scan(*f.approval.library);QCOMPARE(page.takes.size(),std::size_t(1));
  QCOMPARE(page.takes[0].unknownTailBytes,quint64(9));const auto result=recovery.recover(f.take.id);QCOMPARE(result.code,QString("recovered"));QVERIFY(result.token);QCOMPARE(result.token->facts().rawFrames,quint64(4));
 }
 void finalNameCollisionNeverOverwritesEitherOriginal()
 {
  Fixture f;QVERIFY(f.partial());const auto newId=uuid(),target=f.directory.path()+"/"+f.project.id+"/take-"+newId+".wav";
  const QByteArray sentinel("prior sealed original");QVERIFY(saveNew(target,sentinel));const auto original=contents(f.stem()+".partial");
  RecordingRecovery recovery([newId]{return newId;});QVERIFY(recovery.scan(*f.approval.library).code=="ok");
  const auto result=recovery.recover(f.take.id);QVERIFY(!result.ok());QVERIFY(!result.token);QCOMPARE(contents(target),sentinel);QCOMPARE(contents(f.stem()+".partial"),original);
 }
 void junctionAndChangedIdentityDenied_data()
 {QTest::addColumn<QString>("change");QTest::newRow("same-id-content-change")<<QString("content");QTest::newRow("replacement-file")<<QString("replacement");QTest::newRow("revoked-root")<<QString("revoked");}
 void junctionAndChangedIdentityDenied()
 {
  QFETCH(QString,change);Fixture f;QVERIFY(f.partial());RecordingRecovery recovery;QVERIFY(recovery.scan(*f.approval.library).code=="ok");
  if(change=="content"){QFile file(f.stem()+".partial");QVERIFY(file.open(QIODevice::ReadWrite));QVERIFY(file.seek(44));QCOMPARE(file.write("zz",2),qint64(2));}
  if(change=="replacement"){const auto before=contents(f.stem()+".partial");QVERIFY(QFile::rename(f.stem()+".partial",f.stem()+".retained"));QVERIFY(saveNew(f.stem()+".partial",before));}
  if(change=="revoked")f.approval.library->revoke();
  const auto result=recovery.recover(f.take.id);QVERIFY(!result.ok());QVERIFY(!result.token);QVERIFY(!QFile::exists(f.stem()+".wav"));
 }
 void scanAndPaginationBounded()
 {
  Fixture f;QVERIFY(f.approval.library);QCOMPARE(f.approval.library->prepareProjectFolder(f.project),QString("ok"));
  QSet<QString> expected;const auto header=RecordingWavHeader::encodePcm16(48000,0);
  for(unsigned i=0;i<300;++i){auto d=f.descriptor;d.takeId=uuid();expected.insert(d.takeId);const auto stem=f.directory.path()+"/"+f.project.id+"/take-"+d.takeId;
   QVERIFY(saveNew(stem+".partial",QByteArray(reinterpret_cast<const char*>(header.bytes.data()),44)));QVERIFY(saveNew(stem+".journal",RecordingTakeJournalCodec::encodeHeader(d)));}
  RecordingRecovery recovery;QCOMPARE(recovery.scan(*f.approval.library,{},101).code,QString("invalid_page"));
  QSet<QString> seen;RecoveryCursor cursor;bool more=true;unsigned passes=0;
  while(more && ++passes<100){const auto page=recovery.scan(*f.approval.library,cursor,25);QCOMPARE(page.code,QString("ok"));QVERIFY(page.takes.size()<=25);QVERIFY(page.inspectedCandidates<=256);
   for(const auto& m:page.takes){QVERIFY(!seen.contains(m.descriptor.takeId));seen.insert(m.descriptor.takeId);}cursor=page.next;more=page.hasMore;}
  QVERIFY(!more);QCOMPARE(seen,expected);QCOMPARE(recovery.recentTakes().size(),std::size_t(64));
  QCOMPARE(recovery.scan(*f.approval.library,{"forged-cursor"},25).code,QString("stale_cursor"));
  for(const auto& id:expected)QVERIFY(QFile::exists(f.directory.path()+"/"+f.project.id+"/take-"+id+".partial"));
 }
 void dismissNeverDeletes()
 {
  Fixture f;QVERIFY(f.partial());const auto audio=contents(f.stem()+".partial"),journal=contents(f.stem()+".journal");RecordingRecovery recovery;
  QVERIFY(recovery.scan(*f.approval.library).code=="ok");QCOMPARE(recovery.dismiss(f.take.id),QString("dismissed"));
  RecordingRecovery fresh;const auto page=fresh.scan(*f.approval.library);QCOMPARE(page.code,QString("ok"));QVERIFY(page.takes.empty());
  QCOMPARE(contents(f.stem()+".partial"),audio);QCOMPARE(contents(f.stem()+".journal"),journal);
 }
 void actualProjectJunctionAndHardLinkedAudioAreDenied()
 {
#ifdef Q_OS_WIN
  Fixture f;QVERIFY(f.partial());QTemporaryDir outside;QVERIFY(outside.isValid());
  const auto foreignProject=uuid(),foreignTake=uuid();const auto folder=outside.path()+"/"+foreignProject;QVERIFY(QDir().mkpath(folder));
  QVERIFY(saveNew(folder+"/project.json",QJsonDocument(QJsonObject{{"schema",1},{"project_id",foreignProject}}).toJson(QJsonDocument::Compact)));
  const QByteArray sentinel("outside source remains private");QVERIFY(saveNew(folder+"/take-"+foreignTake+".partial",sentinel));
  const auto link=f.directory.path()+"/"+foreignProject;QVERIFY(junction(link,folder));JunctionCleanup cleanup{link};
  const auto old=contents(f.stem()+".partial");QVERIFY(QFile::rename(f.stem()+".partial",f.stem()+".retained"));
  const auto audio=f.stem()+".partial",target=f.stem()+".retained";
  QVERIFY(CreateHardLinkW(reinterpret_cast<LPCWSTR>(audio.utf16()),reinterpret_cast<LPCWSTR>(target.utf16()),nullptr));
  RecordingRecovery recovery;const auto page=recovery.scan(*f.approval.library);QCOMPARE(page.code,QString("ok"));QCOMPARE(page.takes.size(),std::size_t(1));
  QCOMPARE(page.takes[0].state,QString("quarantined"));QVERIFY(!recovery.recover(f.take.id).token);QVERIFY(!recovery.recover(foreignTake).token);
  QCOMPARE(contents(folder+"/take-"+foreignTake+".partial"),sentinel);QCOMPARE(contents(target),old);
#else
  QFAIL("Windows-specific qualification fixture");
#endif
 }
 void irrelevantEntriesCannotExceedThePerPassScanBudget()
 {
  Fixture f;QVERIFY(f.partial());const auto folder=f.directory.path()+"/"+f.project.id;
  for(unsigned i=0;i<550;++i)QVERIFY(saveNew(folder+QString("/irrelevant-%1").arg(i,4,10,QChar('0')),"owned"));
  RecordingRecovery recovery;const auto page=recovery.scan(*f.approval.library,{},100);QCOMPARE(page.code,QString("ok"));
  QCOMPARE(page.inspectedCandidates,256u);QVERIFY(page.hasMore);QVERIFY(!page.next.token.isEmpty());
 }
 void recoveryMetadataFailureStillReportsThePreservedNewTake()
 {
  Fixture f;QVERIFY(f.partial());const auto target=uuid();const auto stem=f.directory.path()+"/"+f.project.id+"/take-"+target;
  const QByteArray sentinel("existing lineage evidence");QVERIFY(saveNew(stem+".recovery.json",sentinel));
  const auto original=contents(f.stem()+".partial");RecordingRecovery recovery([target]{return target;});
  QVERIFY(recovery.scan(*f.approval.library).code=="ok");const auto result=recovery.recover(f.take.id);
  QCOMPARE(result.code,QString("recovery_metadata_failed"));QVERIFY(!result.token);
  QCOMPARE(result.manifest.descriptor.takeId,target);QCOMPARE(result.manifest.state,QString("saved_unattached"));
  QCOMPARE(result.manifest.durableFrames,quint64(4));QCOMPARE(result.manifest.fileBytes,quint64(52));
  QVERIFY(QFile::exists(stem+".wav"));QCOMPARE(contents(stem+".recovery.json"),sentinel);QCOMPARE(contents(f.stem()+".partial"),original);
 }
 void recoveredLineageRetainsOriginalFaultAndTimingWarnings()
 {
  Fixture f;QVERIFY(f.partial());ReceiptStorage receipt;RecordingControllerSnapshot input;
  input.input.epoch=f.descriptor.epoch;input.input.receivedFrames=input.input.acceptedFrames=8;
  input.input.fault=InputFault::ObservationOverflow;input.input.observationsDropped=1;
  input.observations.epoch=f.descriptor.epoch;input.observations.quality=InputTimestampQuality::Discontinuous;
  input.observations.adcIntervalDiscontinuities=3;
  WriterSnapshot written;written.state=WriterState::Incomplete;written.writtenFrames=written.removedFrames=8;written.durableFrames=4;written.fault=WriterFault::InputIncomplete;
  RecordingTakeReceipt record(f.descriptor,std::make_unique<ReceiptSink>(receipt));QVERIFY(record.append(input,written,true));
  {QFile file(f.stem()+".diagnostics.jsonl");QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));QCOMPARE(file.write(receipt.bytes),qint64(receipt.bytes.size()));}
  RecordingRecovery recovery;const auto page=recovery.scan(*f.approval.library);QCOMPARE(page.takes.size(),std::size_t(1));
  QCOMPARE(page.takes[0].originalFault,InputFault::ObservationOverflow);QVERIFY(page.takes[0].sourceFaultKnown);
  QCOMPARE(page.takes[0].timestampDiscontinuities,quint64(3));QCOMPARE(page.takes[0].observationsDropped,quint64(1));
  const auto result=recovery.recover(f.take.id);QCOMPARE(result.code,QString("recovered"));
  QCOMPARE(result.manifest.originalFault,InputFault::ObservationOverflow);QCOMPARE(result.manifest.timestampDiscontinuities,quint64(3));
  QCOMPARE(result.manifest.observationsDropped,quint64(1));QVERIFY(result.manifest.observationLossKnown);
  QCOMPARE(result.manifest.captureIntegrity,QString("incomplete"));
 }
 void recoveredSourceIdentityIsBoundIntoItsNewManifest()
 {
  Fixture f;QVERIFY(f.partial());RecordingRecovery recovery;QVERIFY(recovery.scan(*f.approval.library).code=="ok");
  auto result=recovery.recover(f.take.id);QVERIFY(result.ok());const auto target=result.manifest.descriptor.takeId;
  const auto stem=f.directory.path()+"/"+f.project.id+"/take-"+target;result.token.reset();
  const auto audio=contents(stem+".wav");QVERIFY(QFile::rename(stem+".wav",stem+".retained"));QVERIFY(saveNew(stem+".wav",audio));
  const auto page=recovery.scan(*f.approval.library);const auto found=std::find_if(page.takes.begin(),page.takes.end(),[&](const auto& m){return m.descriptor.takeId==target;});
  QVERIFY(found!=page.takes.end());QCOMPARE(found->state,QString("quarantined"));QCOMPARE(contents(stem+".retained"),audio);
 }
 void maximumLengthRecoveryExcludesTheUncheckpointedLastSecond()
 {
  Fixture f;QVERIFY(f.partial(5712000,5760000));const auto before=QCryptographicHash::hash(contents(f.stem()+".partial"),QCryptographicHash::Sha256);
  RecordingRecovery recovery;const auto page=recovery.scan(*f.approval.library);QCOMPARE(page.takes.size(),std::size_t(1));
  QCOMPARE(page.takes[0].recoverableFrames,quint64(5712000));QCOMPARE(page.takes[0].unknownTailBytes,quint64(96000));
  const auto result=recovery.recover(f.take.id);QVERIFY(result.ok());QVERIFY(result.token);QCOMPARE(result.token->facts().rawFrames,quint64(5712000));
  QCOMPARE(QCryptographicHash::hash(contents(f.stem()+".partial"),QCryptographicHash::Sha256),before);
 }
 void quarantinedAndEmptyAttemptsCanBeDismissedWithoutDeleting_data()
 {QTest::addColumn<QString>("kind");for(const auto name:{"empty","missing","corrupt","oversized"})QTest::newRow(name)<<QString(name);}
 void quarantinedAndEmptyAttemptsCanBeDismissedWithoutDeleting()
 {
  QFETCH(QString,kind);Fixture f;QVERIFY(kind=="empty"?f.partial(0,0,false,false,false):f.partial());
  if(kind=="missing")QVERIFY(QFile::rename(f.stem()+".journal",f.stem()+".retained-journal"));
  if(kind=="corrupt" || kind=="oversized"){QFile file(f.stem()+".journal");QVERIFY(file.open(QIODevice::ReadWrite));
   if(kind=="oversized")QVERIFY(file.resize(65537));else{QVERIFY(file.seek(file.size()-1));QCOMPARE(file.write("x",1),qint64(1));}}
  const auto audio=contents(f.stem()+".partial"),journal=contents(f.stem()+".journal");RecordingRecovery recovery;
  const auto page=recovery.scan(*f.approval.library);QCOMPARE(page.takes.size(),std::size_t(1));QCOMPARE(recovery.dismiss(f.take.id),QString("dismissed"));
  RecordingRecovery restarted;QVERIFY(restarted.scan(*f.approval.library).takes.empty());
  QCOMPARE(contents(f.stem()+".partial"),audio);QCOMPARE(contents(f.stem()+".journal"),journal);
 }
 void staleReceiptEpochCannotInventAKnownOriginalFault()
 {
  Fixture f;QVERIFY(f.partial());ReceiptStorage receipt;RecordingControllerSnapshot input;input.input.epoch={7,99};input.observations.epoch={7,99};
  input.input.fault=InputFault::ObservationOverflow;input.observations.quality=InputTimestampQuality::Discontinuous;
  WriterSnapshot written;written.state=WriterState::Incomplete;
  RecordingTakeReceipt record(f.descriptor,std::make_unique<ReceiptSink>(receipt));QVERIFY(record.append(input,written,true));
  {QFile file(f.stem()+".diagnostics.jsonl");QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));QCOMPARE(file.write(receipt.bytes),qint64(receipt.bytes.size()));}
  RecordingRecovery recovery;const auto page=recovery.scan(*f.approval.library);QCOMPARE(page.takes.size(),std::size_t(1));
  QCOMPARE(page.takes[0].diagnosticCode,QString("receipt_observations_invalid"));QVERIFY(!page.takes[0].sourceFaultKnown);
  QCOMPARE(page.takes[0].originalFault,InputFault::None);
 }
 void tamperedRecoveryManifestIsQuarantined()
 {
  Fixture f;QVERIFY(f.partial());RecordingRecovery recovery;QVERIFY(recovery.scan(*f.approval.library).code=="ok");
  auto recovered=recovery.recover(f.take.id);QVERIFY(recovered.ok());const auto target=recovered.manifest.descriptor.takeId;recovered.token.reset();
  const auto stem=f.directory.path()+"/"+f.project.id+"/take-"+target;auto record=QJsonDocument::fromJson(contents(stem+".recovery.json")).object();
  record["original_fault"]=int(InputFault::HardwareDiscontinuity);
  {QFile file(stem+".recovery.json");QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));const auto bytes=QJsonDocument(record).toJson(QJsonDocument::Compact);QCOMPARE(file.write(bytes),qint64(bytes.size()));}
  const auto page=recovery.scan(*f.approval.library);const auto found=std::find_if(page.takes.begin(),page.takes.end(),[&](const auto& m){return m.descriptor.takeId==target;});
  QVERIFY(found!=page.takes.end());QCOMPARE(found->state,QString("quarantined"));
 }
 void liveWriterExcludesScanRecoverAndDismissUntilItsActualRelease()
 {
  Fixture f;QVERIFY(f.partial());RecordingRecovery recovery;QVERIFY(recovery.scan(*f.approval.library).code=="ok");
  auto active=f.approval.library->createUniqueTake(f.project,{uuid(),"Active writer reservation"},48000,5760000);QVERIFY(active.sink);
  QCOMPARE(recovery.scan(*f.approval.library).code,QString("recording_busy"));
  QCOMPARE(recovery.recover(f.take.id).code,QString("recording_busy"));QCOMPARE(recovery.dismiss(f.take.id),QString("recording_busy"));
  active.sink.reset();active.receipt.reset();const auto result=recovery.recover(f.take.id);QCOMPARE(result.code,QString("recovered"));QVERIFY(result.token);
 }
 void admittedSampleRatesAreCopiedWithoutResampling_data()
 {QTest::addColumn<unsigned>("rate");QTest::newRow("44100-synthetic-only")<<44100u;QTest::newRow("48000")<<48000u;}
 void admittedSampleRatesAreCopiedWithoutResampling()
 {
  QFETCH(unsigned,rate);Fixture f;f.descriptor.sampleRate=rate;f.descriptor.maxRawFrames=std::uint64_t(rate)*120;QVERIFY(f.partial());
  RecordingRecovery recovery;const auto page=recovery.scan(*f.approval.library);QCOMPARE(page.takes.size(),std::size_t(1));QCOMPARE(page.takes[0].source.sampleRate,rate);
  const auto original=contents(f.stem()+".partial");const auto result=recovery.recover(f.take.id);QVERIFY(result.ok());QVERIFY(result.token);
  QCOMPARE(result.token->facts().sampleRate,rate);QCOMPARE(result.token->facts().rawFrames,quint64(4));
  const auto bytes=contents(f.directory.path()+"/"+f.project.id+"/take-"+result.token->facts().takeId+".wav");
  const auto header=RecordingWavHeader::encodePcm16(rate,4);QCOMPARE(bytes.left(44),QByteArray(reinterpret_cast<const char*>(header.bytes.data()),44));
  QCOMPARE(bytes.mid(44),original.mid(44,8));
 }
 void partialDestinationCreationFailureRetainsTheAssignedIdentity()
 {
  Fixture f;QVERIFY(f.partial());const auto target=uuid();const auto stem=f.directory.path()+"/"+f.project.id+"/take-"+target;
  const QByteArray sentinel("prior diagnostic receipt");QVERIFY(saveNew(stem+".diagnostics.jsonl",sentinel));
  const auto original=contents(f.stem()+".partial");RecordingRecovery recovery([target]{return target;});QVERIFY(recovery.scan(*f.approval.library).code=="ok");
  const auto failed=recovery.recover(f.take.id);QVERIFY(!failed.ok());QVERIFY(!failed.token);QCOMPARE(failed.code,QString("take_exists"));
  QCOMPARE(failed.manifest.descriptor.takeId,target);QCOMPARE(failed.manifest.recoveredFromTakeId,f.take.id);
  QVERIFY(QFile::exists(stem+".partial"));QCOMPARE(contents(stem+".diagnostics.jsonl"),sentinel);QCOMPARE(contents(f.stem()+".partial"),original);
 }
 void companionReceiptCannotClearAKnownJournalFault()
 {
  Fixture f;QVERIFY(f.partial());
  TakeProgress progress;progress.sequence=1;progress.receivedFrames=progress.acceptedFrames=progress.writtenFrames=4;progress.fault=InputFault::HardwareDiscontinuity;
  const auto encoded=RecordingTakeJournalCodec::encodeProgress(progress);QVERIFY(encoded.valid);
  {QFile file(f.stem()+".journal");QVERIFY(file.open(QIODevice::ReadWrite));const auto header=RecordingTakeJournalCodec::encodeHeader(f.descriptor);
   QVERIFY(file.resize(header.size()));QVERIFY(file.seek(header.size()));QCOMPARE(file.write(reinterpret_cast<const char*>(encoded.bytes.data()),encoded.size),qint64(encoded.size));}
  ReceiptStorage receipt;RecordingControllerSnapshot input;input.input.epoch=f.descriptor.epoch;input.observations.epoch=f.descriptor.epoch;
  input.input.receivedFrames=input.input.acceptedFrames=8;WriterSnapshot written;written.state=WriterState::Incomplete;
  RecordingTakeReceipt record(f.descriptor,std::make_unique<ReceiptSink>(receipt));QVERIFY(record.append(input,written,true));
  {QFile file(f.stem()+".diagnostics.jsonl");QVERIFY(file.open(QIODevice::WriteOnly|QIODevice::Truncate));QCOMPARE(file.write(receipt.bytes),qint64(receipt.bytes.size()));}
  RecordingRecovery recovery;const auto page=recovery.scan(*f.approval.library);QCOMPARE(page.takes.size(),std::size_t(1));
  QCOMPARE(page.takes[0].originalFault,InputFault::HardwareDiscontinuity);QCOMPARE(page.takes[0].captureIntegrity,QString("incomplete"));
  const auto recovered=recovery.recover(f.take.id);QVERIFY(recovered.ok());QCOMPARE(recovered.manifest.originalFault,InputFault::HardwareDiscontinuity);
 }
 void restartNeverArmsRecordsOrAttaches()
 {
  Fixture f;QVERIFY(f.partial());const auto files=QDir(f.directory.path()+"/"+f.project.id).entryList(QDir::Files);
  f.approval.library.reset();f.approval=RecordingLibrary::authorizeLocalRoot(f.directory.path());QVERIFY(f.approval.library);
  RecordingRecovery recovery;QCOMPARE(QDir(f.directory.path()+"/"+f.project.id).entryList(QDir::Files),files);
  const auto page=recovery.scan(*f.approval.library);QCOMPARE(page.takes.size(),std::size_t(1));
  QCOMPARE(page.takes[0].placementState,QString("saved_unattached"));QCOMPARE(page.takes[0].placementQuality,QString("uncalibrated"));
  QCOMPARE(QDir(f.directory.path()+"/"+f.project.id).entryList(QDir::Files),files);QVERIFY(!QFile::exists(f.stem()+".wav"));
 }
};
QTEST_GUILESS_MAIN(RecordingRecoveryTest)
#include "RecordingRecoveryTest.moc"
