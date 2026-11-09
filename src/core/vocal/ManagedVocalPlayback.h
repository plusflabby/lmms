#ifndef LMMS_MANAGED_VOCAL_PLAYBACK_H
#define LMMS_MANAGED_VOCAL_PLAYBACK_H

#include "SampleFrame.h"
#include "lmms_export.h"
#include <QString>
#include <QtGlobal>
#include <atomic>
#include <memory>
#include <span>
#include <vector>

namespace lmms {

struct ManagedVocalPlaybackGuard
{
	std::atomic<quint64> epoch{1};
};

struct ManagedVocalPlaybackWorld
{
	int tempoBpm = 0;
	int outputRate = 0;
	int ticksPerQuarterNote = 48;
	bool tempoAutomationPresent = false;
	int timeSignatureNumerator = 4;
	int timeSignatureDenominator = 4;
};

struct ManagedVocalPlaybackView
{
	std::shared_ptr<const std::vector<float>> interleavedSamples;
	std::shared_ptr<const ManagedVocalPlaybackGuard> guard;
	quint64 expectedEpoch = 0;
	quint64 frames = 0;
	int sampleRate = 0;
	int channels = 0;
	float amplification = 1.0f;
};

struct ManagedVocalPlaybackPreparation
{
	QString code;
	std::shared_ptr<const ManagedVocalPlaybackView> view;
};

struct ManagedVocalVisualExtent
{
	QString code;
	qint64 visualLengthTicks = 0;
	quint64 nativeCoverageFrames = 0;
	quint64 visualResidualFrames = 0;
	qint64 exactResidualNumerator = 0;
	qint64 exactResidualDenominator = 1;
};

LMMS_EXPORT ManagedVocalVisualExtent managedVocalVisualExtent(
	quint64 mediaFrames,
	int sampleRate,
	int tempoBpm);

LMMS_EXPORT ManagedVocalPlaybackPreparation makeManagedVocalPlaybackView(
	std::vector<float> interleavedSamples,
	int sampleRate,
	int channels,
	const ManagedVocalPlaybackWorld& world,
	bool reversed,
	bool autoResize,
	std::shared_ptr<const ManagedVocalPlaybackGuard> guard,
	quint64 expectedEpoch,
	float amplification = 1.0f);
LMMS_EXPORT ManagedVocalPlaybackPreparation makeManagedVocalPlaybackView(
	std::shared_ptr<const std::vector<float>> interleavedSamples,
	int sampleRate,
	int channels,
	const ManagedVocalPlaybackWorld& world,
	bool reversed,
	bool autoResize,
	std::shared_ptr<const ManagedVocalPlaybackGuard> guard,
	quint64 expectedEpoch,
	float amplification = 1.0f);

// Audio-thread safe: no allocation, locking, file access, or decoding.
LMMS_EXPORT void renderManagedVocalPlayback(
	const ManagedVocalPlaybackView& view,
	qint64 firstFrame,
	std::span<SampleFrame> destination);

} // namespace lmms

#endif
