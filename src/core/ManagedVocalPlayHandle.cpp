#include "ManagedVocalPlayHandle.h"

#include "AudioEngine.h"
#include "Engine.h"
#include "SampleTrack.h"
#include <algorithm>
#include <limits>

namespace lmms {

ManagedVocalPlayHandle::ManagedVocalPlayHandle(
	SampleTrack* track,
	std::shared_ptr<const ManagedVocalPlaybackView> view,
	qint64 elapsedOutputFrames,
	qint64 maximumOutputFrames)
	: PlayHandle(PlayHandle::Type::SamplePlayHandle),
	  m_view(std::move(view)),
	  m_track(track)
{
	if (m_track)
	{
		setAudioBusHandle(m_track->audioBusHandle());
	}
	if (!m_view || elapsedOutputFrames < 0 || quint64(elapsedOutputFrames) > m_view->frames)
	{
		m_startFrame = 0;
		m_cursorFrame = 0;
		m_endFrame = 0;
		return;
	}
	m_startFrame = elapsedOutputFrames;
	m_cursorFrame = elapsedOutputFrames;
	m_endFrame = qint64(m_view->frames);
	if (maximumOutputFrames >= 0)
	{
		const auto available = m_endFrame - m_cursorFrame;
		m_endFrame = m_cursorFrame + std::min(available, maximumOutputFrames);
	}
}

void ManagedVocalPlayHandle::play(SampleFrame* buffer)
{
	const auto period = Engine::audioEngine()->framesPerPeriod();
	std::fill_n(buffer, period, SampleFrame{});
	if (!m_view || !m_track || !m_view->guard || !m_view->expectedEpoch
		|| m_view->guard->epoch.load(std::memory_order_acquire) != m_view->expectedEpoch)
	{
		m_cursorFrame = m_endFrame;
		return;
	}
	if (m_cursorFrame >= m_endFrame)
	{
		return;
	}
	const auto firstOffset = m_firstPeriod ? offset() : 0;
	m_firstPeriod = false;
	if (firstOffset >= period)
	{
		m_cursorFrame = m_endFrame;
		return;
	}
	const auto destinationFrames = period - firstOffset;
	const auto availableFrames = quint64(m_endFrame - m_cursorFrame);
	const auto rendered = std::min(destinationFrames, availableFrames);
	if (!m_track->isMuted() && rendered)
	{
		renderManagedVocalPlayback(*m_view, m_cursorFrame,
			std::span<SampleFrame>(buffer + firstOffset, std::size_t(rendered)));
	}
	m_cursorFrame += qint64(rendered);
}

bool ManagedVocalPlayHandle::isFinished() const
{
	return !m_view || !m_view->guard
		|| m_view->guard->epoch.load(std::memory_order_acquire) != m_view->expectedEpoch
		|| m_cursorFrame >= m_endFrame;
}

bool ManagedVocalPlayHandle::isFromTrack(const Track* track) const
{
	return m_track == track;
}

qint64 ManagedVocalPlayHandle::totalFrames() const
{
	return m_endFrame - m_startFrame;
}

} // namespace lmms
