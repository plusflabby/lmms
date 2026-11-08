#include "RecordingProjectIdentity.h"
#include "Engine.h"
#include "Song.h"
#include "DataFile.h"
#include "ProjectJournal.h"
#include "ConfigManager.h"
#include "ProjectSavePublication.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QCryptographicHash>
#ifdef Q_OS_WIN
#include <windows.h>
#include <wincred.h>
#endif
using namespace lmms;
using namespace lmms::recording;
class RecordingProjectIdentityTest : public QObject
{
 Q_OBJECT
 static QByteArray contents(const QString& path){QFile f(path);if(!f.open(QIODevice::ReadOnly))return {};return f.readAll();}
#ifdef Q_OS_WIN
 struct TemporaryCredential{QString name;bool owned=false;~TemporaryCredential(){if(owned)CredDeleteW(reinterpret_cast<LPCWSTR>(name.utf16()),CRED_TYPE_GENERIC,0);}};
 static QString nativeSaveCredential(const QString& directory){
  const auto h=CreateFileW(reinterpret_cast<LPCWSTR>(directory.utf16()),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
  if(h==INVALID_HANDLE_VALUE)return {};
  BY_HANDLE_FILE_INFORMATION i{};const bool ok=GetFileInformationByHandle(h,&i);CloseHandle(h);if(!ok)return {};
  const auto material=QByteArray::number(qulonglong(i.dwVolumeSerialNumber))+':'+QByteArray::number(qulonglong(i.nFileIndexHigh))+':'+QByteArray::number(qulonglong(i.nFileIndexLow));
  return QStringLiteral("LMMS/NativeProjectSave/v1/")+QString::fromLatin1(QCryptographicHash::hash(material,QCryptographicHash::Sha256).toHex());
 }
#endif
private slots:
 void initTestCase(){Engine::init(true);}
 void cleanupTestCase(){Engine::destroy();}
 void init()
 {
  QTemporaryDir fixture;const auto path=fixture.filePath("empty.mpt");DataFile empty(DataFile::Type::SongProjectTemplate);QVERIFY(empty.writeFile(path));
  Engine::getSong()->createNewProjectFromTemplate(path);QVERIFY(Engine::getSong()->projectFileName().isEmpty());
 }
 void cleanup(){Engine::getSong()->clearProject();}
 void unsavedSaveReloadAndPhysicalRenameRetainProjectFolder()
 {
  QTemporaryDir temp;QVERIFY(QDir(temp.path()).mkdir("library"));auto library=RecordingLibrary::authorizeLocalRoot(temp.filePath("library"));QVERIFY(library.library);
  auto* song=Engine::getSong();const auto initial=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);QCOMPARE(initial.code,QString("ok"));QVERIFY(!initial.identity.id.isEmpty());
  const auto file=temp.filePath("first.mmp");QVERIFY2(song->guiSaveProjectAs(file),qPrintable(song->recordingProjectIdentity().lastCode()));QCOMPARE(song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library).identity.id,initial.identity.id);
  song->loadProject(file);QCOMPARE(song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library).identity.id,initial.identity.id);
  const auto renamed=temp.filePath("renamed.mmp");QVERIFY(QFile::rename(file,renamed));song->loadProject(renamed);
  QCOMPARE(song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library).identity.id,initial.identity.id);
  QVERIFY(QDir(temp.filePath("library/"+initial.identity.id)).exists());
 }
 void distinctSaveAsForksWithoutChangingOldProjectOrTake()
 {
  QTemporaryDir temp;QVERIFY(QDir(temp.path()).mkdir("library"));auto library=RecordingLibrary::authorizeLocalRoot(temp.filePath("library"));QVERIFY(library.library);auto* song=Engine::getSong();
  const auto first=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);QCOMPARE(first.code,QString("ok"));const auto original=temp.filePath("original.mmp");QVERIFY(song->guiSaveProjectAs(original));
  const auto originalBytes=contents(original);const auto takeId=QUuid::createUuid().toString(QUuid::WithoutBraces);auto take=library.library->createUniquePartial(first.identity,{takeId,"original"});QVERIFY(take.file);QCOMPARE(take.file->append("protected-audio"),qint64(15));QVERIFY(take.file->flush());take.file.reset();
  const auto oldTake=temp.filePath("library/"+first.identity.id+"/take-"+takeId+".partial");const auto digest=QCryptographicHash::hash(contents(oldTake),QCryptographicHash::Sha256);
  QVERIFY(song->guiSaveProjectAs(temp.filePath("distinct.mmp")));const auto second=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);
  QCOMPARE(second.code,QString("ok"));QVERIFY(second.identity.id!=first.identity.id);QCOMPARE(contents(original),originalBytes);QCOMPARE(QCryptographicHash::hash(contents(oldTake),QCryptographicHash::Sha256),digest);
 }
 void recoveryAutosaveDoesNotRebindTheLiveProjectIdentity()
 {
  QTemporaryDir temp;QVERIFY(temp.isValid());const auto libraryPath=temp.filePath("library");QVERIFY(QDir().mkpath(libraryPath));
  auto library=RecordingLibrary::authorizeLocalRoot(libraryPath);QVERIFY(library.library);auto* song=Engine::getSong();
  const auto initial=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);QCOMPARE(initial.code,QString("ok"));
  const auto source=temp.filePath("disposable.mmp");QVERIFY(song->guiSaveProjectAs(source));
  const auto sourceBytes=contents(source);const auto activeIdentity=song->recordingProjectIdentity().cachedIdentity();
  const auto activeRevision=song->recordingProjectIdentity().revision();const auto activeFileName=song->projectFileName();
  auto* config=ConfigManager::inst();const auto oldWorkingDir=config->workingDir();
  const auto temporaryWorkingDir=temp.filePath("working");QVERIFY(QDir().mkpath(temporaryWorkingDir));config->setWorkingDir(temporaryWorkingDir);
  const auto recoveryPath=config->recoveryFile();const bool recoverySaved=song->saveProjectFile(recoveryPath,false,Song::ProjectSavePurpose::RecoverySnapshot);config->setWorkingDir(oldWorkingDir);
  QVERIFY(recoverySaved);QCOMPARE(song->projectFileName(),activeFileName);
  QCOMPARE(song->recordingProjectIdentity().cachedIdentity().id,activeIdentity.id);
  QCOMPARE(song->recordingProjectIdentity().revision(),activeRevision);
  QCOMPARE(contents(source),sourceBytes);
  DataFile recovery(recoveryPath);QVERIFY(recovery.head().firstChildElement("recording_project").isNull());
 }
 void recoverySnapshotAtActiveRecoveryPathKeepsItsIdentityTrusted()
 {
  QTemporaryDir temp;QVERIFY(temp.isValid());const auto libraryPath=temp.filePath("library");QVERIFY(QDir().mkpath(libraryPath));
  auto library=RecordingLibrary::authorizeLocalRoot(libraryPath);QVERIFY(library.library);auto* song=Engine::getSong();
  const auto initial=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);QCOMPARE(initial.code,QString("ok"));
  const auto recoveryPath=temp.filePath("recover.mmp");QVERIFY(song->guiSaveProjectAs(recoveryPath));
  const auto activeIdentity=song->recordingProjectIdentity().cachedIdentity();const auto activeRevision=song->recordingProjectIdentity().revision();
  QVERIFY(song->recordingProjectIdentity().sourceIdentityTrusted());
  QVERIFY(song->saveProjectFile(recoveryPath,false,Song::ProjectSavePurpose::RecoverySnapshot));
  QCOMPARE(song->projectFileName(),recoveryPath);QCOMPARE(song->recordingProjectIdentity().revision(),activeRevision);
  const auto rebound=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);
  QCOMPARE(rebound.code,QString("ok"));QCOMPARE(rebound.identity.id,activeIdentity.id);
  QVERIFY(song->recordingProjectIdentity().sourceIdentityTrusted());
  QVERIFY2(song->guiSaveProject(),qPrintable(song->recordingProjectIdentity().lastCode()));
 }
 void duplicatedSerializedUuidGetsSeparateFolder()
 {
  QTemporaryDir temp;QVERIFY(QDir(temp.path()).mkdir("library"));auto library=RecordingLibrary::authorizeLocalRoot(temp.filePath("library"));QVERIFY(library.library);auto* song=Engine::getSong();
  const auto first=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);QCOMPARE(first.code,QString("ok"));const auto original=temp.filePath("source.mmp");QVERIFY(song->guiSaveProjectAs(original));
  const auto copy=temp.filePath("copy.mmp");QVERIFY(QFile::copy(original,copy));song->loadProject(copy);const auto copied=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);
  QCOMPARE(copied.code,QString("ok"));QVERIFY(copied.identity.id!=first.identity.id);QCOMPARE(contents(original),contents(copy));
 }
 void loadedCopyCanSaveWithoutReauthorizingTheRecordingLibrary()
 {
  QTemporaryDir temp;QVERIFY(temp.isValid());QVERIFY(QDir(temp.path()).mkdir("library"));
  auto library=RecordingLibrary::authorizeLocalRoot(temp.filePath("library"));QVERIFY(library.library);auto* song=Engine::getSong();
  const auto original=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);QCOMPARE(original.code,QString("ok"));
  const auto source=temp.filePath("source.mmp");QVERIFY(song->guiSaveProjectAs(source));const auto sourceBytes=contents(source);
  const auto copy=temp.filePath("copy.mmp");QVERIFY(QFile::copy(source,copy));song->loadProject(copy);
  QCOMPARE(song->recordingProjectIdentity().cachedIdentity().id,original.identity.id);
  QVERIFY2(song->guiSaveProject(),qPrintable(song->recordingProjectIdentity().lastCode()));
  const auto copied=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);
  QCOMPARE(copied.code,QString("ok"));QVERIFY(copied.identity.id!=original.identity.id);QCOMPARE(contents(source),sourceBytes);
 }
 void legacyProjectWithoutRecordingDoesNotAcquireMetadata()
 {
  QTemporaryDir temp;auto* song=Engine::getSong();const auto path=temp.filePath("legacy.mmp");QVERIFY(song->saveProjectFile(path));DataFile file(path);
  QVERIFY(file.head().firstChildElement("recording_project").isNull());song->loadProject(path);QVERIFY(song->saveProjectFile(path));DataFile again(path);QVERIFY(again.head().firstChildElement("recording_project").isNull());
 }
 void unknownCriticalSchemaCannotReplaceCurrentProjectOrItsIdentity()
 {
  QTemporaryDir temp;QVERIFY(QDir(temp.path()).mkdir("library"));auto library=RecordingLibrary::authorizeLocalRoot(temp.filePath("library"));QVERIFY(library.library);auto* song=Engine::getSong();
  const auto originalIdentity=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);QCOMPARE(originalIdentity.code,QString("ok"));const auto source=temp.filePath("current.mmp");QVERIFY(song->guiSaveProjectAs(source));
  const auto original=contents(source);DataFile future(DataFile::Type::SongProject);auto node=future.createElement("recording_project");node.setAttribute("schema",2);node.setAttribute("id",QUuid::createUuid().toString(QUuid::WithoutBraces));node.setAttribute("nonce",QUuid::createUuid().toString(QUuid::WithoutBraces));future.head().appendChild(node);
  const auto unsupported=temp.filePath("future.mmp");QVERIFY(future.writeFile(unsupported));const auto protectedFuture=contents(unsupported);
  auto* journal=Engine::projectJournal();journal->setJournalling(false);song->loadProject(unsupported);
  QVERIFY(!journal->isJournalling());journal->setJournalling(true);
  QCOMPARE(song->projectFileName(),source);QCOMPARE(contents(unsupported),protectedFuture);QCOMPARE(contents(source),original);
  QCOMPARE(song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library).identity.id,originalIdentity.identity.id);
 }
 void previousIdentityRemainsReadableDuringProjectChangeSignal()
 {
  QTemporaryDir temp;QVERIFY(QDir(temp.path()).mkdir("library"));auto library=RecordingLibrary::authorizeLocalRoot(temp.filePath("library"));QVERIFY(library.library);auto* song=Engine::getSong();
  const auto prior=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);QCOMPARE(prior.code,QString("ok"));const auto source=temp.filePath("recorded-project.mmp");QVERIFY(song->guiSaveProjectAs(source));
  DataFile legacy(DataFile::Type::SongProject);const auto target=temp.filePath("legacy-target.mmp");QVERIFY(legacy.writeFile(target));QString observed;
  const auto connection=QObject::connect(song,&Song::projectAboutToChange,song,[&]{observed=song->recordingProjectIdentity().cachedIdentity().id;},Qt::DirectConnection);
  song->loadProject(target);QObject::disconnect(connection);QCOMPARE(observed,prior.identity.id);QVERIFY(song->recordingProjectIdentity().cachedIdentity().id.isEmpty());
 }
 void plainAndCompressedNativeSavePreserveUnicodeTrackMetadata()
 {
  QTemporaryDir temp;QVERIFY(QDir(temp.path()).mkdir("library"));auto library=RecordingLibrary::authorizeLocalRoot(temp.filePath("library"));QVERIFY(library.library);auto* song=Engine::getSong();
  auto* track=Track::create(Track::Type::Sample,song);QVERIFY(track);track->setName(QString::fromUtf8("Hook \xCE\xA9"));
  for(const auto& suffix:{QString("mmp"),QString("mmpz")}){
   const auto path=temp.filePath("Unicode."+suffix);const auto current=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);QCOMPARE(current.code,QString("ok"));
   QVERIFY2(song->guiSaveProjectAs(path),qPrintable(song->recordingProjectIdentity().lastCode()));const auto saved=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);QVERIFY(saved.code=="ok");
   if(suffix=="mmp")QCOMPARE(saved.identity.id,current.identity.id);else QVERIFY(saved.identity.id!=current.identity.id);
   DataFile data(path);QVERIFY(!data.head().firstChildElement("recording_project").isNull());
   QCOMPARE(data.content().firstChildElement("trackcontainer").firstChildElement("track").attribute("name"),QString::fromUtf8("Hook \xCE\xA9"));
   song->loadProject(path);QCOMPARE(song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library).identity.id,saved.identity.id);
  }
 }
 void serializedProjectPayloadMatchesNativeWriter()
 {
  QTemporaryDir temp;QVERIFY(temp.isValid());
  for(const auto& suffix:{QString("mmp"),QString("mmpz")}){
   const auto path=temp.filePath("payload."+suffix);DataFile data(DataFile::Type::SongProject);
   data.head().setAttribute("fixture",QString::fromUtf8("Hook \xCE\xA9"));QByteArray payload;
   QVERIFY(data.serializeProjectForSave(path,payload));QVERIFY(!payload.isEmpty());
   QVERIFY(data.writeFile(path));QCOMPARE(contents(path),payload);
  }
 }
 void recordingIdentityCommitsWhilePublishedFileIsPinned()
 {
#ifndef Q_OS_WIN
  QSKIP("Windows handle-bound publication");
#else
  QTemporaryDir temp;QVERIFY(temp.isValid());QVERIFY(QDir(temp.path()).mkdir("library"));
  auto library=RecordingLibrary::authorizeLocalRoot(temp.filePath("library"));QVERIFY(library.library);auto* song=Engine::getSong();
  const auto prior=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);QCOMPARE(prior.code,QString("ok"));
  const auto source=temp.filePath("source.mmp");QVERIFY(song->guiSaveProjectAs(source));
  const auto target=temp.filePath("target.mmp");DataFile previous(DataFile::Type::SongProject);QVERIFY(previous.writeFile(target));
  TemporaryCredential credential;credential.name=nativeSaveCredential(temp.path());QVERIFY(!credential.name.isEmpty());PCREDENTIALW existing=nullptr;
  const bool exists=CredReadW(reinterpret_cast<LPCWSTR>(credential.name.utf16()),CRED_TYPE_GENERIC,0,&existing);
  const auto error=exists?DWORD(ERROR_SUCCESS):GetLastError();if(existing)CredFree(existing);QVERIFY(!exists);QCOMPARE(error,DWORD(ERROR_NOT_FOUND));credential.owned=true;
  DataFile data(DataFile::Type::SongProject);QVERIFY(song->recordingProjectIdentity().beginSave(data,target,false));
  QByteArray payload;QVERIFY(data.serializeProjectForSave(target,payload));
  auto held=ProjectSavePublication::acquireSelectedTarget(target);QVERIFY(held.publication);
  const auto tx=QUuid::createUuid().toString(QUuid::WithoutBraces);
  QCOMPARE(held.publication->createStage(tx,payload),QString("ok"));QCOMPARE(held.publication->prepareNativeJournal(),QString("ok"));
  QCOMPARE(held.publication->publishNativePending(),QString("ok"));
  QVERIFY2(song->recordingProjectIdentity().finishSave(target,true,*held.publication),qPrintable(song->recordingProjectIdentity().lastCode()));
  const auto savedId=song->recordingProjectIdentity().cachedIdentity().id;QVERIFY(!savedId.isEmpty());QVERIFY(savedId!=prior.identity.id);
  QCOMPARE(ProjectSavePublication::nativeJournalStatus(target),QString("pending"));
  QCOMPARE(held.publication->finalizeNativePublication(),QString("ok"));held.publication.reset();
  QCOMPARE(song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library).identity.id,savedId);
