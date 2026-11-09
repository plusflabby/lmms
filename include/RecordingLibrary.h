#ifndef LMMS_RECORDING_LIBRARY_H
#define LMMS_RECORDING_LIBRARY_H
#include <QObject>
#include <QIODevice>
#include <QString>
#include <QJsonObject>
#include <memory>
#include <functional>
#include "RecordingWriter.h"
#include "RecordingTakeReceipt.h"
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
class RecordingRecovery;
class LMMS_EXPORT RecordingLibrary : public QObject
{
public:
 struct RootState;
 struct Approval {QString code;std::unique_ptr<RecordingLibrary> library;};
 struct NewFileResult {QString code;std::unique_ptr<OwnedNewFile> file;};
 struct NewTakeResult {QString code;std::unique_ptr<RecordingWriterSink> sink;std::unique_ptr<RecordingReceiptSink> receipt;};
 struct CheckedFile {QString code;std::unique_ptr<QIODevice> source;};
 struct SealedSourceResult {QString code;std::unique_ptr<SealedTakeToken> source;};
 struct RecoveryName {QString code,projectId,takeId;bool candidate=false,end=false;};
 class RecoveryScan
 {
 public:
  ~RecoveryScan();
  RecoveryName next();
 private:
  friend class RecordingLibrary;
  struct State;
  explicit RecoveryScan(std::unique_ptr<State>);
  std::unique_ptr<State> m_state;
 };
 struct RecoveryScanResult {QString code;std::unique_ptr<RecoveryScan> scan;};
 struct RecoveryFiles
 {
  QString code;
  std::unique_ptr<QIODevice> audio,journal,receipt,provenance,dismissal;
  RecordingFileIdentity audioIdentity,journalIdentity;
  bool sealed=false;
 };
 // Internal, explicit sharing capability. It does not reopen or broaden the
 // exclusive recording-library lock and is never exposed through IPC.
 struct ManagedRootGrant {QString code,selectedPath;std::shared_ptr<void> lock;std::function<bool()> valid;};
 static Approval authorizeLocalRoot(const QString&);
 ManagedRootGrant managedRootGrant() const;
 ~RecordingLibrary() override;
 QString prepareProjectFolder(const ProjectIdentity&);
 NewFileResult createUniquePartial(const ProjectIdentity&,const TakeIdentity&);
 NewTakeResult createUniqueTake(const ProjectIdentity&,const TakeIdentity&,std::uint32_t sampleRate,std::uint64_t maxRawFrames);
 SealedSourceResult openSealedTake(const ProjectIdentity&,const QString& takeId);
 CheckedFile openRecoveryCandidate(const ProjectIdentity&,const QString& takeId);
 void revoke();
private:
 friend class RecordingProjectIdentity;
 friend class RecordingRecovery;
 enum class RecoveryMetadata {Provenance,Dismissal};
 QString recoveryAdmissionCode() const;
 RecoveryScanResult beginRecoveryScan();
 RecoveryFiles readRecoveryFiles(const ProjectIdentity&,const QString& takeId);
 QString writeRecoveryMetadata(const ProjectIdentity&,const QString& takeId,RecoveryMetadata,const QByteArray&);
 std::unique_ptr<SealedTakeToken> mintRecoveryToken(const RawTakeFacts&);
 explicit RecordingLibrary(std::shared_ptr<RootState>);
 QJsonObject identityRecord(const ProjectIdentity&,QString& code);
 QString storeIdentityRecord(const ProjectIdentity&,const QJsonObject&);
 std::shared_ptr<RootState> m_root;
};
}
#endif
