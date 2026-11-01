#include "RecordingLibrary.h"
#include "RecordingRecovery.h"
#include "RecordingWriter.h"
#include "RecordingTakeJournal.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QUuid>
#include <QCryptographicHash>
#include <vector>
#include <QSaveFile>
#include <cstring>
#ifdef Q_OS_WIN
#include <windows.h>
#include <aclapi.h>
#include <QProcess>
#include <sddl.h>
#include <winioctl.h>
#endif
namespace {
#ifdef Q_OS_WIN
struct JunctionCleanup {QString path;~JunctionCleanup(){if(!path.isEmpty())RemoveDirectoryW(reinterpret_cast<LPCWSTR>(path.utf16()));}};
bool createOwnedJunction(const QString& link,const QString& target)
{
 if(!CreateDirectoryW(reinterpret_cast<LPCWSTR>(link.utf16()),nullptr))return false;
 const auto handle=CreateFileW(reinterpret_cast<LPCWSTR>(link.utf16()),GENERIC_READ|GENERIC_WRITE|FILE_WRITE_ATTRIBUTES,
  FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr);
 if(handle==INVALID_HANDLE_VALUE){RemoveDirectoryW(reinterpret_cast<LPCWSTR>(link.utf16()));return false;}
 auto dosTarget=target;dosTarget.replace('/','\\');const auto substitute=QStringLiteral("\\??\\")+dosTarget;
 const USHORT subLength=USHORT(substitute.size()*sizeof(wchar_t)),printLength=USHORT(dosTarget.size()*sizeof(wchar_t));
 const DWORD header=16,bytes=header+subLength+sizeof(wchar_t)+printLength+sizeof(wchar_t);
 auto storage=std::vector<BYTE>(bytes,0);auto* raw=storage.data();const ULONG tag=IO_REPARSE_TAG_MOUNT_POINT;const USHORT dataLength=USHORT(bytes-8);
 std::memcpy(raw,&tag,sizeof(tag));std::memcpy(raw+4,&dataLength,sizeof(dataLength));
 const USHORT reserved=0,subOffset=0,printOffset=USHORT(subLength+sizeof(wchar_t));
 std::memcpy(raw+6,&reserved,sizeof(reserved));std::memcpy(raw+8,&subOffset,sizeof(subOffset));std::memcpy(raw+10,&subLength,sizeof(subLength));
 std::memcpy(raw+12,&printOffset,sizeof(printOffset));std::memcpy(raw+14,&printLength,sizeof(printLength));
 std::memcpy(raw+header,substitute.utf16(),subLength);std::memcpy(raw+header+subLength+sizeof(wchar_t),dosTarget.utf16(),printLength);
 DWORD returned=0;const bool linked=DeviceIoControl(handle,FSCTL_SET_REPARSE_POINT,raw,bytes,nullptr,0,&returned,nullptr)!=FALSE;
 CloseHandle(handle);if(!linked)RemoveDirectoryW(reinterpret_cast<LPCWSTR>(link.utf16()));return linked;
}
struct RestoreAcl
{
 QString path;PSECURITY_DESCRIPTOR saved=nullptr;
 ~RestoreAcl()
 {
  if(saved){BOOL present=FALSE,defaulted=FALSE;auto* dacl=static_cast<PACL>(nullptr);
   if(GetSecurityDescriptorDacl(saved,&present,&dacl,&defaulted) && present)SetNamedSecurityInfoW(reinterpret_cast<LPWSTR>(path.data()),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,dacl,nullptr);
   LocalFree(saved);
  }
 }
 bool denyCreationAndDelete()
 {
  PACL ignored=nullptr;const auto get=GetNamedSecurityInfoW(reinterpret_cast<LPWSTR>(path.data()),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&ignored,nullptr,&saved);
  if(get!=ERROR_SUCCESS)return false;
  PSECURITY_DESCRIPTOR deny=nullptr;
  if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(D;;0x46;;;WD)",SDDL_REVISION_1,&deny,nullptr))return false;
  BOOL present=FALSE,defaulted=FALSE;auto* dacl=static_cast<PACL>(nullptr);
  const bool read=GetSecurityDescriptorDacl(deny,&present,&dacl,&defaulted) && present;
  const auto result=read?SetNamedSecurityInfoW(reinterpret_cast<LPWSTR>(path.data()),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,dacl,nullptr):ERROR_ACCESS_DENIED;
  LocalFree(deny);return result==ERROR_SUCCESS;
 }
};
#endif
}
using namespace lmms::recording;
namespace {
bool waitForWriter(RecordingWriter& writer,WriterState state)
{QElapsedTimer timer;timer.start();while(timer.elapsed()<3000){if(writer.poll().state==state)return true;QTest::qWait(1);}return writer.poll().state==state;}
TakeDescriptor takeDescriptor(const ProjectIdentity& project,const TakeIdentity& take,std::uint64_t maximum)
{TakeDescriptor d;d.projectId=project.id;d.takeId=take.id;d.label=take.label;d.epoch={1,1};d.projectGeneration=1;d.maxRawFrames=maximum;d.targetTick=0;d.sampleRate=48000;return d;}
}
class RecordingLibraryTest : public QObject
{
 Q_OBJECT
 static QString id(){return QUuid::createUuid().toString(QUuid::WithoutBraces);}
 static QByteArray bytes(const QString& path){QFile f(path);if(!f.open(QIODevice::ReadOnly))return {};return f.readAll();}
private slots:
 void recoveredTokenPinsOnlyReadOnlyNewSourceAndRevocationKeepsOriginals()
 {
  QTemporaryDir temp;QVERIFY(temp.isValid());auto approval=RecordingLibrary::authorizeLocalRoot(temp.path());QVERIFY(approval.library);
  const ProjectIdentity project{id(),1};const TakeIdentity take{id(),"recovery pin"};const auto descriptor=takeDescriptor(project,take,5760000);
  auto files=approval.library->createUniqueTake(project,take,48000,5760000);QVERIFY(files.sink);
  QCOMPARE(files.sink->beginTake(descriptor),WriterIoCode::Ok);const std::array<std::uint8_t,8> pcm{1,0,2,0,3,0,4,0};
  QCOMPARE(files.sink->appendPayload(pcm).bytesWritten,8u);TakeProgress progress;progress.sequence=1;progress.receivedFrames=progress.acceptedFrames=progress.writtenFrames=4;
  QCOMPARE(files.sink->appendCheckpoint(progress),WriterIoCode::Ok);files.sink.reset();files.receipt.reset();
  const auto original=temp.path()+"/"+project.id+"/take-"+take.id+".partial";const auto before=bytes(original);
  RecordingRecovery recovery;QVERIFY(recovery.scan(*approval.library).code=="ok");auto result=recovery.recover(take.id);QVERIFY(result.ok());QVERIFY(result.token);
  const auto recovered=temp.path()+"/"+project.id+"/take-"+result.token->facts().takeId+".wav";const auto sealed=bytes(recovered);
  {QFile writable(recovered);QVERIFY(!writable.open(QIODevice::ReadWrite));}QVERIFY(!QFile::rename(recovered,recovered+".renamed"));
  approval.library->revoke();QVERIFY(!recovery.recover(take.id).ok());QCOMPARE(bytes(original),before);QCOMPARE(bytes(recovered),sealed);
 }
 void receiptCapabilityIsCreateNewAndSeparatelyBounded()
 {
  QTemporaryDir temp;QVERIFY(temp.isValid());auto approved=RecordingLibrary::authorizeLocalRoot(temp.path());QVERIFY(approved.library);
  const ProjectIdentity project{id(),1};const TakeIdentity take{id(),"diagnostic"};
  auto created=approved.library->createUniqueTake(project,take,48000,5760000);QVERIFY(created.sink);QVERIFY(created.receipt);
  const auto file=temp.path()+"/"+project.id+"/take-"+take.id+".diagnostics.jsonl";
  QVERIFY(QFileInfo::exists(file));QVERIFY(!created.receipt->appendAndFlush(QByteArray(RecordingTakeReceipt::MaximumFileBytes+1,'x')));
  created.receipt.reset();QCOMPARE(bytes(file),QByteArray{});
 }
 void receiptCollisionNeverOverwritesExistingEvidence()
 {
  QTemporaryDir temp;QVERIFY(temp.isValid());auto approved=RecordingLibrary::authorizeLocalRoot(temp.path());QVERIFY(approved.library);
  const ProjectIdentity project{id(),1};const TakeIdentity take{id(),"collision"};QCOMPARE(approved.library->prepareProjectFolder(project),QString("ok"));
  const auto file=temp.path()+"/"+project.id+"/take-"+take.id+".diagnostics.jsonl";
  {QFile f(file);QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::NewOnly));QCOMPARE(f.write("preserve",8),qint64(8));}
  auto result=approved.library->createUniqueTake(project,take,48000,5760000);QVERIFY(!result.sink);QCOMPARE(bytes(file),QByteArray("preserve"));
 }
 void independentlyOwnedWriterSealsOnlyItsCreateNewTake()
 {
  QTemporaryDir temp;QVERIFY(temp.isValid());auto approved=RecordingLibrary::authorizeLocalRoot(temp.path());QVERIFY(approved.library);
  const ProjectIdentity project{id(),1};const TakeIdentity take{id(),"Count-in and phrase"};QVERIFY(approved.library->prepareProjectFolder(project)=="ok");
  auto created=approved.library->createUniqueTake(project,take,48000,5760000);QCOMPARE(created.code,QString("ok"));QVERIFY(created.sink);
  const auto folder=temp.path()+"/"+project.id;const auto partial=folder+"/take-"+take.id+".partial";const auto journal=folder+"/take-"+take.id+".journal";
  QVERIFY(QFileInfo::exists(partial));QVERIFY(QFileInfo::exists(journal));QCOMPARE(QFileInfo(partial).size(),qint64(0));
  RecordingRing ring{48000,131072,5760000};RecordingWriter writer;QVERIFY(writer.begin(takeDescriptor(project,take,5760000),std::move(created.sink),ring).ok);
  QVERIFY(waitForWriter(writer,WriterState::Ready));QCOMPARE(QFileInfo(partial).size(),qint64(44));QVERIFY(QFileInfo(journal).size()>0 && QFileInfo(journal).size()<=256);
  const float samples[]{-.5f,0.f,.5f};QCOMPARE(ring.push(samples,3,1,0).acceptedFrames,3u);
  InputSnapshot end;end.epoch={1,1};end.receivedFrames=3;end.acceptedFrames=3;writer.requestInputClosed({1,1},end);
  QVERIFY(waitForWriter(writer,WriterState::Sealed));QVERIFY(writer.joinAfterExit().ok);
  const auto final=folder+"/take-"+take.id+".wav";QVERIFY(QFileInfo::exists(final));QVERIFY(!QFileInfo::exists(partial));QVERIFY(QFileInfo(journal).size()>44);
  QFile sealed(final);QVERIFY2(sealed.open(QIODevice::ReadOnly),qPrintable(sealed.errorString()));const auto audio=sealed.readAll();
  QCOMPARE(audio.size(),qint64(50));QCOMPARE(audio.left(4),QByteArray("RIFF"));QCOMPARE(audio.mid(36,4),QByteArray("data"));
  QCOMPARE(std::uint8_t(audio[44]),std::uint8_t(0));QCOMPARE(std::uint8_t(audio[45]),std::uint8_t(0xc0));
  const auto digest=QCryptographicHash::hash(audio,QCryptographicHash::Sha256);QCOMPARE(writer.poll().sourceSha256,digest);
  QCOMPARE(QCryptographicHash::hash(bytes(final),QCryptographicHash::Sha256),digest);
  QFile journalFile(journal);QVERIFY(journalFile.open(QIODevice::ReadOnly));const auto journalBytes=journalFile.readAll();
  const auto le16=[](const char* p){return std::uint16_t(std::uint8_t(p[0]))|std::uint16_t(std::uint8_t(p[1]))<<8;};
  QVERIFY(journalBytes.size()<=65536);const auto headerSize=le16(journalBytes.constData()+2);TakeDescriptor saved;
  QVERIFY(headerSize<=TakeJournalRecordMaximum && headerSize<=journalBytes.size());QVERIFY(RecordingTakeJournalCodec::validateHeader(journalBytes.left(headerSize),saved));
  QCOMPARE(saved.takeId,take.id);TakeJournalCursor cursor;std::vector<std::uint8_t> types;std::size_t offset=headerSize;
  while(offset<std::size_t(journalBytes.size()))
  {
   QVERIFY(offset+4<=std::size_t(journalBytes.size()));const auto length=le16(journalBytes.constData()+offset+2);
   QVERIFY(length<=TakeJournalRecordMaximum && offset+length<=std::size_t(journalBytes.size()));
   const auto* record=reinterpret_cast<const std::uint8_t*>(journalBytes.constData()+offset);
   QVERIFY(RecordingTakeJournalCodec::validateRecord(record,length,cursor));types.push_back(record[0]);offset+=length;
  }
  QCOMPARE(types.size(),std::size_t(3));QCOMPARE(types[0],std::uint8_t(2));QCOMPARE(types[1],std::uint8_t(3));QCOMPARE(types[2],std::uint8_t(3));QVERIFY(cursor.lastProgress.sourceDigestKnown);
  QCOMPARE(QByteArray(reinterpret_cast<const char*>(cursor.lastProgress.sourceDigest.data()),32),digest);
 }
 void existingFinalIsNeverReplacedByWriterSeal()
 {
  QTemporaryDir temp;QVERIFY(temp.isValid());auto approved=RecordingLibrary::authorizeLocalRoot(temp.path());QVERIFY(approved.library);
  const ProjectIdentity project{id(),1};const TakeIdentity take{id(),"collision"};QVERIFY(approved.library->prepareProjectFolder(project)=="ok");
  const auto folder=temp.path()+"/"+project.id;const auto final=folder+"/take-"+take.id+".wav";const QByteArray sentinel("protected prior audio");
  {QFile f(final);QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::NewOnly));QCOMPARE(f.write(sentinel),qint64(sentinel.size()));}
  auto created=approved.library->createUniqueTake(project,take,48000,5760000);QVERIFY(created.sink);
  RecordingRing ring{48000,131072,5760000};RecordingWriter writer;QVERIFY(writer.begin(takeDescriptor(project,take,5760000),std::move(created.sink),ring).ok);
  QVERIFY(waitForWriter(writer,WriterState::Ready));const float sample[]{.25f};QCOMPARE(ring.push(sample,1,1,0).acceptedFrames,1u);
  InputSnapshot end;end.epoch={1,1};end.receivedFrames=1;end.acceptedFrames=1;writer.requestInputClosed({1,1},end);
  QVERIFY(waitForWriter(writer,WriterState::Incomplete));QVERIFY(writer.joinAfterExit().ok);
  QCOMPARE(bytes(final),sentinel);QVERIFY(QFileInfo::exists(folder+"/take-"+take.id+".partial"));QVERIFY(!writer.poll().audioSealed);
 }
 void locallyAuthorizedRootCreatesOnlyNewOpaquePartial()
 {
  QTemporaryDir temp;QVERIFY(temp.isValid());auto approved=RecordingLibrary::authorizeLocalRoot(temp.path());QCOMPARE(approved.code,QString("ok"));QVERIFY(approved.library);
  const ProjectIdentity project{id(),1};QCOMPARE(approved.library->prepareProjectFolder(project),QString("ok"));
  const TakeIdentity take{id(),"Hook / ../ user label"};auto created=approved.library->createUniquePartial(project,take);QCOMPARE(created.code,QString("ok"));QVERIFY(created.file);
  QCOMPARE(created.file->append(QByteArray("pcm-data")),qint64(8));QVERIFY(created.file->flush());created.file.reset();
  QCOMPARE(bytes(temp.filePath(project.id+"/take-"+take.id+".partial")),QByteArray("pcm-data"));
  QCOMPARE(QDir(temp.path()).entryList(QDir::Dirs|QDir::NoDotAndDotDot),QStringList{project.id});
 }
 void existingTakeCannotBeClobberedOrReopenedWritable()
 {
  QTemporaryDir temp;auto approved=RecordingLibrary::authorizeLocalRoot(temp.path());QVERIFY(approved.library);ProjectIdentity project{id(),1};QCOMPARE(approved.library->prepareProjectFolder(project),QString("ok"));TakeIdentity take{id(),"original"};
  auto original=approved.library->createUniquePartial(project,take);QVERIFY(original.file);QCOMPARE(original.file->append("prior-original"),qint64(14));QVERIFY(original.file->flush());original.file.reset();
  const auto path=temp.filePath(project.id+"/take-"+take.id+".partial");const auto before=QCryptographicHash::hash(bytes(path),QCryptographicHash::Sha256);
  const auto collision=approved.library->createUniquePartial(project,take);QCOMPARE(collision.code,QString("take_exists"));QVERIFY(!collision.file);
  QCOMPARE(QCryptographicHash::hash(bytes(path),QCryptographicHash::Sha256),before);
  auto checked=approved.library->openRecoveryCandidate(project,take.id);QCOMPARE(checked.code,QString("ok"));QVERIFY(checked.source);QCOMPARE(checked.source->readAll(),QByteArray("prior-original"));QTest::ignoreMessage(QtWarningMsg,"QIODevice::write (QIODevice): ReadOnly device");QCOMPARE(checked.source->write("bad",3),qint64(-1));
 }
 void malformedIdsAndMissingRootCannotWrite()
 {
  QTemporaryDir temp;
  QCOMPARE(RecordingLibrary::authorizeLocalRoot(temp.filePath("missing")).code,QString("unauthorized_library"));
  auto approved=RecordingLibrary::authorizeLocalRoot(temp.path());QVERIFY(approved.library);
  QCOMPARE(approved.library->prepareProjectFolder({"../outside",1}),QString("invalid_identity"));
  QCOMPARE(approved.library->prepareProjectFolder({id(),2}),QString("unsupported_schema"));
  QCOMPARE(approved.library->createUniquePartial({id(),1},{"../outside","label"}).code,QString("invalid_identity"));
  QCOMPARE(QDir(temp.path()).entryList(QDir::Files|QDir::Dirs|QDir::NoDotAndDotDot),QStringList{"recording-library.lock"});
 }
 void revocationStopsOwnedWriterWithoutDestroyingBytes()
 {
  QTemporaryDir temp;auto approved=RecordingLibrary::authorizeLocalRoot(temp.path());QVERIFY(approved.library);ProjectIdentity project{id(),1};QCOMPARE(approved.library->prepareProjectFolder(project),QString("ok"));TakeIdentity take{id(),"take"};auto created=approved.library->createUniquePartial(project,take);QVERIFY(created.file);
  QCOMPARE(created.file->append("safe"),qint64(4));QVERIFY(created.file->flush());approved.library->revoke();
  QCOMPARE(created.file->append("bad"),qint64(-1));QCOMPARE(approved.library->createUniquePartial(project,{id(),"next"}).code,QString("unauthorized_library"));created.file.reset();
  QCOMPARE(bytes(temp.filePath(project.id+"/take-"+take.id+".partial")),QByteArray("safe"));
 }
 void recoveryLookupNeverCreatesUnknownProject()
 {
  QTemporaryDir temp;auto approved=RecordingLibrary::authorizeLocalRoot(temp.path());QVERIFY(approved.library);
  const auto before=QDir(temp.path()).entryList(QDir::Files|QDir::Dirs|QDir::NoDotAndDotDot);
  auto result=approved.library->openRecoveryCandidate({id(),1},id());QVERIFY(!result.source);
  QCOMPARE(QDir(temp.path()).entryList(QDir::Files|QDir::Dirs|QDir::NoDotAndDotDot),before);
 }
 void manifestTamperingAndFutureVersionFailWithoutOverwrite()
 {
  QTemporaryDir temp;const ProjectIdentity project{id(),1};QVERIFY(QDir(temp.path()).mkdir(project.id));const auto manifest=temp.filePath(project.id+"/project.json");
  const QByteArray future="{\"schema\":2,\"project_id\":\""+project.id.toUtf8()+"\"}";
  {QFile f(manifest);QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::NewOnly));QCOMPARE(f.write(future),qint64(future.size()));}
  auto approved=RecordingLibrary::authorizeLocalRoot(temp.path());QVERIFY(approved.library);QCOMPARE(approved.library->prepareProjectFolder(project),QString("unsupported_schema"));QCOMPARE(bytes(manifest),future);
 }
 void pinnedRootAndProjectCannotBeRenamedDuringWriting()
 {
#ifdef Q_OS_WIN
  QTemporaryDir temp;const auto root=temp.filePath("library");QVERIFY(QDir(temp.path()).mkdir("library"));auto approved=RecordingLibrary::authorizeLocalRoot(root);QVERIFY(approved.library);ProjectIdentity project{id(),1};QCOMPARE(approved.library->prepareProjectFolder(project),QString("ok"));
  QVERIFY(!MoveFileW(reinterpret_cast<LPCWSTR>(root.utf16()),reinterpret_cast<LPCWSTR>(temp.filePath("moved").utf16())));
  const auto folder=root+'/'+project.id;QVERIFY(!MoveFileW(reinterpret_cast<LPCWSTR>(folder.utf16()),reinterpret_cast<LPCWSTR>((root+"/moved-project").utf16())));
  auto take=approved.library->createUniquePartial(project,{id(),"safe"});QCOMPARE(take.code,QString("ok"));
#else
  QSKIP("Windows identity gate");
#endif
 }
 void rootHasSingleWriterOwnership()
 {
  QTemporaryDir temp;auto first=RecordingLibrary::authorizeLocalRoot(temp.path());QVERIFY(first.library);
  auto second=RecordingLibrary::authorizeLocalRoot(temp.path());QVERIFY(!second.library);
  first.library.reset();auto next=RecordingLibrary::authorizeLocalRoot(temp.path());QVERIFY(next.library);
 }
 void boundedFolderHandleCacheRetainsOldTakeFiles()
 {
  QTemporaryDir temp;QVERIFY(QDir(temp.path()).mkdir("library"));auto approved=RecordingLibrary::authorizeLocalRoot(temp.filePath("library"));QVERIFY(approved.library);
  const ProjectIdentity first{id(),1};const TakeIdentity prior{id(),"old-take"};auto created=approved.library->createUniquePartial(first,prior);QVERIFY(created.file);QCOMPARE(created.file->append("retained-across-projects"),qint64(24));QVERIFY(created.file->flush());created.file.reset();
  for(int i=0;i<70;++i){const ProjectIdentity project{id(),1};QCOMPARE(approved.library->prepareProjectFolder(project),QString("ok"));}
  auto reopened=approved.library->openRecoveryCandidate(first,prior.id);QCOMPARE(reopened.code,QString("ok"));QVERIFY(reopened.source);QCOMPARE(reopened.source->readAll(),QByteArray("retained-across-projects"));
 }
 void selectedRootDoesNotBlockSiblingAtomicProjectSave()
 {
  QTemporaryDir temp;QVERIFY(QDir(temp.path()).mkdir("library"));auto approved=RecordingLibrary::authorizeLocalRoot(temp.filePath("library"));QVERIFY(approved.library);
  QSaveFile project(temp.filePath("sibling.mmp"));QVERIFY(project.open(QIODevice::WriteOnly));QCOMPARE(project.write("safe-project"),qint64(12));
  QVERIFY2(project.commit(),qPrintable(project.errorString()));QCOMPARE(bytes(temp.filePath("sibling.mmp")),QByteArray("safe-project"));
 }
 void selectedRootDoesNotBlockItsNativeProjectSave()
 {
  QTemporaryDir temp;auto approved=RecordingLibrary::authorizeLocalRoot(temp.path());QVERIFY(approved.library);
  QSaveFile project(temp.filePath("native.mmp"));QVERIFY(project.open(QIODevice::WriteOnly));QCOMPARE(project.write("safe-project"),qint64(12));
  QVERIFY2(project.commit(),qPrintable(project.errorString()));QCOMPARE(bytes(temp.filePath("native.mmp")),QByteArray("safe-project"));
 }
 void junctionProjectFolderCannotEscapeRecordingLibrary()
 {
#ifdef Q_OS_WIN
  QTemporaryDir temp;QVERIFY(temp.isValid());QVERIFY(QDir(temp.path()).mkdir("library"));QVERIFY(QDir(temp.path()).mkdir("outside"));
  const auto outside=temp.filePath("outside"),keep=outside+"/preserve.wav";
  {QFile f(keep);QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::NewOnly));QCOMPARE(f.write("unchanged-outside"),qint64(17));}
  const auto digest=QCryptographicHash::hash(bytes(keep),QCryptographicHash::Sha256);
  const auto project=ProjectIdentity{id(),1};const auto junction=temp.filePath("library/"+project.id);JunctionCleanup cleanup{junction};QVERIFY(createOwnedJunction(junction,outside));
  auto approved=RecordingLibrary::authorizeLocalRoot(temp.filePath("library"));QVERIFY(approved.library);
  QCOMPARE(approved.library->prepareProjectFolder(project),QString("unauthorized_library"));
  QCOMPARE(approved.library->createUniquePartial(project,{id(),"must stay inside"}).code,QString("unauthorized_library"));
  QCOMPARE(approved.library->openRecoveryCandidate(project,id()).code,QString("unauthorized_library"));
  QCOMPARE(QDir(outside).entryList(QDir::Files|QDir::NoDotAndDotDot),QStringList{"preserve.wav"});
  QCOMPARE(QCryptographicHash::hash(bytes(keep),QCryptographicHash::Sha256),digest);
