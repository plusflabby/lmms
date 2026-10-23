#ifndef LMMS_RECORDING_PROJECT_IDENTITY_H
#define LMMS_RECORDING_PROJECT_IDENTITY_H
#include "RecordingLibrary.h"
#include <QPointer>
#include <QDomElement>
namespace lmms {class Song;class DataFile;}
namespace lmms::recording {
class LMMS_EXPORT RecordingProjectIdentity
{
public:
 struct IdentityResult {QString code;ProjectIdentity identity;};
 explicit RecordingProjectIdentity(Song* owner);
 ~RecordingProjectIdentity();
 IdentityResult getOrCreateForCurrentProject(RecordingLibrary&);
 ProjectIdentity cachedIdentity() const;
 static bool metadataSupported(const QDomElement& head);
 void resetForNewProject();
 void acceptLoad(const QDomElement& head,const QString& source);
 bool beginSave(DataFile&,const QString& target,bool withResources);
 bool finishSave(const QString& target,bool nativeResult);
 quint64 revision() const;
 QString lastCode() const;
private:
 struct State;
 std::unique_ptr<State> m_state;
 bool reject(const QString& code);
};
}
#endif
