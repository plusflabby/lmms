#ifndef LMMS_MANAGED_VOCAL_PLAY_HANDLE_H
#define LMMS_MANAGED_VOCAL_PLAY_HANDLE_H

#include "PlayHandle.h"
#include "src/core/vocal/ManagedVocalPlayback.h"
#include <memory>

namespace lmms {

class SampleTrack;

class LMMS_EXPORT ManagedVocalPlayHandle final : public PlayHandle
{
public:
	ManagedVocalPlayHandle(
		SampleTrack* track,
		std::shared_ptr<const ManagedVocalPlaybackView> view,
		qint64 elapsedOutputFrames = 0,
		qint64 maximumOutputFrames = -1);

	bool affinityMatters() const override { return true; }
	void play(SampleFrame* buffer) override;
	bool isFinished() const override;
	bool isFromTrack(const Track* track) const override;
	qint64 totalFrames() const;
	qint64 framesDone() const { return m_cursorFrame - m_startFrame; }

private:
	std::shared_ptr<const ManagedVocalPlaybackView> m_view;
	SampleTrack* m_track = nullptr;
	qint64 m_startFrame = 0;
	qint64 m_cursorFrame = 0;
	qint64 m_endFrame = 0;
	bool m_firstPeriod = true;
};

} // namespace lmms

#endif