#endif
 }
 void uncertainPublishedSaveKeepsRecordingIdentityBlocked()
 {
#ifndef Q_OS_WIN
  QSKIP("Windows handle-bound publication");
#else
  QTemporaryDir temp;QVERIFY(temp.isValid());QVERIFY(QDir(temp.path()).mkdir("library"));
  auto library=RecordingLibrary::authorizeLocalRoot(temp.filePath("library"));QVERIFY(library.library);auto* song=Engine::getSong();
  const auto prior=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);QCOMPARE(prior.code,QString("ok"));
  const auto source=temp.filePath("source.mmp");QVERIFY(song->guiSaveProjectAs(source));const auto original=contents(source);
  TemporaryCredential credential;credential.name=nativeSaveCredential(temp.path());QVERIFY(!credential.name.isEmpty());PCREDENTIALW existing=nullptr;
  const bool exists=CredReadW(reinterpret_cast<LPCWSTR>(credential.name.utf16()),CRED_TYPE_GENERIC,0,&existing);
  const auto error=exists?DWORD(ERROR_SUCCESS):GetLastError();if(existing)CredFree(existing);QVERIFY(!exists);QCOMPARE(error,DWORD(ERROR_NOT_FOUND));credential.owned=true;
  DataFile data(DataFile::Type::SongProject);QVERIFY(song->recordingProjectIdentity().beginSave(data,source,false));
  QByteArray payload;QVERIFY(data.serializeProjectForSave(source,payload));
  auto held=ProjectSavePublication::acquireSelectedTarget(source);QVERIFY(held.publication);
  const auto tx=QUuid::createUuid().toString(QUuid::WithoutBraces);
  QCOMPARE(held.publication->createStage(tx,payload),QString("ok"));QCOMPARE(held.publication->prepareNativeJournal(),QString("ok"));
  QCOMPARE(held.publication->publishNativePending(),QString("ok"));
  QVERIFY(CredDeleteW(reinterpret_cast<LPCWSTR>(credential.name.utf16()),CRED_TYPE_GENERIC,0));
  QVERIFY(held.publication->finalizeNativePublication()!=QString("ok"));
  QVERIFY(song->recordingProjectIdentity().suspendUncertainSave(source));
  QCOMPARE(song->recordingProjectIdentity().lastCode(),QString("recovery_required"));
  QVERIFY(!song->recordingProjectIdentity().sourceIdentityTrusted());
  QCOMPARE(song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library).code,QString("recording_busy"));
  QCOMPARE(song->recordingProjectIdentity().cachedIdentity().id,prior.identity.id);
  held.publication.reset();QCOMPARE(contents(temp.filePath(QStringLiteral(".lmms-vocal-backup-")+tx)),original);
