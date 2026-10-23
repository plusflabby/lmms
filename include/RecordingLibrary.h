#ifndef LMMS_RECORDING_LIBRARY_H
#define LMMS_RECORDING_LIBRARY_H
#include <QObject>
#include <QIODevice>
#include <QString>
#include <QJsonObject>
#include <memory>
#include "lmms_export.h"
namespace lmms::recording {
struct ProjectIdentity {QString id;unsigned schema=1;};
struct TakeIdentity {QString id,label;};
class LMMS_EXPORT OwnedNewFile
{
public:
 struct State;
 explicit OwnedNewFile(std::unique_ptr<State>);
 ~OwnedNewFile();
 qint64 append(const QByteArray&);
 bool flush();
private:
 std::unique_ptr<State> m_state;
};
class LMMS_EXPORT RecordingLibrary : public QObject
{
public:
 struct RootState;
 struct Approval {QString code;std::unique_ptr<RecordingLibrary> library;};
 struct NewFileResult {QString code;std::unique_ptr<OwnedNewFile> file;};
 struct CheckedFile {QString code;std::unique_ptr<QIODevice> source;};
 static Approval authorizeLocalRoot(const QString&);
 ~RecordingLibrary() override;
 QString prepareProjectFolder(const ProjectIdentity&);
 NewFileResult createUniquePartial(const ProjectIdentity&,const TakeIdentity&);
 CheckedFile openRecoveryCandidate(const ProjectIdentity&,const QString& takeId);
 void revoke();
private:
 friend class RecordingProjectIdentity;
 explicit RecordingLibrary(std::shared_ptr<RootState>);
 QJsonObject identityRecord(const ProjectIdentity&,QString& code);
 QString storeIdentityRecord(const ProjectIdentity&,const QJsonObject&);
 std::shared_ptr<RootState> m_root;
};
}
#endif
