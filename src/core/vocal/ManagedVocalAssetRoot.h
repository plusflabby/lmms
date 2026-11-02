#ifndef LMMS_MANAGED_VOCAL_ASSET_ROOT_H
#define LMMS_MANAGED_VOCAL_ASSET_ROOT_H

#include <QString>
#include <memory>
#include "lmms_export.h"

namespace lmms {

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
	~ManagedVocalAssetRoot();
	ManagedVocalAssetRoot(const ManagedVocalAssetRoot&) = delete;
	ManagedVocalAssetRoot& operator=(const ManagedVocalAssetRoot&) = delete;

	bool isAuthorized() const;
	QString rootId() const;
	void revoke();

private:
	friend class ManagedVocalAssetStore;
	explicit ManagedVocalAssetRoot(std::shared_ptr<ManagedVocalAssetRootState> state);
	std::shared_ptr<ManagedVocalAssetRootState> m_state;
};

} // namespace lmms

#endif