#endif
 }
 void failedIdentityCommitKeepsPublishedSavePending()
 {
#ifndef Q_OS_WIN
  QSKIP("Windows handle-bound publication");
#else
  QTemporaryDir temp;QVERIFY(temp.isValid());QVERIFY(QDir(temp.path()).mkdir("library"));
  auto library=RecordingLibrary::authorizeLocalRoot(temp.filePath("library"));QVERIFY(library.library);auto* song=Engine::getSong();
  const auto prior=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);QCOMPARE(prior.code,QString("ok"));
  const auto source=temp.filePath("source.mmp");QVERIFY(song->guiSaveProjectAs(source));const auto original=contents(source);
  TemporaryCredential credential;credential.name=nativeSaveCredential(temp.path());QVERIFY(!credential.name.isEmpty());PCREDENTIALW existing=nullptr;
  const bool exists=CredReadW(reinterpret_cast<LPCWSTR>(credential.name.utf16()),CRED_TYPE_GENERIC,0,&existing);
  const auto error=exists?DWORD(ERROR_SUCCESS):GetLastError();if(existing)CredFree(existing);QVERIFY(!exists);QCOMPARE(error,DWORD(ERROR_NOT_FOUND));credential.owned=true;
  DataFile data(DataFile::Type::SongProject);QVERIFY(song->recordingProjectIdentity().beginSave(data,source,false));
  QByteArray payload;QVERIFY(data.serializeProjectForSave(source,payload));
  auto held=ProjectSavePublication::acquireSelectedTarget(source);QVERIFY(held.publication);
  const auto tx=QUuid::createUuid().toString(QUuid::WithoutBraces);
  QCOMPARE(held.publication->createStage(tx,payload),QString("ok"));QCOMPARE(held.publication->prepareNativeJournal(),QString("ok"));
  QCOMPARE(held.publication->publishNativePending(),QString("ok"));QCOMPARE(held.publication->finalizeNativePublication(),QString("ok"));
  library.library->revoke();QVERIFY(!song->recordingProjectIdentity().finishSave(source,true,*held.publication));
  QCOMPARE(song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library).code,QString("recording_busy"));
  QVERIFY(!song->recordingProjectIdentity().sourceIdentityTrusted());
  QCOMPARE(song->recordingProjectIdentity().cachedIdentity().id,prior.identity.id);
  held.publication.reset();QCOMPARE(contents(temp.filePath(QStringLiteral(".lmms-vocal-backup-")+tx)),original);
