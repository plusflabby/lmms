#ifndef LMMS_RECORDING_RECOVERY_H
#define LMMS_RECORDING_RECOVERY_H
#include "RecordingLibrary.h"
#include "RecordingTakeTypes.h"
#include <functional>
#include <memory>
#include <vector>
namespace lmms::recording {
struct RecoveryCursor {QString token;};
struct RecoveryPage
{
 QString code;
 std::vector<TakeManifest> takes;
 RecoveryCursor next;
 bool hasMore=false;
 std::uint32_t inspectedCandidates=0;
};
struct RecoveryResult
{
 QString code;
 TakeManifest manifest;
 std::unique_ptr<SealedTakeToken> token;
 bool ok() const noexcept{return code==QStringLiteral("recovered");}
};
class RecordingRecovery
{
public:
 using TakeIdGenerator=std::function<QString()>;
 explicit RecordingRecovery(TakeIdGenerator={});
 ~RecordingRecovery();
 RecordingRecovery(const RecordingRecovery&)=delete;
 RecordingRecovery& operator=(const RecordingRecovery&)=delete;
 RecoveryPage scan(RecordingLibrary&,RecoveryCursor={},std::uint32_t limit=25);
 RecoveryResult recover(const QString& takeId);
 QString dismiss(const QString& takeId);
 std::vector<TakeManifest> recentTakes() const;
private:
 struct Impl;
 std::unique_ptr<Impl> m_impl;
};
}
#endif
