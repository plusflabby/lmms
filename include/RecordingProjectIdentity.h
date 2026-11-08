#ifndef LMMS_RECORDING_PROJECT_IDENTITY_H
#define LMMS_RECORDING_PROJECT_IDENTITY_H
#include "RecordingLibrary.h"
#include <QPointer>
#include <QDomElement>
namespace lmms {class Song;class DataFile;class ProjectSavePublication;}
namespace lmms::recording {
class LMMS_EXPORT RecordingProjectIdentity
{
public:
 struct IdentityResult {QString code;ProjectIdentity identity;};
 explicit RecordingProjectIdentity(Song* owner);
 ~RecordingProjectIdentity();
 IdentityResult getOrCreateForCurrentProject(RecordingLibrary&);
 ProjectIdentity cachedIdentity() const;
 bool sourceIdentityTrusted() const noexcept;
 // Internal capability handoff, available only while this source is trusted.
 RecordingLibrary* managedRootLibrary() const noexcept;
 // Only lends the already-authorized root lock; callers must independently
 // authenticate the managed owner and must not infer recording identity trust.
 RecordingLibrary* managedRootLibraryForOwner() const noexcept;
 static bool metadataSupported(const QDomElement& head);
 void resetForNewProject();
 void acceptLoad(const QDomElement& head,const QString& source);
 bool beginSave(DataFile&,const QString& target,bool withResources);
 bool finishSave(const QString& target,bool nativeResult);
 bool finishSave(const QString& target,bool nativeResult,const ProjectSavePublication& publication);
 bool suspendUncertainSave(const QString& target);
 quint64 revision() const;
 QString lastCode() const;
private:
 struct State;
 std::unique_ptr<State> m_state;
 IdentityResult resolveCurrentProject(RecordingLibrary&,bool allowFork);
 bool reject(const QString& code);
};
}
#endif