#endif
 }
 void nativePublicationFailureCannotCommitNewIdentityOrFilename()
 {
#ifdef Q_OS_WIN
  QTemporaryDir temp;QVERIFY(QDir(temp.path()).mkdir("library"));auto library=RecordingLibrary::authorizeLocalRoot(temp.filePath("library"));QVERIFY(library.library);auto* song=Engine::getSong();
  const auto identity=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);QCOMPARE(identity.code,QString("ok"));const auto source=temp.filePath("source.mmp");QVERIFY(song->guiSaveProjectAs(source));
  const auto target=temp.filePath("locked.mmp");DataFile other(DataFile::Type::SongProject);other.head().setAttribute("fixture","preserve");QVERIFY(other.writeFile(target));const auto before=contents(target);
  HANDLE held=CreateFileW(reinterpret_cast<LPCWSTR>(target.utf16()),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);QVERIFY(held!=INVALID_HANDLE_VALUE);
  const bool saved=song->guiSaveProjectAs(target);CloseHandle(held);
  QVERIFY(!saved);QCOMPARE(song->projectFileName(),source);QCOMPARE(contents(target),before);
  QCOMPARE(song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library).identity.id,identity.identity.id);
#else
  QSKIP("Windows publication gate");
#endif
 }
 void failedPublicationPreservesPreexistingBackup()
 {
#ifdef Q_OS_WIN
  QTemporaryDir temp;QVERIFY(temp.isValid());QVERIFY(QDir(temp.path()).mkdir("library"));
  auto library=RecordingLibrary::authorizeLocalRoot(temp.filePath("library"));QVERIFY(library.library);auto* song=Engine::getSong();
  const auto identity=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);QCOMPARE(identity.code,QString("ok"));
  const auto source=temp.filePath("source.mmp");QVERIFY(song->guiSaveProjectAs(source));
  const auto target=temp.filePath("locked.mmp");DataFile other(DataFile::Type::SongProject);QVERIFY(other.writeFile(target));const auto targetBefore=contents(target);
  const auto backup=target+".bak";QFile backupFile(backup);QVERIFY(backupFile.open(QIODevice::WriteOnly | QIODevice::NewOnly));
  QCOMPARE(backupFile.write("preexisting-backup"),qint64(18));backupFile.close();const auto backupBefore=contents(backup);
  HANDLE held=CreateFileW(reinterpret_cast<LPCWSTR>(target.utf16()),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);QVERIFY(held!=INVALID_HANDLE_VALUE);
  const bool saved=song->guiSaveProjectAs(target);CloseHandle(held);
  QVERIFY(!saved);QCOMPARE(contents(target),targetBefore);QVERIFY(QFile::exists(backup));QCOMPARE(contents(backup),backupBefore);
