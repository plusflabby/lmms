#ifndef LMMS_AIBRIDGE_METER_SNAPSHOT_H
#define LMMS_AIBRIDGE_METER_SNAPSHOT_H

#include <atomic>

namespace lmms
{
struct AiBridgeMeterSnapshot
{
	float leftPeak = 0.0f;
	float rightPeak = 0.0f;
};

class AiBridgeMeterPublisher
{
public:
	void publish(float left, float right) noexcept;
	AiBridgeMeterSnapshot read() const noexcept;

private:
	std::atomic<float> m_left{0.0f};
	std::atomic<float> m_right{0.0f};
};
}

#endif
