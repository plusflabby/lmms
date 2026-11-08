#ifndef LMMS_MANAGED_VOCAL_PROJECT_OWNER_STORE_H
#define LMMS_MANAGED_VOCAL_PROJECT_OWNER_STORE_H

#include "ManagedVocalAssetRoot.h"
#include <QString>
#include <memory>
#include <optional>

namespace lmms {
class ProjectSavePublication;

struct ManagedVocalProjectOwner
{
	QString projectId;
	QString rootId;
	QString fileIdentity;
	QString documentSha256;
};

// Pure recovery classification only. It is not authorization to touch files:
// callers must authenticate the journal and pin and revalidate all paths.
struct ManagedVocalSaveFileEvidence
{
	enum class State { Unknown, Missing, Verified };
	State state = State::Unknown;
	QString fileIdentity;
	QString documentSha256;
};

struct ManagedVocalSaveRecoveryEvidence
{
	ManagedVocalProjectOwner original;
	QString stagedFileIdentity;
	QString stagedSha256;
	ManagedVocalSaveFileEvidence target, backup, stage;
};

enum class ManagedVocalSaveRecoveryDecision
{
	RecoveryRequired,
	OriginalIntact,
	RestoreOriginal,
	FinalizePublished
};

struct ManagedVocalPendingSaveJournal
{
	QString transactionId;
	ManagedVocalProjectOwner original;
	QString targetPathHash;
	QString stagedFileIdentity;
	QString stagedSha256;
};

class ManagedVocalSaveAsSource
{
public:
	const QString& projectPath() const { return path; }
	const ManagedVocalProjectOwner& projectOwner() const { return owner; }
	bool isAuthorized() const { return hold && root && root->isAuthorized(); }
private:
	friend class ManagedVocalProjectOwnerStore;
	QString path;
	ManagedVocalProjectOwner owner;
	std::shared_ptr<void> hold;
	std::shared_ptr<ManagedVocalAssetRoot> root;
};

struct ManagedVocalSaveAsJournal
{
	QString transactionId, sourcePathHash, targetPathHash;
	ManagedVocalProjectOwner source;
	bool destinationMissing = false;
	QString destinationFileIdentity, destinationSha256, stagedFileIdentity, stagedSha256;
};

LMMS_EXPORT ManagedVocalSaveRecoveryDecision classifyManagedVocalSaveRecovery(
	const ManagedVocalSaveRecoveryEvidence& evidence);

// Internal-only binding records for saved project snapshots. A record read
// after restart is a candidate, not proof of an owner: callers must validate
// it against a separately trusted owner. This primitive does not implement save
// transactions, relocation, recovery, or playback authorization.
class LMMS_EXPORT ManagedVocalProjectOwnerStore
{
public:
	struct Result { QString code; std::optional<ManagedVocalProjectOwner> owner; };
	struct HoldResult { QString code; std::shared_ptr<void> hold; };
	struct SaveJournalResult { QString code; std::optional<ManagedVocalPendingSaveJournal> journal; };
	struct SaveAsSourceResult { QString code; std::shared_ptr<ManagedVocalSaveAsSource> source; };
	struct SaveAsJournalResult { QString code; std::optional<ManagedVocalSaveAsJournal> journal; };
	explicit ManagedVocalProjectOwnerStore(std::shared_ptr<ManagedVocalAssetRoot> root);
	Result lookup(const QString& projectPath) const;
	Result ensure(const QString& projectPath) const;
	// Both values must come from a previously trusted project snapshot. The
	// identity is lowercase SHA-256 of decimal volume:index-high:index-low;
	// the document digest is lowercase SHA-256 hex of the serialized bytes.
	// A mismatch fails before any owner record or credential is created.
	Result ensureExpected(const QString& projectPath, const QString& fileIdentity, const QString& documentSha256) const;
	// Accepts the native DataFile snapshot representation, after the caller has
	// established that it belongs to the currently loaded project.
	Result ensureLoadedSnapshot(const QString& projectPath, const QString& sourceFileId,
		const QByteArray& sourceSha256) const;
	QString verify(const QString& projectPath, const ManagedVocalProjectOwner& owner) const;
	// The returned hold pins the validated file and any existing .bak against
	// writes and replacement; unreadable or unsafe backups fail closed.
	// The retained pin owns native handles only and can be released on a worker.
	HoldResult holdVerified(const QString& projectPath, const ManagedVocalProjectOwner& owner) const;
	// This writes only an authenticated pending record. It does not prove that
	// the stage was created by this process, authorize moves, or perform recovery.
	SaveJournalResult prepareSaveJournal(const QString& projectPath, const QString& transactionId,
		const ManagedVocalProjectOwner& owner) const;
	SaveJournalResult prepareSaveJournal(const ProjectSavePublication& publication,
		const ManagedVocalProjectOwner& owner) const;
	SaveJournalResult loadSaveJournal(const QString& projectPath, const QString& transactionId) const;
	SaveJournalResult findPendingSaveJournal(const QString& projectPath, const ManagedVocalProjectOwner& owner) const;
	// Recovery-only discovery: a missing index is ambiguous, never proof that
	// no transaction exists. Returned records still do not authorize file moves.
	SaveJournalResult findPendingSaveJournalForTarget(const QString& projectPath) const;
	// Internal save step; requires both an authenticated managed journal and
	// the retained, verified native publication. It does not clear the journal.
	Result bindPublishedSnapshot(const ProjectSavePublication& publication,
		const ManagedVocalProjectOwner& original) const;
	QString finishPublishedSnapshot(const ProjectSavePublication& publication,
		const ManagedVocalProjectOwner& original) const;
	QString recoverPublishedSnapshot(const QString& projectPath) const;
	QString recoverUnpublishedSnapshot(const QString& projectPath) const;
	SaveAsSourceResult holdSaveAsSource(const QString& projectPath, const ManagedVocalProjectOwner& owner) const;
	SaveAsJournalResult prepareSaveAsJournal(const ManagedVocalSaveAsSource& source,
		const ProjectSavePublication& destination) const;
	Result bindPublishedSaveAs(const ManagedVocalSaveAsSource& source,
		const ProjectSavePublication& destination) const;
	QString finishPublishedSaveAs(const ManagedVocalSaveAsSource& source,
		const ProjectSavePublication& destination) const;
	QString recoverPublishedSaveAs(const QString& destinationPath) const;
	QString recoverUnpublishedSaveAs(const QString& destinationPath) const;

private:
	Result ensureImpl(const QString& projectPath, const QString& fileIdentity,
		const QString& documentSha256, bool requireExpected) const;
	QString recoverSnapshot(const QString& projectPath, bool published) const;
	QString recoverSaveAs(const QString& destinationPath, bool published) const;
	std::shared_ptr<ManagedVocalAssetRoot> m_root;
};

}
#endif
