#include "RecordingProjectIdentity.h"
#include "Engine.h"
#include "Song.h"
#include "DataFile.h"
#include "ProjectJournal.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QCryptographicHash>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
using namespace lmms;
using namespace lmms::recording;
class RecordingProjectIdentityTest : public QObject
{
 Q_OBJECT
 static QByteArray contents(const QString& path){QFile f(path);if(!f.open(QIODevice::ReadOnly))return {};return f.readAll();}
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
 void duplicatedSerializedUuidGetsSeparateFolder()
 {
  QTemporaryDir temp;QVERIFY(QDir(temp.path()).mkdir("library"));auto library=RecordingLibrary::authorizeLocalRoot(temp.filePath("library"));QVERIFY(library.library);auto* song=Engine::getSong();
  const auto first=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);QCOMPARE(first.code,QString("ok"));const auto original=temp.filePath("source.mmp");QVERIFY(song->guiSaveProjectAs(original));
  const auto copy=temp.filePath("copy.mmp");QVERIFY(QFile::copy(original,copy));song->loadProject(copy);const auto copied=song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);
  QCOMPARE(copied.code,QString("ok"));QVERIFY(copied.identity.id!=first.identity.id);QCOMPARE(contents(original),contents(copy));
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
};
QTEST_MAIN(RecordingProjectIdentityTest)
#include "RecordingProjectIdentityTest.moc"