#else
  QSKIP("Windows publication gate");
#endif
 }
 void successfulRecordingSavePreservesExistingBackupAndOriginal()
 {
#ifndef Q_OS_WIN
  QSKIP("Windows handle-bound publication");
#else
  QTemporaryDir temp;QVERIFY(temp.isValid());QVERIFY(QDir(temp.path()).mkdir("library"));
  auto library=RecordingLibrary::authorizeLocalRoot(temp.filePath("library"));QVERIFY(library.library);auto* song=Engine::getSong();
  const auto identity=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);QCOMPARE(identity.code,QString("ok"));
  const auto source=temp.filePath("source.mmp");QVERIFY(song->guiSaveProjectAs(source));const auto original=contents(source);
  const auto priorBackup=source+".bak";{QFile f(priorBackup);QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::NewOnly));QCOMPARE(f.write("prior-backup"),qint64(12));}
  auto* track=Track::create(Track::Type::Sample,song);QVERIFY(track);track->setName(QStringLiteral("Changed vocal"));
  QVERIFY2(song->guiSaveProject(),qPrintable(song->recordingProjectIdentity().lastCode()));
  QCOMPARE(contents(priorBackup),QByteArray("prior-backup"));QVERIFY(contents(source)!=original);
  const auto retained=QDir(temp.path()).entryList({QStringLiteral(".lmms-vocal-backup-*")},QDir::Files|QDir::Hidden);
  QCOMPARE(retained.size(),1);QCOMPARE(contents(temp.filePath(retained.front())),original);
  QCOMPARE(song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library).identity.id,identity.identity.id);