#endif
 }
 void selectedRootReparsePointIsRejected()
 {
#ifdef Q_OS_WIN
  QTemporaryDir temp;QVERIFY(temp.isValid());QVERIFY(QDir(temp.path()).mkdir("outside"));const auto junction=temp.filePath("root-link");JunctionCleanup cleanup{junction};
  QVERIFY(createOwnedJunction(junction,temp.filePath("outside")));
  const auto approved=RecordingLibrary::authorizeLocalRoot(junction);QCOMPARE(approved.code,QString("unauthorized_library"));QVERIFY(!approved.library);
  QVERIFY(QDir(temp.filePath("outside")).entryList(QDir::Files|QDir::NoDotAndDotDot).isEmpty());
#endif
 }
 void accessDeniedCannotWritePartialOrReplacePriorTake()
 {
#ifdef Q_OS_WIN
  QTemporaryDir temp;QVERIFY(temp.isValid());QVERIFY(QDir(temp.path()).mkdir("library"));auto approved=RecordingLibrary::authorizeLocalRoot(temp.filePath("library"));QVERIFY(approved.library);
  ProjectIdentity project{id(),1};QCOMPARE(approved.library->prepareProjectFolder(project),QString("ok"));TakeIdentity prior{id(),"protected"};auto old=approved.library->createUniquePartial(project,prior);QVERIFY(old.file);QCOMPARE(old.file->append("prior-original"),qint64(14));QVERIFY(old.file->flush());old.file.reset();
  const auto projectDir=temp.filePath("library/"+project.id);auto checked=approved.library->openRecoveryCandidate(project,prior.id);QCOMPARE(checked.code,QString("ok"));QVERIFY(checked.source);
  const auto before=checked.source->readAll();QCOMPARE(QCryptographicHash::hash(before,QCryptographicHash::Sha256),QCryptographicHash::hash("prior-original",QCryptographicHash::Sha256));
  RestoreAcl acl{projectDir};QVERIFY(acl.denyCreationAndDelete());QCOMPARE(approved.library->createUniquePartial(project,{id(),"denied"}).code,QString("write_failed"));
  QVERIFY(checked.source->seek(0));QCOMPARE(checked.source->readAll(),before);
#else
  QSKIP("Windows filesystem ACL fixture");
#endif
 }
};
QTEST_GUILESS_MAIN(RecordingLibraryTest)
#include "RecordingLibraryTest.moc"
