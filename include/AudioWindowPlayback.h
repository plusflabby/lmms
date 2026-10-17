#ifndef LMMS_AUDIO_WINDOW_PLAYBACK_H
#define LMMS_AUDIO_WINDOW_PLAYBACK_H
#include "AudioSourceWindow.h"
#include "SampleBuffer.h"
#include <memory>
#include <span>

namespace lmms {
struct AudioWindowPlaybackView
{
	std::shared_ptr<const SampleBuffer> buffer;
	AudioSourceWindowV2 window;
	qint64 firstRelativeOutputFrame=0, outputFrameCount=0;
	double amplification=1;
	std::shared_ptr<const AudioWindowPlaybackGuard> guard;
	quint64 expectedEpoch=0;
};
// Control-thread qualification only. Never probe dependencies in the callback.
LMMS_EXPORT bool verifyAudioWindowReferenceProfile();
LMMS_EXPORT bool audioWindowReferenceProfileMatches(qint32 refillCapacity,const QByteArray& librarySha256);
LMMS_EXPORT std::optional<AudioWindowPlaybackView> makeAudioWindowPlaybackView(
	std::shared_ptr<const SampleBuffer>,const AudioSourceWindowV2&,qint64 elapsedOutputFrames,
	double amplification,std::shared_ptr<const AudioWindowPlaybackGuard>,quint64 epoch);
LMMS_EXPORT void renderAudioWindow(const AudioWindowPlaybackView&,qint64 framesAlreadyRendered,
	std::span<SampleFrame> destination);
}
#endif
