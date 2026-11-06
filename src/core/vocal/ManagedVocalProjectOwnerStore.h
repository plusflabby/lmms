#ifndef LMMS_MANAGED_VOCAL_PROJECT_OWNER_STORE_H
#define LMMS_MANAGED_VOCAL_PROJECT_OWNER_STORE_H

#include "ManagedVocalAssetRoot.h"
#include <QString>
#include <memory>
#include <optional>

namespace lmms {

struct ManagedVocalProjectOwner
{
	QString projectId;
	QString rootId;
	QString fileIdentity;
	QString documentSha256;
};

// Internal-only binding records for saved project snapshots. A record read
// after restart is a candidate, not proof of an owner: callers must validate
// it against a separately trusted owner. This primitive does not implement save
// transactions, relocation, recovery, or playback authorization.
class LMMS_EXPORT ManagedVocalProjectOwnerStore
{
public:
	struct Result { QString code; std::optional<ManagedVocalProjectOwner> owner; };
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

private:
	Result ensureImpl(const QString& projectPath, const QString& fileIdentity,
		const QString& documentSha256, bool requireExpected) const;
	std::shared_ptr<ManagedVocalAssetRoot> m_root;
};

}
#endif