#endif
 }
 void replacedCurrentProjectCannotRotateIdentityDuringSave()
 {
#ifndef Q_OS_WIN
  QSKIP("Windows handle-bound publication");
#else
  QTemporaryDir temp;QVERIFY(temp.isValid());QVERIFY(QDir(temp.path()).mkdir("library"));
  auto library=RecordingLibrary::authorizeLocalRoot(temp.filePath("library"));QVERIFY(library.library);auto* song=Engine::getSong();
  const auto identity=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);QCOMPARE(identity.code,QString("ok"));
  const auto source=temp.filePath("source.mmp");QVERIFY(song->guiSaveProjectAs(source));const auto original=contents(source);
  const auto retained=temp.filePath("retained.mmp");QVERIFY(QFile::rename(source,retained));QVERIFY(QFile::copy(retained,source));
  const auto backup=source+".bak";{QFile f(backup);QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::NewOnly));QCOMPARE(f.write("prior-backup"),qint64(12));}
  QVERIFY(!song->guiSaveProject());
  QCOMPARE(song->recordingProjectIdentity().cachedIdentity().id,identity.identity.id);
  QCOMPARE(contents(source),original);QCOMPARE(contents(retained),original);QCOMPARE(contents(backup),QByteArray("prior-backup"));
#endif
 }
 void recordingSaveRecoversAnInterruptedNativePublication()
 {
#ifndef Q_OS_WIN
  QSKIP("Windows handle-bound publication");
#else
  QTemporaryDir temp;QVERIFY(temp.isValid());QVERIFY(QDir(temp.path()).mkdir("library"));
  auto library=RecordingLibrary::authorizeLocalRoot(temp.filePath("library"));QVERIFY(library.library);auto* song=Engine::getSong();
  const auto identity=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);QCOMPARE(identity.code,QString("ok"));
  const auto source=temp.filePath("source.mmp");QVERIFY(song->guiSaveProjectAs(source));const auto sourceBytes=contents(source);
  const auto target=temp.filePath("target.mmp");DataFile previous(DataFile::Type::SongProject);QVERIFY(previous.writeFile(target));const auto targetBytes=contents(target);
  const auto backup=target+".bak";{QFile f(backup);QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::NewOnly));QCOMPARE(f.write("prior-backup"),qint64(12));}
  TemporaryCredential credential;credential.name=nativeSaveCredential(temp.path());QVERIFY(!credential.name.isEmpty());PCREDENTIALW existing=nullptr;
  const bool exists=CredReadW(reinterpret_cast<LPCWSTR>(credential.name.utf16()),CRED_TYPE_GENERIC,0,&existing);
  const auto error=exists?DWORD(ERROR_SUCCESS):GetLastError();if(existing)CredFree(existing);QVERIFY(!exists);QCOMPARE(error,DWORD(ERROR_NOT_FOUND));credential.owned=true;
  const auto tx=QUuid::createUuid().toString(QUuid::WithoutBraces);
  {auto held=ProjectSavePublication::acquireSelectedTarget(target);QVERIFY(held.publication);
   QCOMPARE(held.publication->createStage(tx,QByteArray("interrupted")),QString("ok"));
   QCOMPARE(held.publication->prepareNativeJournal(),QString("ok"));QCOMPARE(held.publication->moveOriginalToBackup(tx),QString("ok"));}
  QVERIFY(!QFileInfo::exists(target));
  QVERIFY2(song->guiSaveProjectAs(target),qPrintable(song->recordingProjectIdentity().lastCode()));
  QCOMPARE(contents(source),sourceBytes);QCOMPARE(contents(backup),QByteArray("prior-backup"));
  QCOMPARE(contents(temp.filePath(QStringLiteral(".lmms-vocal-stage-")+tx)),QByteArray("interrupted"));
  const auto retained=QDir(temp.path()).entryList({QStringLiteral(".lmms-vocal-backup-*")},QDir::Files|QDir::Hidden);
  bool foundOriginal=false;for(const auto& name:retained)if(contents(temp.filePath(name))==targetBytes)foundOriginal=true;
  QVERIFY(foundOriginal);QCOMPARE(ProjectSavePublication::nativeJournalStatus(target),QString("none"));
