#ifndef LMMS_MANAGED_VOCAL_ASSET_ROOT_H
#define LMMS_MANAGED_VOCAL_ASSET_ROOT_H

#include <QString>
#include <memory>
#include "lmms_export.h"

namespace lmms {

namespace recording { class RecordingLibrary; }

struct ManagedVocalAssetRootState;

class LMMS_EXPORT ManagedVocalAssetRoot
{
public:
	struct Approval
	{
		QString code;
		std::shared_ptr<ManagedVocalAssetRoot> root;
	};

	static Approval authorizeLocalRoot(const QString& selectedRoot);
	static Approval authorizeConfiguredRoot();
	static Approval authorizeConfiguredRoot(recording::RecordingLibrary* sharedRecordingLibrary);
	// Explicitly shares an already authorized recording capability; independent
	// authorizations retain the existing exclusive interprocess lock.
	static Approval authorizeRecordingRoot(recording::RecordingLibrary& library);
	static Approval authorizeRecordingRoot(recording::RecordingLibrary& library, const QString& expectedRoot);
	~ManagedVocalAssetRoot();
	ManagedVocalAssetRoot(const ManagedVocalAssetRoot&) = delete;
	ManagedVocalAssetRoot& operator=(const ManagedVocalAssetRoot&) = delete;

	bool isAuthorized() const;
	QString rootId() const;
	void revoke();

private:
	friend class ManagedVocalAssetStore;
	friend class ManagedVocalProjectOwnerStore;
	explicit ManagedVocalAssetRoot(std::shared_ptr<ManagedVocalAssetRootState> state);
	std::shared_ptr<ManagedVocalAssetRootState> m_state;
};

} // namespace lmms

#endif
