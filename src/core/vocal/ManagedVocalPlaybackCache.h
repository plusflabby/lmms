#ifndef LMMS_MANAGED_VOCAL_PLAYBACK_CACHE_H
#define LMMS_MANAGED_VOCAL_PLAYBACK_CACHE_H

#include "ManagedVocalAssetStore.h"
#include <QThreadPool>
#include <memory>
#include <mutex>
#include <vector>
#include "lmms_export.h"

namespace lmms {

struct ManagedVocalPlaybackBufferResult
{
	QString code;
	std::shared_ptr<const std::vector<float>> samples;
};

class LMMS_EXPORT ManagedVocalPlaybackLoadTicket
{
public:
	bool isComplete() const;
	ManagedVocalPlaybackBufferResult result() const;

private:
	friend class ManagedVocalPlaybackCache;
	void finish(ManagedVocalPlaybackBufferResult result);
	mutable std::mutex m_mutex;
	bool m_complete = false;
	ManagedVocalPlaybackBufferResult m_result;
};

struct ManagedVocalPlaybackLoadRequest
{
	QString code;
	std::shared_ptr<ManagedVocalPlaybackLoadTicket> ticket;
};

class LMMS_EXPORT ManagedVocalPlaybackCache
{
public:
	explicit ManagedVocalPlaybackCache(quint64 maximumResidentBytes = 256ULL * 1024 * 1024);
	~ManagedVocalPlaybackCache();
	ManagedVocalPlaybackCache(const ManagedVocalPlaybackCache&) = delete;
	ManagedVocalPlaybackCache& operator=(const ManagedVocalPlaybackCache&) = delete;

	static ManagedVocalPlaybackCache& instance();
	ManagedVocalPlaybackBufferResult acquire(
		const ManagedVocalAssetStore& store,
		const ManagedVocalVariantReference& reference);
	ManagedVocalPlaybackLoadRequest requestLoad(
		const ManagedVocalAssetStore& store,
		const ManagedVocalVariantReference& reference);
	quint64 usedBytes() const;

private:
	struct State;
	static ManagedVocalPlaybackBufferResult acquireFromState(
		const std::shared_ptr<State>& state,
		const ManagedVocalAssetStore& store,
		const ManagedVocalVariantReference& reference);
	std::shared_ptr<State> m_state;
	QThreadPool m_loaderPool;
};

} // namespace lmms

#endif
