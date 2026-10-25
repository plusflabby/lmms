#include "RecordingLibrary.h"
#include "RecordingTakeJournal.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
#include <array>
#include <atomic>
#include <map>
#include <vector>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#ifdef Q_OS_WIN
#include <windows.h>
#include <winternl.h>
#endif
namespace lmms::recording {
namespace {
bool validId(const QString& text){const QUuid id(text);return !id.isNull() && id.toString(QUuid::WithoutBraces)==text;}
QString identityError(const ProjectIdentity& project)
{
 if(project.schema!=1)return "unsupported_schema";
 return validId(project.id)?QString{}:QStringLiteral("invalid_identity");
}
#ifdef Q_OS_WIN
struct ProjectPin
{
 HANDLE directory=INVALID_HANDLE_VALUE,manifest=INVALID_HANDLE_VALUE;
 std::uint64_t lastUse=0;
 ~ProjectPin(){if(manifest!=INVALID_HANDLE_VALUE)CloseHandle(manifest);if(directory!=INVALID_HANDLE_VALUE)CloseHandle(directory);}
};
QString finalName(HANDLE handle)
{
 std::array<wchar_t,1025> buffer{};const auto size=GetFinalPathNameByHandleW(handle,buffer.data(),DWORD(buffer.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_GUID);
 return size && size<buffer.size()?QString::fromWCharArray(buffer.data(),int(size)).replace('\\','/'):QString{};
}
bool ordinary(HANDLE h,bool directory,DWORD volume)
{
 BY_HANDLE_FILE_INFORMATION info{};
 return GetFileType(h)==FILE_TYPE_DISK && GetFileInformationByHandle(h,&info) && info.dwVolumeSerialNumber==volume
  && !(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) && bool(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)==directory
  && (directory || info.nNumberOfLinks==1);
}
HANDLE relative(HANDLE parent,const QString& component,ACCESS_MASK access,ULONG disposition,bool directory,NTSTATUS& status,ULONG share=std::numeric_limits<ULONG>::max())
{
 const auto fn=reinterpret_cast<decltype(&NtCreateFile)>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtCreateFile"));
 if(!fn){status=NTSTATUS(0xc0000002u);return INVALID_HANDLE_VALUE;}
 UNICODE_STRING name{};name.Buffer=const_cast<PWSTR>(reinterpret_cast<LPCWSTR>(component.utf16()));name.Length=USHORT(component.size()*2);name.MaximumLength=name.Length;
 OBJECT_ATTRIBUTES attrs{};attrs.Length=sizeof(attrs);attrs.RootDirectory=parent;attrs.ObjectName=&name;
 // Native object parsing must fail instead of following any reparse encountered
 // in the handle-relative lookup. Directory sharing permits ordinary child-file
 // publication but never directory deletion/replacement; file pins deny writes.
 attrs.Attributes=OBJ_DONT_REPARSE;
 if(share==std::numeric_limits<ULONG>::max())share=directory?(FILE_SHARE_READ|FILE_SHARE_WRITE):FILE_SHARE_READ;
 IO_STATUS_BLOCK io{};HANDLE h=nullptr;
 status=fn(&h,access|SYNCHRONIZE,&attrs,&io,nullptr,FILE_ATTRIBUTE_NORMAL,share,disposition,
  FILE_OPEN_REPARSE_POINT|FILE_SYNCHRONOUS_IO_NONALERT|(directory?FILE_DIRECTORY_FILE:FILE_NON_DIRECTORY_FILE),nullptr,0);
 return status>=0?h:INVALID_HANDLE_VALUE;
}
bool writeAll(HANDLE h,const QByteArray& bytes)
{
 DWORD written=0;return WriteFile(h,bytes.constData(),DWORD(bytes.size()),&written,nullptr) && written==DWORD(bytes.size()) && FlushFileBuffers(h);
}
QByteArray readBounded(HANDLE h,qint64 bound)
{
 LARGE_INTEGER size{},zero{};if(!GetFileSizeEx(h,&size) || size.QuadPart<0 || size.QuadPart>bound || !SetFilePointerEx(h,zero,nullptr,FILE_BEGIN))return {};
 QByteArray data(int(size.QuadPart),Qt::Uninitialized);DWORD read=0;
 return ReadFile(h,data.data(),DWORD(data.size()),&read,nullptr) && read==DWORD(data.size())?data:QByteArray{};
}
#endif
}
struct RecordingLibrary::RootState
{
 std::atomic_bool revoked{false};
#ifdef Q_OS_WIN
 std::vector<HANDLE> ancestors;
 QString finalPath;
 DWORD volume=0;
 std::uint64_t projectUse=0;
 HANDLE writerLock=INVALID_HANDLE_VALUE;
 std::map<QString,std::shared_ptr<ProjectPin>> projects;
 ~RootState(){projects.clear();if(writerLock!=INVALID_HANDLE_VALUE)CloseHandle(writerLock);for(auto i=ancestors.rbegin();i!=ancestors.rend();++i)CloseHandle(*i);}
#endif
};
struct OwnedNewFile::State
{
 std::shared_ptr<RecordingLibrary::RootState> root;
#ifdef Q_OS_WIN
 std::shared_ptr<ProjectPin> project;
 HANDLE file=INVALID_HANDLE_VALUE;
 ~State(){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}
#endif
 quint64 written=0;
};
namespace {
bool authorized(const std::shared_ptr<RecordingLibrary::RootState>& root)
{
#ifdef Q_OS_WIN
 return root && !root->revoked.load(std::memory_order_acquire) && !root->ancestors.empty()
  && ordinary(root->ancestors.back(),true,root->volume) && finalName(root->ancestors.back())==root->finalPath;
#else
 return false;
#endif
}
#ifdef Q_OS_WIN
std::pair<QString,std::shared_ptr<ProjectPin>> projectPin(const std::shared_ptr<RecordingLibrary::RootState>& root,const ProjectIdentity& project,bool create=true)
{
 if(const auto error=identityError(project);!error.isEmpty())return {error,{}};
 if(!authorized(root))return {"unauthorized_library",{}};
 if(root->projectUse==std::numeric_limits<std::uint64_t>::max())return {"resource_limit",{}};
 if(const auto found=root->projects.find(project.id);found!=root->projects.end())
 {found->second->lastUse=++root->projectUse;return {"ok",found->second};}
 if(root->projects.size()>=64)
 {
  auto victim=root->projects.end();
  for(auto it=root->projects.begin();it!=root->projects.end();++it)
   if(it->second.use_count()==1 && (victim==root->projects.end() || it->second->lastUse<victim->second->lastUse))victim=it;
  if(victim==root->projects.end())return {"resource_limit",{}};
  root->projects.erase(victim);
 }
 NTSTATUS status=0;auto pin=std::make_shared<ProjectPin>();
 pin->directory=relative(root->ancestors.back(),project.id,FILE_READ_ATTRIBUTES|FILE_TRAVERSE,create?FILE_OPEN_IF:FILE_OPEN,true,status);
 if(pin->directory==INVALID_HANDLE_VALUE || !ordinary(pin->directory,true,root->volume) || finalName(pin->directory)!=root->finalPath+'/'+project.id)return {"unauthorized_library",{}};
 // Immutable project manifest. Pin the read handle so it cannot be replaced after validation.
 const auto path=root->finalPath+'/'+project.id+"/project.json";
 auto manifest=create?relative(pin->directory,"project.json",GENERIC_READ|GENERIC_WRITE,FILE_CREATE,false,status):INVALID_HANDLE_VALUE;
 if(create && manifest!=INVALID_HANDLE_VALUE)
 {
  const auto bytes=QJsonDocument(QJsonObject{{"schema",1},{"project_id",project.id}}).toJson(QJsonDocument::Compact);
  const bool saved=ordinary(manifest,false,root->volume) && finalName(manifest)==path && writeAll(manifest,bytes);
  CloseHandle(manifest);if(!saved)return {"write_failed",{}};
 }
 else if(create && quint32(status)!=0xc0000035u)return {"unauthorized_library",{}};
 pin->manifest=relative(pin->directory,"project.json",GENERIC_READ,FILE_OPEN,false,status);
 if(pin->manifest==INVALID_HANDLE_VALUE || !ordinary(pin->manifest,false,root->volume) || finalName(pin->manifest)!=path)return {"source_identity_changed",{}};
 QJsonParseError parse{};const auto doc=QJsonDocument::fromJson(readBounded(pin->manifest,512),&parse);
 if(parse.error!=QJsonParseError::NoError || !doc.isObject())return {"invalid_manifest",{}};
 const auto obj=doc.object();if(obj["schema"]!=QJsonValue(1))return {"unsupported_schema",{}};
 if(obj.size()!=2 || obj["project_id"]!=QJsonValue(project.id))return {"invalid_manifest",{}};
 pin->lastUse=++root->projectUse;root->projects.emplace(project.id,pin);return {"ok",std::move(pin)};
}
class RecoveryDevice final : public QIODevice
{
public:
 explicit RecoveryDevice(std::unique_ptr<OwnedNewFile::State> state):m_state(std::move(state)){open(QIODevice::ReadOnly|QIODevice::Unbuffered);}
 qint64 size() const override {LARGE_INTEGER size{};return GetFileSizeEx(m_state->file,&size)?size.QuadPart:-1;}
 bool seek(qint64 position) override
 {
  if(position<0 || !authorized(m_state->root))return false;
  LARGE_INTEGER offset{};offset.QuadPart=position;return SetFilePointerEx(m_state->file,offset,nullptr,FILE_BEGIN) && QIODevice::seek(position);
 }
protected:
 qint64 readData(char* dest,qint64 length) override
 {
  if(length<0 || !authorized(m_state->root))return -1;
  DWORD count=0;return ReadFile(m_state->file,dest,DWORD(std::min<qint64>(length,65536)),&count,nullptr)?qint64(count):-1;
 }
 qint64 writeData(const char*,qint64) override{return -1;}
private:
 std::unique_ptr<OwnedNewFile::State> m_state;
};
#endif
}
OwnedNewFile::OwnedNewFile(std::unique_ptr<State> state):m_state(std::move(state)){}
OwnedNewFile::~OwnedNewFile()=default;
qint64 OwnedNewFile::append(const QByteArray& bytes)
{
#ifdef Q_OS_WIN
 if(!m_state || !authorized(m_state->root) || quint64(bytes.size())>57600044-m_state->written)return -1;
 DWORD count=0;if(!WriteFile(m_state->file,bytes.constData(),DWORD(bytes.size()),&count,nullptr))return -1;
 m_state->written+=count;return count;
#else
 return -1;
#endif
}
bool OwnedNewFile::flush()
{
#ifdef Q_OS_WIN
 return m_state && authorized(m_state->root) && FlushFileBuffers(m_state->file);
#else
 return false;
#endif
}
namespace {
class OwnedLibraryTakeSink final : public RecordingWriterSink
{
public:
#ifdef Q_OS_WIN
 OwnedLibraryTakeSink(std::shared_ptr<RecordingLibrary::RootState> root,std::shared_ptr<ProjectPin> project,
  HANDLE audio,HANDLE journal,QString projectId,QString takeId,std::uint32_t rate,std::uint64_t maximum)
  :m_root(std::move(root)),m_project(std::move(project)),m_audio(audio),m_journal(journal),m_projectId(std::move(projectId)),
   m_takeId(std::move(takeId)),m_audioPath(rootPath()+"/take-"+m_takeId+".partial"),m_journalPath(rootPath()+"/take-"+m_takeId+".journal"),m_rate(rate),m_maximum(maximum){}
 ~OwnedLibraryTakeSink() override{if(m_audio!=INVALID_HANDLE_VALUE)CloseHandle(m_audio);if(m_journal!=INVALID_HANDLE_VALUE)CloseHandle(m_journal);}
#else
 OwnedLibraryTakeSink()=default;
#endif
 WriterIoCode beginTake(const TakeDescriptor& descriptor) noexcept override
 {
#ifdef Q_OS_WIN
  if(m_started || descriptor.projectId!=m_projectId || descriptor.takeId!=m_takeId || descriptor.sampleRate!=m_rate
   || descriptor.maxRawFrames!=m_maximum || !authorized(m_root) || !ordinary(m_audio,false,m_root->volume)
   || !ordinary(m_journal,false,m_root->volume) || finalName(m_audio)!=m_audioPath || finalName(m_journal)!=m_journalPath)return WriterIoCode::IdentityChanged;
  const auto header=RecordingWavHeader::encodePcm16(m_rate,0);const auto manifest=RecordingTakeJournalCodec::encodeHeader(descriptor);
  if(!header.valid || manifest.isEmpty() || manifest.size()>int(TakeJournalRecordMaximum))return WriterIoCode::WriteFailed;
  DWORD wrote=0;if(!WriteFile(m_audio,header.bytes.data(),DWORD(header.bytes.size()),&wrote,nullptr) || wrote!=DWORD(header.bytes.size()) || !FlushFileBuffers(m_audio))return WriterIoCode::FlushFailed;
  m_audioWritten=header.bytes.size();
  if(!writeJournal(manifest))return WriterIoCode::FlushFailed;
  m_journalWritten=std::uint64_t(manifest.size());m_descriptor=descriptor;m_started=true;return WriterIoCode::Ok;
#else
  Q_UNUSED(descriptor);return WriterIoCode::IdentityChanged;
#endif
 }
 WriterIoResult appendPayload(std::span<const std::uint8_t> bytes) noexcept override
 {
#ifdef Q_OS_WIN
  if(!m_started || m_audioSealed || !authorized(m_root) || finalName(m_audio)!=m_audioPath
   || bytes.size()>std::uint64_t(44)+m_maximum*2-m_audioWritten)return {WriterIoCode::IdentityChanged,0};
  DWORD wrote=0;const bool ok=WriteFile(m_audio,bytes.data(),DWORD(bytes.size()),&wrote,nullptr);m_audioWritten+=wrote;
  return {ok?WriterIoCode::Ok:WriterIoCode::WriteFailed,wrote};
#else
  Q_UNUSED(bytes);return {WriterIoCode::IdentityChanged,0};
#endif
 }
 WriterIoCode appendCheckpoint(const TakeProgress& progress) noexcept override
 {
#ifdef Q_OS_WIN
  const auto record=RecordingTakeJournalCodec::encodeProgress(progress);
  if(!record.valid || !m_started || m_audioSealed || !authorized(m_root) || finalName(m_audio)!=m_audioPath || finalName(m_journal)!=m_journalPath)return WriterIoCode::IdentityChanged;
  if(m_journalWritten>std::uint64_t(65536-record.size))return WriterIoCode::WriteFailed;
  if(!FlushFileBuffers(m_audio) || !writeJournal(QByteArray(reinterpret_cast<const char*>(record.bytes.data()),record.size)))return WriterIoCode::FlushFailed;
  m_journalWritten+=record.size;return WriterIoCode::Ok;
#else
  Q_UNUSED(progress);return WriterIoCode::IdentityChanged;
#endif
 }
 WriterIoCode appendSealIntent(const TakeProgress& progress) noexcept override
 {
#ifdef Q_OS_WIN
  const auto record=RecordingTakeJournalCodec::encodeSealIntent(progress);
  if(!record.valid || !m_started || m_audioSealed || !authorized(m_root) || finalName(m_audio)!=m_audioPath || finalName(m_journal)!=m_journalPath)return WriterIoCode::IdentityChanged;
  if(m_journalWritten>std::uint64_t(65536-record.size))return WriterIoCode::WriteFailed;
  if(!FlushFileBuffers(m_audio) || !writeJournal(QByteArray(reinterpret_cast<const char*>(record.bytes.data()),record.size)))return WriterIoCode::FlushFailed;
  m_journalWritten+=record.size;return WriterIoCode::Ok;
#else
  Q_UNUSED(progress);return WriterIoCode::IdentityChanged;
#endif
 }
 WriterIoCode sealPcm16(std::uint32_t rate,std::uint64_t frames) noexcept override
 {
#ifdef Q_OS_WIN
  if(!m_started || m_audioSealed || rate!=m_rate || frames>m_maximum || m_audioWritten!=44+frames*2
   || !authorized(m_root) || finalName(m_audio)!=m_audioPath)return WriterIoCode::SealFailed;
  const auto header=RecordingWavHeader::encodePcm16(rate,frames);if(!header.valid)return WriterIoCode::SealFailed;
  LARGE_INTEGER start{};DWORD wrote=0;
  if(!SetFilePointerEx(m_audio,start,nullptr,FILE_BEGIN) || !WriteFile(m_audio,header.bytes.data(),DWORD(header.bytes.size()),&wrote,nullptr)
   || wrote!=DWORD(header.bytes.size()) || !FlushFileBuffers(m_audio))return WriterIoCode::SealFailed;
  BY_HANDLE_FILE_INFORMATION beforeRename{};if(!GetFileInformationByHandle(m_audio,&beforeRename))return WriterIoCode::IdentityChanged;
  const auto target=QStringLiteral("take-")+m_takeId+QStringLiteral(".wav");
  const auto bytes=offsetof(FILE_RENAME_INFORMATION,FileName)+std::size_t(target.size())*sizeof(wchar_t);
  std::vector<std::uint64_t> storage((bytes+7)/8,0);auto* rename=reinterpret_cast<FILE_RENAME_INFORMATION*>(storage.data());
  rename->ReplaceIfExists=FALSE;rename->RootDirectory=m_project->directory;rename->FileNameLength=ULONG(target.size()*sizeof(wchar_t));
  std::memcpy(rename->FileName,target.utf16(),std::size_t(target.size())*sizeof(wchar_t));
  const auto set=reinterpret_cast<decltype(&NtSetInformationFile)>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtSetInformationFile"));
  IO_STATUS_BLOCK io{};const auto status=set?set(m_audio,&io,rename,ULONG(bytes),FileRenameInformation):NTSTATUS(0xc0000002u);
  if(status<0)return quint32(status)==0xc0000035u?WriterIoCode::Collision:WriterIoCode::SealFailed;
  m_audioPath=rootPath()+"/"+target;
  if(!ordinary(m_audio,false,m_root->volume) || finalName(m_audio)!=m_audioPath || !FlushFileBuffers(m_audio))return WriterIoCode::IdentityChanged;
  CloseHandle(m_audio);m_audio=INVALID_HANDLE_VALUE;NTSTATUS readStatus=0;
  const auto readonly=relative(m_project->directory,target,GENERIC_READ,FILE_OPEN,false,readStatus);
  if(readonly==INVALID_HANDLE_VALUE)return WriterIoCode::IdentityChanged;
  BY_HANDLE_FILE_INFORMATION afterRename{};
  if(!ordinary(readonly,false,m_root->volume) || finalName(readonly)!=m_audioPath || !GetFileInformationByHandle(readonly,&afterRename)
   || beforeRename.dwVolumeSerialNumber!=afterRename.dwVolumeSerialNumber || beforeRename.nFileIndexHigh!=afterRename.nFileIndexHigh
   || beforeRename.nFileIndexLow!=afterRename.nFileIndexLow){CloseHandle(readonly);return WriterIoCode::IdentityChanged;}
  m_audio=readonly;
  LARGE_INTEGER fileSize{};if(!GetFileSizeEx(m_audio,&fileSize) || fileSize.QuadPart!=LONGLONG(44+frames*2))return WriterIoCode::IdentityChanged;
  QCryptographicHash hash(QCryptographicHash::Sha256);std::array<char,65536> buffer{};std::uint64_t total=0;LARGE_INTEGER zero{};
  if(!SetFilePointerEx(m_audio,zero,nullptr,FILE_BEGIN))return WriterIoCode::IdentityChanged;
  for(;;){DWORD count=0;if(!ReadFile(m_audio,buffer.data(),DWORD(buffer.size()),&count,nullptr))return WriterIoCode::IdentityChanged;
   if(!count)break;hash.addData(buffer.data(),int(count));total+=count;}
  if(total!=std::uint64_t(fileSize.QuadPart) || !SetFilePointerEx(m_audio,zero,nullptr,FILE_BEGIN))return WriterIoCode::IdentityChanged;
  m_audioSha256=hash.result();if(m_audioSha256.size()!=32)return WriterIoCode::IdentityChanged;
  m_audioSealed=true;return WriterIoCode::Ok;
#else
  Q_UNUSED(rate);Q_UNUSED(frames);return WriterIoCode::SealFailed;
#endif
 }
 QByteArray sealedAudioSha256() const noexcept override{return m_audioSealed?m_audioSha256:QByteArray{};}
 WriterIoCode appendTerminal(TakeTerminal terminal,const TakeProgress& progress) noexcept override
 {
#ifdef Q_OS_WIN
  if(!m_started || !m_audioSealed || !authorized(m_root) || finalName(m_audio)!=m_audioPath || finalName(m_journal)!=m_journalPath)return WriterIoCode::IdentityChanged;
  if(terminal==TakeTerminal::Complete && (!progress.sourceDigestKnown || QByteArray(reinterpret_cast<const char*>(progress.sourceDigest.data()),int(progress.sourceDigest.size()))!=m_audioSha256))return WriterIoCode::IdentityChanged;
  const auto record=RecordingTakeJournalCodec::encodeTerminal(terminal,progress);if(!record.valid || m_journalWritten>std::uint64_t(65536-record.size))return WriterIoCode::WriteFailed;
  if(!writeJournal(QByteArray(reinterpret_cast<const char*>(record.bytes.data()),record.size)))return WriterIoCode::FlushFailed;
  m_journalWritten+=record.size;return WriterIoCode::Ok;
#else
  Q_UNUSED(terminal);Q_UNUSED(progress);return WriterIoCode::IdentityChanged;
#endif
 }
 void preservePartial() noexcept override
 {
#ifdef Q_OS_WIN
  if(authorized(m_root)){if(m_audio!=INVALID_HANDLE_VALUE)FlushFileBuffers(m_audio);if(m_journal!=INVALID_HANDLE_VALUE)FlushFileBuffers(m_journal);}
#endif
 }
private:
#ifdef Q_OS_WIN
 QString rootPath() const{return m_root->finalPath+'/'+m_projectId;}
 bool writeJournal(const QByteArray& bytes) noexcept
 {
  if(m_journalPoisoned || bytes.isEmpty() || bytes.size()>int(TakeJournalRecordMaximum) || m_journal==INVALID_HANDLE_VALUE)return false;
  DWORD offset=0,total=DWORD(bytes.size());
  while(offset<total)
  {DWORD wrote=0;if(!WriteFile(m_journal,bytes.constData()+offset,total-offset,&wrote,nullptr) || !wrote){m_journalPoisoned=true;return false;}offset+=wrote;}
  if(!FlushFileBuffers(m_journal)){m_journalPoisoned=true;return false;}return true;
 }
 std::shared_ptr<RecordingLibrary::RootState> m_root;std::shared_ptr<ProjectPin> m_project;
 HANDLE m_audio=INVALID_HANDLE_VALUE,m_journal=INVALID_HANDLE_VALUE;QString m_projectId,m_takeId,m_audioPath,m_journalPath;
 std::uint32_t m_rate=0;std::uint64_t m_maximum=0,m_audioWritten=0,m_journalWritten=0;TakeDescriptor m_descriptor;QByteArray m_audioSha256;bool m_started=false,m_audioSealed=false,m_journalPoisoned=false;
#endif
};
}
RecordingLibrary::RecordingLibrary(std::shared_ptr<RootState> root):m_root(std::move(root)){}
RecordingLibrary::~RecordingLibrary(){revoke();}
RecordingLibrary::Approval RecordingLibrary::authorizeLocalRoot(const QString& selected)
{
#ifdef Q_OS_WIN
 auto path=selected;path.replace('\\','/');
 if(path.size()<=3 || path.size()>1024 || path[1]!=':' || path[2]!='/' || !path[0].isLetter())return {"unauthorized_library",{}};
 path[0]=path[0].toUpper();const auto components=path.mid(3).split('/');
 for(const auto& part:components)
 {
  if(part.isEmpty() || part=="." || part==".." || part.endsWith('.') || part.endsWith(' '))return {"unauthorized_library",{}};
  for(const auto c:part)if(c.unicode()<32 || QStringLiteral(":*?\"<>|").contains(c))return {"unauthorized_library",{}};
 }
 auto root=std::make_shared<RootState>();const auto drive=path.left(3).replace('/','\\');
 const auto driveType=GetDriveTypeW(reinterpret_cast<LPCWSTR>(drive.utf16()));if(driveType!=DRIVE_FIXED && driveType!=DRIVE_REMOVABLE)return {"unauthorized_library",{}};
 auto anchor=CreateFileW(reinterpret_cast<LPCWSTR>(drive.utf16()),FILE_READ_ATTRIBUTES|FILE_TRAVERSE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
 if(anchor==INVALID_HANDLE_VALUE)return {"unauthorized_library",{}};
 root->ancestors.push_back(anchor);BY_HANDLE_FILE_INFORMATION info{};
 if(!GetFileInformationByHandle(anchor,&info) || !ordinary(anchor,true,info.dwVolumeSerialNumber))return {"unauthorized_library",{}};
 root->volume=info.dwVolumeSerialNumber;auto expected=finalName(anchor);
 if(!expected.startsWith("//?/Volume{") || !expected.endsWith("}/"))return {"unauthorized_library",{}};
 for(const auto& part:components)
 {
  NTSTATUS status=0;const auto opened=relative(root->ancestors.back(),part,FILE_READ_ATTRIBUTES|FILE_TRAVERSE,FILE_OPEN,true,status);
  if(opened==INVALID_HANDLE_VALUE)return {"unauthorized_library",{}};
  root->ancestors.push_back(opened);expected+=part;
  if(!ordinary(opened,true,root->volume) || finalName(opened)!=expected)return {"unauthorized_library",{}};
  expected+='/';
 }
 root->finalPath=expected.chopped(1);
 // The walk pins every component until the selected root is acquired. All later
 // operations are relative to that root/project handles, never a reopened path.
 // Retaining restrictive pins on unrelated ancestors blocks sibling QSaveFile
 // publication on Windows; retain only the selected physical root capability.
 for(size_t i=0;i+1<root->ancestors.size();++i)CloseHandle(root->ancestors[i]);
 root->ancestors.erase(root->ancestors.begin(),root->ancestors.end()-1);
 NTSTATUS lockStatus=0;
 root->writerLock=relative(root->ancestors.back(),"recording-library.lock",GENERIC_READ|GENERIC_WRITE,FILE_OPEN_IF,false,lockStatus,0);
 LARGE_INTEGER lockSize{};
 if(root->writerLock==INVALID_HANDLE_VALUE || !ordinary(root->writerLock,false,root->volume)
  || finalName(root->writerLock)!=root->finalPath+"/recording-library.lock" || !GetFileSizeEx(root->writerLock,&lockSize) || lockSize.QuadPart!=0)return {"unauthorized_library",{}};
 return {"ok",std::unique_ptr<RecordingLibrary>(new RecordingLibrary(std::move(root)))};
#else
 return {"unsupported_backend",{}};
#endif
}
QString RecordingLibrary::prepareProjectFolder(const ProjectIdentity& project)
{
#ifdef Q_OS_WIN
 return projectPin(m_root,project).first;
#else
 return "unsupported_backend";
#endif
}
RecordingLibrary::NewFileResult RecordingLibrary::createUniquePartial(const ProjectIdentity& project,const TakeIdentity& take)
{
 if(const auto error=identityError(project);!error.isEmpty())return {error,{}};
 if(!validId(take.id) || take.label.toUtf8().size()>256)return {"invalid_identity",{}};
#ifdef Q_OS_WIN
 auto [code,pin]=projectPin(m_root,project);if(!pin)return {code,{}};
 auto state=std::make_unique<OwnedNewFile::State>();state->root=m_root;state->project=pin;NTSTATUS status=0;
 const auto name="take-"+take.id+".partial";
 state->file=relative(pin->directory,name,GENERIC_READ|GENERIC_WRITE,FILE_CREATE,false,status);
 if(state->file==INVALID_HANDLE_VALUE)return {quint32(status)==0xc0000035u?"take_exists":"write_failed",{}};
 if(!ordinary(state->file,false,m_root->volume) || finalName(state->file)!=m_root->finalPath+'/'+project.id+'/'+name)return {"source_identity_changed",{}};
 return {"ok",std::make_unique<OwnedNewFile>(std::move(state))};
#else
 return {"unsupported_backend",{}};
#endif
}
RecordingLibrary::NewTakeResult RecordingLibrary::createUniqueTake(const ProjectIdentity& project,const TakeIdentity& take,std::uint32_t rate,std::uint64_t maximum)
{
 if(const auto error=identityError(project);!error.isEmpty())return {error,{}};
 if(!validId(take.id) || take.label.toUtf8().size()>96 || (rate!=44100 && rate!=48000)
  || !maximum || maximum>std::uint64_t(rate)*120)return {"invalid_take",{}};
#ifdef Q_OS_WIN
 auto [code,pin]=projectPin(m_root,project);if(!pin)return {code,{}};
 const auto stem=QStringLiteral("take-")+take.id;
 NTSTATUS status=0;const auto audio=relative(pin->directory,stem+QStringLiteral(".partial"),GENERIC_READ|GENERIC_WRITE|DELETE,FILE_CREATE,false,status);
 if(audio==INVALID_HANDLE_VALUE)return {quint32(status)==0xc0000035u?QStringLiteral("take_exists"):QStringLiteral("write_failed"),{}};
 if(!ordinary(audio,false,m_root->volume) || finalName(audio)!=m_root->finalPath+'/'+project.id+'/'+stem+".partial")
 {CloseHandle(audio);return {"source_identity_changed",{}};}
 const auto journal=relative(pin->directory,stem+QStringLiteral(".journal"),GENERIC_READ|GENERIC_WRITE,FILE_CREATE,false,status);
 if(journal==INVALID_HANDLE_VALUE){CloseHandle(audio);return {quint32(status)==0xc0000035u?QStringLiteral("take_exists"):QStringLiteral("write_failed"),{}};}
 if(!ordinary(journal,false,m_root->volume) || finalName(journal)!=m_root->finalPath+'/'+project.id+'/'+stem+".journal")
 {CloseHandle(audio);CloseHandle(journal);return {"source_identity_changed",{}};}
 return {"ok",std::make_unique<OwnedLibraryTakeSink>(m_root,std::move(pin),audio,journal,project.id,take.id,rate,maximum)};
#else
 Q_UNUSED(take);Q_UNUSED(rate);Q_UNUSED(maximum);return {"unsupported_backend",{}};
#endif
}
RecordingLibrary::CheckedFile RecordingLibrary::openRecoveryCandidate(const ProjectIdentity& project,const QString& take)
{
 if(!validId(take))return {"invalid_identity",{}};
#ifdef Q_OS_WIN
 auto [code,pin]=projectPin(m_root,project,false);if(!pin)return {code,{}};
 auto state=std::make_unique<OwnedNewFile::State>();state->root=m_root;state->project=pin;NTSTATUS status=0;const auto name="take-"+take+".partial";
 state->file=relative(pin->directory,name,GENERIC_READ,FILE_OPEN,false,status);
 if(state->file==INVALID_HANDLE_VALUE || !ordinary(state->file,false,m_root->volume) || finalName(state->file)!=m_root->finalPath+'/'+project.id+'/'+name)return {"source_identity_changed",{}};
 LARGE_INTEGER size{};if(!GetFileSizeEx(state->file,&size) || size.QuadPart<0 || size.QuadPart>57600044)return {"resource_limit",{}};
 return {"ok",std::make_unique<RecoveryDevice>(std::move(state))};
#else
 return {"unsupported_backend",{}};
#endif
}
void RecordingLibrary::revoke(){if(m_root)m_root->revoked.store(true,std::memory_order_release);}
QJsonObject RecordingLibrary::identityRecord(const ProjectIdentity& project,QString& code)
{
#ifdef Q_OS_WIN
 auto [opened,pin]=projectPin(m_root,project,false);if(!pin){code=opened;return {};}
 NTSTATUS status=0;const auto file=relative(pin->directory,"identity.json",GENERIC_READ,FILE_OPEN,false,status);
 if(file==INVALID_HANDLE_VALUE){code=quint32(status)==0xc0000034u?"identity_not_registered":"source_identity_changed";return {};}
 const bool valid=ordinary(file,false,m_root->volume) && finalName(file)==m_root->finalPath+'/'+project.id+"/identity.json";
 const auto bytes=valid?readBounded(file,8192):QByteArray{};CloseHandle(file);
 QJsonParseError parse{};const auto doc=QJsonDocument::fromJson(bytes,&parse);
 if(!valid || parse.error!=QJsonParseError::NoError || !doc.isObject()){code="recovery_required";return {};}
 const auto record=doc.object();
 if(record["schema"]!=QJsonValue(1)){code="unsupported_schema";return {};}
 if(record.size()!=4 || record["project_id"]!=QJsonValue(project.id) || !record["current"].isObject() || !record["pending"].isObject()){code="recovery_required";return {};}
 code="ok";return record;
#else
 code="unsupported_backend";return {};
#endif
}
QString RecordingLibrary::storeIdentityRecord(const ProjectIdentity& project,const QJsonObject& record)
{
#ifdef Q_OS_WIN
 auto [code,pin]=projectPin(m_root,project);if(!pin)return code;
 QString priorCode;identityRecord(project,priorCode);
 if(priorCode!="ok" && priorCode!="identity_not_registered")return priorCode;
 const auto bytes=QJsonDocument(record).toJson(QJsonDocument::Compact);
 if(bytes.size()>8192 || record.size()!=4 || record["schema"]!=QJsonValue(1) || record["project_id"]!=QJsonValue(project.id)
  || !record["current"].isObject() || !record["pending"].isObject())return "invalid_manifest";
 NTSTATUS status=0;const auto name="identity-"+QUuid::createUuid().toString(QUuid::WithoutBraces)+".new";
 const auto file=relative(pin->directory,name,GENERIC_READ|GENERIC_WRITE|DELETE,FILE_CREATE,false,status);
 if(file==INVALID_HANDLE_VALUE)return "write_failed";
 bool saved=ordinary(file,false,m_root->volume) && finalName(file)==m_root->finalPath+'/'+project.id+'/'+name && writeAll(file,bytes);
 const QString target="identity.json";const auto size=offsetof(FILE_RENAME_INFORMATION,FileName)+size_t(target.size())*sizeof(wchar_t);
 std::vector<std::uint64_t> memory((size+7)/8,0);auto* rename=reinterpret_cast<FILE_RENAME_INFORMATION*>(memory.data());
 // A simple NT rename with NULL RootDirectory resolves in the source handle's
 // own directory, never the process working directory. Only this managed metadata
 // file may be replaced; original take publication must remain no-clobber.
 rename->ReplaceIfExists=TRUE;rename->RootDirectory=nullptr;rename->FileNameLength=ULONG(target.size()*2);std::memcpy(rename->FileName,target.utf16(),size_t(target.size())*2);
 if(saved)
 {
  const auto set=reinterpret_cast<decltype(&NtSetInformationFile)>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtSetInformationFile"));IO_STATUS_BLOCK io{};
  const auto renamed=set?set(file,&io,rename,ULONG(size),FileRenameInformation):NTSTATUS(0xc0000002u);
  if(renamed<0){qWarning("recording identity publication failed (native status %lx)",ULONG(renamed));saved=false;}
  else saved=finalName(file)==m_root->finalPath+'/'+project.id+"/identity.json" && FlushFileBuffers(file);
 }
 CloseHandle(file);return saved?"ok":"write_failed";
#else
 return "unsupported_backend";
#endif
}
}
