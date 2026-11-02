#include "ManagedVocalPlaybackCache.h"

#include <QHash>
#include <QRunnable>
#include <QSet>
#include <algorithm>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>

namespace lmms {

struct ManagedVocalPlaybackCache::State
{
	struct Reservation
	{
		std::weak_ptr<State> state;
		std::atomic_bool released{false};
		quint64 bytes = 0;
		void release()
		{
			if (released.exchange(true, std::memory_order_acq_rel)) return;
			const auto owner = state.lock();
			if (!owner) return;
			std::lock_guard lock(owner->mutex);
			owner->usedBytes -= std::min(owner->usedBytes, bytes);
			owner->condition.notify_all();
		}
		~Reservation()
		{
			release();
		}
	};

	mutable std::mutex mutex;
	std::condition_variable condition;
	QHash<QString, std::weak_ptr<const std::vector<float>>> buffers;
	QSet<QString> loading;
	quint64 maximumResidentBytes = 0;
	quint64 usedBytes = 0;
	quint64 outstandingLoads = 0;
	quint64 maximumOutstandingLoads = 8;
};

bool validReference(const ManagedVocalVariantReference& reference)
{
	return reference.frames != 0 && (reference.channels == 1 || reference.channels == 2)
		&& (reference.sampleRate == 44100 || reference.sampleRate == 48000)
		&& reference.frames <= std::numeric_limits<quint64>::max()
			/ quint64(reference.channels * int(sizeof(float)));
}

QString cacheKey(const ManagedVocalVariantReference& reference)
{
	return reference.assetRootId + QLatin1Char(':') + reference.assetId
		+ QLatin1Char(':') + reference.mediaSha256;
}

bool ManagedVocalPlaybackLoadTicket::isComplete() const
{
	std::lock_guard lock(m_mutex);
	return m_complete;
}

ManagedVocalPlaybackBufferResult ManagedVocalPlaybackLoadTicket::result() const
{
	std::lock_guard lock(m_mutex);
	return m_complete ? m_result
		: ManagedVocalPlaybackBufferResult{QStringLiteral("job_busy"), {}};
}

void ManagedVocalPlaybackLoadTicket::finish(ManagedVocalPlaybackBufferResult result)
{
	std::lock_guard lock(m_mutex);
	if (m_complete) return;
	m_result = std::move(result);
	m_complete = true;
}

ManagedVocalPlaybackCache::ManagedVocalPlaybackCache(quint64 maximumResidentBytes)
	: m_state(std::make_shared<State>())
{
	m_state->maximumResidentBytes = maximumResidentBytes;
	m_loaderPool.setMaxThreadCount(1);
	m_loaderPool.setExpiryTimeout(-1);
}

ManagedVocalPlaybackCache::~ManagedVocalPlaybackCache()
{
	m_loaderPool.waitForDone();
}

ManagedVocalPlaybackCache& ManagedVocalPlaybackCache::instance()
{
	static ManagedVocalPlaybackCache cache;
	return cache;
}

ManagedVocalPlaybackBufferResult ManagedVocalPlaybackCache::acquire(
	const ManagedVocalAssetStore& store,
	const ManagedVocalVariantReference& reference)
{
	return acquireFromState(m_state, store, reference);
}

ManagedVocalPlaybackBufferResult ManagedVocalPlaybackCache::acquireFromState(
	const std::shared_ptr<State>& state,
	const ManagedVocalAssetStore& store,
	const ManagedVocalVariantReference& reference)
{
	if (!validReference(reference))
	{
		return {QStringLiteral("invalid_parameter"), {}};
	}
	const auto byteCount = reference.frames * quint64(reference.channels * int(sizeof(float)));
	if (!state->maximumResidentBytes || byteCount > state->maximumResidentBytes)
	{
		return {QStringLiteral("resource_limit"), {}};
	}
	const auto key = cacheKey(reference);
	std::shared_ptr<State::Reservation> reservation;
	{
		std::unique_lock lock(state->mutex);
		for (;;)
		{
			if (const auto found = state->buffers.find(key); found != state->buffers.end())
			{
				if (auto existing = found.value().lock())
				{
					return {QStringLiteral("ok"), std::move(existing)};
				}
				state->buffers.erase(found);
			}
			if (state->loading.contains(key))
			{
				state->condition.wait(lock);
				continue;
			}
			if (state->usedBytes > state->maximumResidentBytes - byteCount)
			{
				return {QStringLiteral("resource_limit"), {}};
			}
			try
			{
				state->loading.insert(key);
			}
			catch (const std::bad_alloc&)
			{
				return {QStringLiteral("resource_limit"), {}};
			}
			state->usedBytes += byteCount;
			try
			{
				reservation = std::make_shared<State::Reservation>();
			}
			catch (const std::bad_alloc&)
			{
				state->usedBytes -= byteCount;
				state->loading.remove(key);
				state->condition.notify_all();
				return {QStringLiteral("resource_limit"), {}};
			}
			reservation->state = std::weak_ptr<State>(state);
			reservation->bytes = byteCount;
			break;
		}
	}

	auto opened = store.openVerified(reference);
	if (opened.code != QStringLiteral("ok"))
	{
		reservation.reset();
		std::lock_guard lock(state->mutex);
		state->loading.remove(key);
		state->condition.notify_all();
		return {opened.code, {}};
	}
	const auto sampleCount = reference.frames * quint64(reference.channels);
	if (sampleCount != opened.samples.size())
	{
		reservation.reset();
		std::lock_guard lock(state->mutex);
		state->loading.remove(key);
		state->condition.notify_all();
		return {QStringLiteral("asset_integrity_failed"), {}};
	}

	std::shared_ptr<const std::vector<float>> sharedSamples;
	try
	{
		auto* samples = new std::vector<float>(std::move(opened.samples));
		sharedSamples = std::shared_ptr<const std::vector<float>>(samples,
			[reservation](const std::vector<float>* owned) {
				delete owned;
				reservation->release();
			});
	}
	catch (const std::bad_alloc&)
	{
		sharedSamples.reset();
		reservation.reset();
		std::lock_guard lock(state->mutex);
		state->loading.remove(key);
		state->condition.notify_all();
		return {QStringLiteral("resource_limit"), {}};
	}
	bool cached = false;
	{
		std::lock_guard lock(state->mutex);
		try
		{
			state->buffers.insert(key, sharedSamples);
			cached = true;
		}
		catch (const std::bad_alloc&)
		{
		}
		state->loading.remove(key);
		state->condition.notify_all();
	}
	if (!cached)
	{
		sharedSamples.reset();
		reservation.reset();
		return {QStringLiteral("resource_limit"), {}};
	}
	return {QStringLiteral("ok"), std::move(sharedSamples)};
}

ManagedVocalPlaybackLoadRequest ManagedVocalPlaybackCache::requestLoad(
	const ManagedVocalAssetStore& store,
	const ManagedVocalVariantReference& reference)
{
	if (!validReference(reference))
	{
		return {QStringLiteral("invalid_parameter"), {}};
	}
	const auto byteCount = reference.frames * quint64(reference.channels * int(sizeof(float)));
	if (!m_state->maximumResidentBytes || byteCount > m_state->maximumResidentBytes)
	{
		return {QStringLiteral("resource_limit"), {}};
	}
	std::shared_ptr<ManagedVocalPlaybackLoadTicket> ticket;
	try
	{
		ticket = std::make_shared<ManagedVocalPlaybackLoadTicket>();
	}
	catch (const std::bad_alloc&)
	{
		return {QStringLiteral("resource_limit"), {}};
	}
	const auto key = cacheKey(reference);
	{
		std::lock_guard lock(m_state->mutex);
		if (const auto found = m_state->buffers.find(key); found != m_state->buffers.end())
		{
			if (auto existing = found.value().lock())
			{
				ticket->finish({QStringLiteral("ok"), std::move(existing)});
				return {QStringLiteral("ok"), std::move(ticket)};
			}
			m_state->buffers.erase(found);
		}
		if (m_state->outstandingLoads >= m_state->maximumOutstandingLoads)
		{
			return {QStringLiteral("job_busy"), {}};
		}
		++m_state->outstandingLoads;
	}
	const auto state = m_state;
	try
	{
		m_loaderPool.start(QRunnable::create([state, store, reference, ticket] {
			ManagedVocalPlaybackBufferResult loaded;
			try
			{
				loaded = ManagedVocalPlaybackCache::acquireFromState(state, store, reference);
			}
			catch (const std::bad_alloc&)
			{
				loaded = {QStringLiteral("resource_limit"), {}};
			}
			catch (...)
			{
				loaded = {QStringLiteral("asset_integrity_failed"), {}};
			}
			ticket->finish(std::move(loaded));
			std::lock_guard lock(state->mutex);
			if (state->outstandingLoads) --state->outstandingLoads;
			state->condition.notify_all();
		}));
	}
	catch (const std::bad_alloc&)
	{
		{
			std::lock_guard lock(m_state->mutex);
			if (m_state->outstandingLoads) --m_state->outstandingLoads;
			m_state->condition.notify_all();
		}
		ticket->finish({QStringLiteral("resource_limit"), {}});
		return {QStringLiteral("resource_limit"), std::move(ticket)};
	}
	return {QStringLiteral("ok"), std::move(ticket)};
}

quint64 ManagedVocalPlaybackCache::usedBytes() const
{
	std::lock_guard lock(m_state->mutex);
	return m_state->usedBytes;
}

} // namespace lmms
