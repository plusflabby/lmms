#include "RecordingProjectIdentity.h"
#include "Song.h"
#include "DataFile.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTextStream>
#include <QUuid>
#include <array>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
namespace lmms::recording {
namespace {
QString freshId(){return QUuid::createUuid().toString(QUuid::WithoutBraces);}
bool uuid(const QString& text){const QUuid id(text);return !id.isNull() && id.toString(QUuid::WithoutBraces)==text;}
bool digest(const QString& text)
{
 if(text.size()!=64)return false;
 for(const auto c:text)if(!((c>='a' && c<='f') || (c>='0' && c<='9')))return false;
 return true;
}
QString normalized(const QString& path){return QFileInfo(path).absoluteFilePath().replace('\\','/');}
struct Snapshot {QString code="source_identity_changed",id,sha;};
Snapshot snapshot(const QString& path)
{
 Snapshot result;
#ifdef Q_OS_WIN
 const auto file=CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
 if(file==INVALID_HANDLE_VALUE)return result;
 BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER length{};
 if(GetFileInformationByHandle(file,&info) && GetFileType(file)==FILE_TYPE_DISK && !(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))
  && info.nNumberOfLinks==1 && GetFileSizeEx(file,&length) && length.QuadPart>=0 && length.QuadPart<=67108864)
 {
  QCryptographicHash hash(QCryptographicHash::Sha256);std::array<char,65536> buffer{};DWORD count=0;bool ok=true;
  do{if(!ReadFile(file,buffer.data(),DWORD(buffer.size()),&count,nullptr)){ok=false;break;}hash.addData(buffer.data(),int(count));}while(count);
  if(ok)
  {
   const auto identity=QByteArray::number(qulonglong(info.dwVolumeSerialNumber))+':'+QByteArray::number(qulonglong(info.nFileIndexHigh))+':'+QByteArray::number(qulonglong(info.nFileIndexLow));
   result={"ok",QString::fromLatin1(QCryptographicHash::hash(identity,QCryptographicHash::Sha256).toHex()),QString::fromLatin1(hash.result().toHex())};
  }
 }
 CloseHandle(file);
#endif
 return result;
}
bool bindingValid(const QJsonObject& value,bool pending)
{
 if(value.isEmpty())return true;
 if(value.size()!=(pending?3:4) || !uuid(value["nonce"].toString()) || !digest(value["digest"].toString()))return false;
 const auto path=value["path"].toString();
 if(path.isEmpty() || path.size()>1024 || path.contains(QChar(0)))return false;
 return pending || digest(value["file_id"].toString());
}
QJsonObject recordFor(const ProjectIdentity& identity,const QJsonObject& current,const QJsonObject& pending)
{return {{"schema",1},{"project_id",identity.id},{"current",current},{"pending",pending}};}
}
struct RecordingProjectIdentity::State
{
 Song* owner;
 QPointer<RecordingLibrary> library;
 ProjectIdentity identity;
 QString nonce,sourcePath;
 QString lastCode;
 bool loaded=false;
 quint64 revision=1;
 struct Pending {bool active=false;ProjectIdentity identity;QString target,nonce,digest;QJsonObject current;} pending;
 explicit State(Song* song):owner(song){}
};
RecordingProjectIdentity::RecordingProjectIdentity(Song* owner):m_state(std::make_unique<State>(owner)){}
RecordingProjectIdentity::~RecordingProjectIdentity()=default;
quint64 RecordingProjectIdentity::revision() const{return m_state->revision;}
ProjectIdentity RecordingProjectIdentity::cachedIdentity() const{return m_state->identity;}
QString RecordingProjectIdentity::lastCode() const{return m_state->lastCode;}
bool RecordingProjectIdentity::reject(const QString& code){m_state->lastCode=code;return false;}
bool RecordingProjectIdentity::metadataSupported(const QDomElement& head)
{
 const auto field=head.firstChildElement("recording_project");if(field.isNull())return true;
 return field.nextSiblingElement("recording_project").isNull() && field.attributes().count()==3 && field.attribute("schema")=="1"
  && uuid(field.attribute("id")) && uuid(field.attribute("nonce")) && field.firstChild().isNull();
}
void RecordingProjectIdentity::resetForNewProject()
{
 auto root=m_state->library;const auto next=m_state->revision+1;auto* owner=m_state->owner;
 m_state=std::make_unique<State>(owner);m_state->library=root;m_state->revision=next;
}
void RecordingProjectIdentity::acceptLoad(const QDomElement& head,const QString& source)
{
 const auto field=head.firstChildElement("recording_project");if(field.isNull())return;
 m_state->identity={field.attribute("id"),1};m_state->nonce=field.attribute("nonce");m_state->sourcePath=normalized(source);m_state->loaded=true;
}
RecordingProjectIdentity::IdentityResult RecordingProjectIdentity::getOrCreateForCurrentProject(RecordingLibrary& library)
{
 if(m_state->pending.active)return {"recording_busy",{}};
 m_state->library=&library;
 if(m_state->identity.id.isEmpty()){m_state->identity={freshId(),1};m_state->sourcePath=m_state->owner->projectFileName().isEmpty()?QString{}:normalized(m_state->owner->projectFileName());}
 auto code=library.prepareProjectFolder(m_state->identity);if(code!="ok")return {code,{}};
 if(!m_state->loaded)return {"ok",m_state->identity};
 const auto actual=snapshot(m_state->sourcePath);if(actual.code!="ok")return {actual.code,{}};
 QString readCode;const auto record=library.identityRecord(m_state->identity,readCode);
 if(readCode!="ok" && readCode!="identity_not_registered")return {readCode,{}};
 const auto current=record["current"].toObject(),pending=record["pending"].toObject();
 if(!bindingValid(current,false) || !bindingValid(pending,true))return {"recovery_required",{}};
 if(current["file_id"]==QJsonValue(actual.id) && current["digest"]==QJsonValue(actual.sha) && current["nonce"]==QJsonValue(m_state->nonce))return {"ok",m_state->identity};
 if(pending["path"]==QJsonValue(m_state->sourcePath) && pending["digest"]==QJsonValue(actual.sha) && pending["nonce"]==QJsonValue(m_state->nonce))
 {
  const QJsonObject recovered{{"path",m_state->sourcePath},{"digest",actual.sha},{"nonce",m_state->nonce},{"file_id",actual.id}};
  code=library.storeIdentityRecord(m_state->identity,recordFor(m_state->identity,recovered,{}));
  return {code,code=="ok"?m_state->identity:ProjectIdentity{}};
 }
 // A copied serialized UUID is not authority to reuse the original recording folder.
 m_state->identity={freshId(),1};m_state->loaded=false;m_state->nonce.clear();
 code=library.prepareProjectFolder(m_state->identity);return {code,code=="ok"?m_state->identity:ProjectIdentity{}};
}
bool RecordingProjectIdentity::beginSave(DataFile& file,const QString& target,bool withResources)
{
 m_state->lastCode.clear();
 if(m_state->identity.id.isEmpty())return true;
 if(!m_state->library)return reject("unauthorized_library");
 if(m_state->pending.active)return reject("recording_busy");
 if(withResources)return reject("unsupported_project_bundle");
 const auto resolved=getOrCreateForCurrentProject(*m_state->library);if(resolved.code!="ok")return reject(resolved.code);
 const auto full=normalized(file.nameWithExtension(target));if(full.size()>1024)return reject("resource_limit");
 ProjectIdentity identity=m_state->identity;
 if(!m_state->sourcePath.isEmpty() && m_state->sourcePath!=full)identity={freshId(),1};
 const auto nonce=freshId();auto field=file.createElement("recording_project");field.setAttribute("schema",1);field.setAttribute("id",identity.id);field.setAttribute("nonce",nonce);file.head().appendChild(field);
 QString text;QTextStream stream(&text);file.write(stream);stream.flush();const auto xml=text.toUtf8();if(xml.size()>33554432)return false;
 const auto encoded=(full.endsWith(".mmpz") || full.endsWith(".xptz"))?qCompress(xml):xml;
 const auto sha=QString::fromLatin1(QCryptographicHash::hash(encoded,QCryptographicHash::Sha256).toHex());
 const auto folder=m_state->library->prepareProjectFolder(identity);if(folder!="ok")return reject(folder);
 QString code;const auto prior=m_state->library->identityRecord(identity,code);
 if(code!="ok" && code!="identity_not_registered")return reject(code);
 const auto current=prior["current"].toObject(),priorPending=prior["pending"].toObject();if(!bindingValid(current,false) || !bindingValid(priorPending,true))return reject("recovery_required");
 const QJsonObject pending{{"nonce",nonce},{"path",full},{"digest",sha}};
 const auto stored=m_state->library->storeIdentityRecord(identity,recordFor(identity,current,pending));if(stored!="ok")return reject(stored);
 m_state->pending={true,identity,full,nonce,sha,current};return true;
}
bool RecordingProjectIdentity::finishSave(const QString& target,bool nativeResult)
{
 if(m_state->identity.id.isEmpty())return nativeResult;
 if(!m_state->pending.active || !m_state->library)return reject("unauthorized_library");
 const auto pending=m_state->pending;m_state->pending={};
 if(!nativeResult || normalized(target)!=pending.target)return reject("write_failed");
 const auto actual=snapshot(pending.target);
 // DataFile's success flag does not establish that its final rename succeeded.
 if(actual.code!="ok" || actual.sha!=pending.digest)return reject("project_publish_mismatch");
 const QJsonObject current{{"nonce",pending.nonce},{"path",pending.target},{"digest",actual.sha},{"file_id",actual.id}};
 const auto stored=m_state->library->storeIdentityRecord(pending.identity,recordFor(pending.identity,current,{}));if(stored!="ok")return reject(stored);
 m_state->identity=pending.identity;m_state->nonce=pending.nonce;m_state->sourcePath=pending.target;m_state->loaded=true;
 return true;
}
}
