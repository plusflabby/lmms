#include "AiBridgeMeterSnapshot.h"

namespace lmms
{
void AiBridgeMeterPublisher::publish(float left, float right) noexcept
{
	m_left.store(left, std::memory_order_relaxed);
	m_right.store(right, std::memory_order_relaxed);
}

AiBridgeMeterSnapshot AiBridgeMeterPublisher::read() const noexcept
{
	return {m_left.load(std::memory_order_acquire), m_right.load(std::memory_order_acquire)};
}
}