#endif
 }
 void loadingAnInterruptedRecordingProjectRecoversBeforeOpening()
 {
#ifndef Q_OS_WIN
  QSKIP("Windows handle-bound publication");
#else
  QTemporaryDir temp;QVERIFY(temp.isValid());QVERIFY(QDir(temp.path()).mkdir("library"));
  auto library=RecordingLibrary::authorizeLocalRoot(temp.filePath("library"));QVERIFY(library.library);auto* song=Engine::getSong();
  const auto identity=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);QCOMPARE(identity.code,QString("ok"));
  const auto source=temp.filePath("source.mmp");QVERIFY(song->guiSaveProjectAs(source));const auto original=contents(source);
  TemporaryCredential credential;credential.name=nativeSaveCredential(temp.path());QVERIFY(!credential.name.isEmpty());PCREDENTIALW existing=nullptr;
  const bool exists=CredReadW(reinterpret_cast<LPCWSTR>(credential.name.utf16()),CRED_TYPE_GENERIC,0,&existing);
  const auto error=exists?DWORD(ERROR_SUCCESS):GetLastError();if(existing)CredFree(existing);QVERIFY(!exists);QCOMPARE(error,DWORD(ERROR_NOT_FOUND));credential.owned=true;
  const auto tx=QUuid::createUuid().toString(QUuid::WithoutBraces);
  {auto held=ProjectSavePublication::acquireSelectedTarget(source);QVERIFY(held.publication);
   QCOMPARE(held.publication->createStage(tx,QByteArray("interrupted")),QString("ok"));
   QCOMPARE(held.publication->prepareNativeJournal(),QString("ok"));QCOMPARE(held.publication->moveOriginalToBackup(tx),QString("ok"));}
  QVERIFY(!QFileInfo::exists(source));song->loadProject(source);
  QCOMPARE(contents(source),original);QCOMPARE(song->projectFileName(),source);
  QCOMPARE(song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library).identity.id,identity.identity.id);
  QCOMPARE(contents(temp.filePath(QStringLiteral(".lmms-vocal-stage-")+tx)),QByteArray("interrupted"));
