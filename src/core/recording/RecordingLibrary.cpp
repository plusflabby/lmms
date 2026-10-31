#include "RecordingLibrary.h"
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
#ifdef Q_OS_WIN
bool authorized(const std::shared_ptr<RecordingLibrary::RootState>& root)
{
 return root && !root->revoked.load(std::memory_order_acquire) && !root->ancestors.empty()
  && ordinary(root->ancestors.back(),true,root->volume) && finalName(root->ancestors.back())==root->finalPath;
}
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
 struct RenameInformation{BOOLEAN replace;HANDLE root;ULONG nameBytes;WCHAR name[1];};
 using NtSetInformationFileFunction=NTSTATUS (NTAPI*)(HANDLE,PIO_STATUS_BLOCK,PVOID,ULONG,FILE_INFORMATION_CLASS);
 const QString target="identity.json";const auto size=offsetof(RenameInformation,name)+size_t(target.size())*sizeof(wchar_t);
 std::vector<std::uint64_t> memory((size+7)/8,0);auto* rename=reinterpret_cast<RenameInformation*>(memory.data());
 // A simple NT rename with NULL RootDirectory resolves in the source handle's
 // own directory, never the process working directory. Only this managed metadata
 // file may be replaced; original take publication must remain no-clobber.
 rename->replace=TRUE;rename->root=nullptr;rename->nameBytes=ULONG(target.size()*2);std::memcpy(rename->name,target.utf16(),size_t(target.size())*2);
 if(saved)
 {
  const auto set=reinterpret_cast<NtSetInformationFileFunction>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"NtSetInformationFile"));IO_STATUS_BLOCK io{};
  const auto renamed=set?set(file,&io,rename,ULONG(size),static_cast<FILE_INFORMATION_CLASS>(10)):NTSTATUS(0xc0000002u); // FileRenameInformation
  if(renamed<0){qWarning("recording identity publication failed (native status %lx)",ULONG(renamed));saved=false;}
  else saved=finalName(file)==m_root->finalPath+'/'+project.id+"/identity.json" && FlushFileBuffers(file);
 }
 CloseHandle(file);return saved?"ok":"write_failed";
#else
 return "unsupported_backend";
#endif
}
}
