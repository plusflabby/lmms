#include "RecordingTakePreview.h"

#include "AudioEngine.h"
#include "AudioImportPolicy.h"
#include "Sample.h"
#include "SamplePlayHandle.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

namespace lmms::recording
{
namespace
{
constexpr std::size_t WaveformBins = 2048;
}

struct RecordingTakePreview::Impl
{
	explicit Impl(AudioEngine& engine) : audio(engine), owner(QThread::currentThread()) {}
	AudioEngine& audio;
	QThread* owner;
	SamplePlayHandle* handle = nullptr;
	std::shared_ptr<const SampleBuffer> buffer;
	std::shared_ptr<SealedTakeToken> source;
	RecordingWaveformSummary summary;
};

RecordingTakePreview::RecordingTakePreview(AudioEngine& engine, QObject* parent)
	: QObject(parent), m_impl(std::make_unique<Impl>(engine))
{
	Q_ASSERT(!parent || parent->thread() == QThread::currentThread());
}

RecordingTakePreview::~RecordingTakePreview()
{
	Q_ASSERT(QThread::currentThread() == m_impl->owner);
	std::ignore = stop();
}

QString RecordingTakePreview::play(SealedTakeToken&& source, const PlacementPlan& plan)
{
	if (QThread::currentThread() != m_impl->owner) return QStringLiteral("wrong_thread");
	if (active())
	{
		const auto stopped = stop();
		if (stopped != QStringLiteral("stopped")) return stopped;
	}
	const auto facts = source.facts();
	if (!source.isCurrent() || !facts.rawFrames || facts.rawFrames > std::uint64_t(facts.sampleRate) * 120
		|| facts.rawFrames > std::uint64_t(std::numeric_limits<int>::max())
		|| plan.rawEndFrame != facts.rawFrames || plan.effectiveSourceInFrame >= facts.rawFrames
		|| plan.offsetTicks > 0 || plan.offsetTicks == std::numeric_limits<qint64>::min()
		|| plan.positionTicks < 0 || plan.lengthTicks <= 0 || plan.sampleAccuracyVerified
		|| plan.placementQuality != QStringLiteral("uncalibrated") || plan.driftQuality != QStringLiteral("unmeasured"))
		return QStringLiteral("invalid_placement");

	auto decoded = AudioImportPolicy::configured().prepareRecordedTake(std::move(source));
	if (!decoded.audio) return decoded.code;
	if (!decoded.audio->isCurrent() || !decoded.audio->m_buffer
		|| decoded.audio->m_info.frames != facts.rawFrames || decoded.audio->m_info.sampleRate != int(facts.sampleRate))
		return QStringLiteral("recorded_source_changed");
	const auto buffer = decoded.audio->m_buffer;
	if (!buffer || buffer->size() != facts.rawFrames) return QStringLiteral("recorded_source_changed");

	std::unique_ptr<Sample> sample;
	try
	{
		sample = std::make_unique<Sample>(buffer);
	}
	catch (const std::bad_alloc&)
	{
		return QStringLiteral("source_too_large");
	}
	sample->setStartFrame(int(plan.effectiveSourceInFrame));
	sample->setEndFrame(int(facts.rawFrames));
	if (sample->startFrame() >= sample->endFrame()) return QStringLiteral("invalid_placement");

	RecordingWaveformSummary summary;
	summary.frameCount = facts.rawFrames;
	summary.sourceInFrame = plan.effectiveSourceInFrame;
	summary.sampleRate = facts.sampleRate;
	const auto bins = std::min<std::uint64_t>(WaveformBins, facts.rawFrames);
	try
	{
		summary.peakEnvelope.reserve(std::size_t(bins));
		const auto* frames = buffer->data();
		for (std::uint64_t bin = 0; bin < bins; ++bin)
		{
			const auto begin = facts.rawFrames * bin / bins;
			const auto end = std::max(begin + 1, facts.rawFrames * (bin + 1) / bins);
			float peak = 0.0f;
			for (auto frame = begin; frame < end; ++frame)
			{
				peak = std::max(peak, float(std::abs(frames[frame].left())));
				peak = std::max(peak, float(std::abs(frames[frame].right())));
			}
			summary.peakEnvelope.push_back(peak);
		}
	}
	catch (const std::bad_alloc&)
	{
		return QStringLiteral("waveform_resource_limit");
	}

	auto* handle = new SamplePlayHandle(sample.release(), true);
	if (!m_impl->audio.addPlayHandle(handle)) return QStringLiteral("preview_unavailable");
	m_impl->handle = handle;
	m_impl->buffer = buffer;
	m_impl->source = decoded.audio->m_recordedTake;
	m_impl->summary = std::move(summary);
	if (!m_impl->source || !m_impl->source->isCurrent())
	{
		std::ignore = stop();
		return QStringLiteral("recorded_source_changed");
	}
	return QStringLiteral("ok");
}

QString RecordingTakePreview::stop()
{
	if (QThread::currentThread() != m_impl->owner) return QStringLiteral("wrong_thread");
	if (!m_impl->handle) return QStringLiteral("already_stopped");
	auto* handle = std::exchange(m_impl->handle, nullptr);
	m_impl->audio.removePlayHandle(handle);
	m_impl->source.reset();
	m_impl->buffer.reset();
	m_impl->summary = {};
	return QStringLiteral("stopped");
}

bool RecordingTakePreview::active() const noexcept { return m_impl->handle != nullptr; }
QThread* RecordingTakePreview::ownerThread() const noexcept { return m_impl->owner; }
const RecordingWaveformSummary& RecordingTakePreview::waveform() const noexcept { return m_impl->summary; }
}