#endif
 }
 void interruptedActiveRecoverySnapshotKeepsRecordingMetadata()
 {
#ifndef Q_OS_WIN
  QSKIP("Windows handle-bound publication");
#else
  QTemporaryDir temp;QVERIFY(temp.isValid());QVERIFY(QDir(temp.path()).mkdir("library"));
  auto library=RecordingLibrary::authorizeLocalRoot(temp.filePath("library"));QVERIFY(library.library);auto* song=Engine::getSong();
  const auto identity=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);QCOMPARE(identity.code,QString("ok"));
  const auto source=temp.filePath("source.mmp");QVERIFY(song->guiSaveProjectAs(source));
  TemporaryCredential credential;credential.name=nativeSaveCredential(temp.path());QVERIFY(!credential.name.isEmpty());PCREDENTIALW existing=nullptr;
  const bool exists=CredReadW(reinterpret_cast<LPCWSTR>(credential.name.utf16()),CRED_TYPE_GENERIC,0,&existing);
  const auto error=exists?DWORD(ERROR_SUCCESS):GetLastError();if(existing)CredFree(existing);QVERIFY(!exists);QCOMPARE(error,DWORD(ERROR_NOT_FOUND));credential.owned=true;
  const auto tx=QUuid::createUuid().toString(QUuid::WithoutBraces);
  {auto held=ProjectSavePublication::acquireSelectedTarget(source);QVERIFY(held.publication);
   QCOMPARE(held.publication->createStage(tx,QByteArray("interrupted")),QString("ok"));
   QCOMPARE(held.publication->prepareNativeJournal(),QString("ok"));QCOMPARE(held.publication->moveOriginalToBackup(tx),QString("ok"));}
  QVERIFY(!QFileInfo::exists(source));
  QVERIFY(song->saveProjectFile(source,false,Song::ProjectSavePurpose::RecoverySnapshot));
  DataFile saved(source);const auto metadata=saved.head().firstChildElement("recording_project");QVERIFY(!metadata.isNull());
  QCOMPARE(metadata.attribute("id"),identity.identity.id);
  QCOMPARE(song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library).identity.id,identity.identity.id);
#endif
 }
 void unresolvedRecoveryDoesNotReplaceUnsavedProject()
 {
#ifndef Q_OS_WIN
  QSKIP("Windows handle-bound publication");
#else
  QTemporaryDir currentDir,targetDir;QVERIFY(currentDir.isValid()&&targetDir.isValid());
  const auto current=currentDir.filePath("current.mmp");auto* song=Engine::getSong();QVERIFY(song->guiSaveProjectAs(current));
  auto* track=Track::create(Track::Type::Sample,song);QVERIFY(track);track->setName(QStringLiteral("Unsaved vocal"));song->setModified();
  const auto count=song->tracks().size();QVERIFY(song->isModified());
  const auto target=targetDir.filePath("target.mmp");DataFile initial(DataFile::Type::SongProject);QVERIFY(initial.writeFile(target));
  const auto original=contents(target);TemporaryCredential credential;credential.name=nativeSaveCredential(targetDir.path());QVERIFY(!credential.name.isEmpty());
  PCREDENTIALW existing=nullptr;const bool exists=CredReadW(reinterpret_cast<LPCWSTR>(credential.name.utf16()),CRED_TYPE_GENERIC,0,&existing);
  const auto error=exists?DWORD(ERROR_SUCCESS):GetLastError();if(existing)CredFree(existing);QVERIFY(!exists);QCOMPARE(error,DWORD(ERROR_NOT_FOUND));credential.owned=true;
  const auto tx=QUuid::createUuid().toString(QUuid::WithoutBraces);
  {auto held=ProjectSavePublication::acquireSelectedTarget(target);QVERIFY(held.publication);
   QCOMPARE(held.publication->createStage(tx,QByteArray("edited")),QString("ok"));
   QCOMPARE(held.publication->prepareNativeJournal(),QString("ok"));QCOMPARE(held.publication->moveOriginalToBackup(tx),QString("ok"));}
  QVERIFY(CredDeleteW(reinterpret_cast<LPCWSTR>(credential.name.utf16()),CRED_TYPE_GENERIC,0));
  song->loadProject(target);
  QCOMPARE(song->projectFileName(),current);QVERIFY(song->isModified());QCOMPARE(song->tracks().size(),count);
  QCOMPARE(track->name(),QStringLiteral("Unsaved vocal"));QVERIFY(!QFileInfo::exists(target));
  QCOMPARE(contents(targetDir.filePath(QStringLiteral(".lmms-vocal-backup-")+tx)),original);
  QCOMPARE(contents(targetDir.filePath(QStringLiteral(".lmms-vocal-stage-")+tx)),QByteArray("edited"));
#endif
 }
};
QTEST_MAIN(RecordingProjectIdentityTest)
#include "RecordingProjectIdentityTest.moc"
