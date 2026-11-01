#ifndef LMMS_RECORDING_TAKE_PREVIEW_H
#define LMMS_RECORDING_TAKE_PREVIEW_H

#include "RecordingLibrary.h"
#include "RecordingPlacement.h"
#include <QObject>
#include <QThread>
#include <cstdint>
#include <memory>
#include <vector>

namespace lmms
{
class AudioEngine;
}

namespace lmms::recording
{
struct RecordingWaveformSummary
{
	std::uint64_t frameCount = 0;
	std::uint64_t sourceInFrame = 0;
	std::uint32_t sampleRate = 0;
	std::vector<float> peakEnvelope;
};

class RecordingTakePreview final : public QObject
{
public:
	explicit RecordingTakePreview(AudioEngine&, QObject* parent = nullptr);
	~RecordingTakePreview() override;
	QString play(SealedTakeToken&&, const PlacementPlan&);
	QString stop();
	bool active() const noexcept;
	QThread* ownerThread() const noexcept;
	const RecordingWaveformSummary& waveform() const noexcept;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
}

#endif
