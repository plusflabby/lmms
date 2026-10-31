#include "RecordingRecorderView.h"

#include "Engine.h"
#include "RecordingRing.h"
#include "RecordingTakePreview.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPaintEvent>
#include <QMdiSubWindow>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <limits>

namespace lmms::recording
{
namespace
{
class RecordingWaveformWidget final : public QWidget
{
public:
	explicit RecordingWaveformWidget(QWidget* parent) : QWidget(parent)
	{
		setObjectName("recordingWaveform");
		setMinimumHeight(72);
		setProperty("frame_count", qulonglong(0));
		setProperty("source_in_frame", qulonglong(0));
		setProperty("source_in_fraction", 0.0);
		setProperty("envelope_bins", 0);
	}

	void setSummary(const RecordingWaveformSummary& summary)
	{
		m_peaks = summary.peakEnvelope;
		m_frameCount = summary.frameCount;
		m_sourceInFrame = summary.sourceInFrame;
		setProperty("frame_count", qulonglong(m_frameCount));
		setProperty("source_in_frame", qulonglong(m_sourceInFrame));
		setProperty("source_in_fraction", m_frameCount ? double(m_sourceInFrame) / double(m_frameCount) : 0.0);
		setProperty("envelope_bins", int(m_peaks.size()));
		update();
	}

protected:
	void paintEvent(QPaintEvent*) override
	{
		QPainter painter(this);
		painter.fillRect(rect(), palette().base());
		const auto center = height() / 2;
		painter.setPen(palette().mid().color());
		painter.drawLine(0, center, width(), center);
		if (m_peaks.empty() || !m_frameCount)
		{
			painter.setPen(palette().text().color());
			painter.drawText(rect(), Qt::AlignCenter, tr("Select a sealed take to review its raw waveform and source-in."));
			return;
		}
		const auto marker = std::clamp(int(double(width()) * double(m_sourceInFrame) / double(m_frameCount)), 0, width());
		painter.fillRect(QRect(0, 0, marker, height()), QColor(230, 120, 80, 36));
		painter.setPen(QPen(palette().text().color(), 1));
		for (std::size_t i = 0; i < m_peaks.size(); ++i)
		{
			const auto x = int(i * std::size_t(width()) / m_peaks.size());
			const auto amplitude = std::clamp(m_peaks[i], 0.0f, 1.0f);
			const auto halfHeight = int(amplitude * float(std::max(1, center - 2)));
			painter.drawLine(x, center - halfHeight, x, center + halfHeight);
		}
		painter.setPen(QPen(QColor(230, 90, 55), 2));
		painter.drawLine(marker, 0, marker, height());
	}

private:
	std::vector<float> m_peaks;
	std::uint64_t m_frameCount = 0;
	std::uint64_t m_sourceInFrame = 0;
};

QString phaseText(Phase phase)
{
	switch (phase)
	{
	case Phase::Disarmed: return QObject::tr("Disarmed");
	case Phase::Armed: return QObject::tr("Armed locally; input remains closed until Start");
	case Phase::Prepared: return QObject::tr("Take prepared; waiting for explicit Start");
	case Phase::Priming: return QObject::tr("Starting selected input");
	case Phase::Preroll: return QObject::tr("Musical pre-roll");
	case Phase::Recording: return QObject::tr("Recording");
	case Phase::Quiescing: return QObject::tr("Stopping input and finalizing take");
	case Phase::Finalizing: return QObject::tr("Finalizing WAV and journal");
	}
	return QObject::tr("Unavailable");
}

bool activePhase(Phase phase)
{
	return phase == Phase::Priming || phase == Phase::Preroll || phase == Phase::Recording
		|| phase == Phase::Quiescing || phase == Phase::Finalizing;
}

QString qualityText(const RecordingStatus& status)
{
	return QObject::tr("Position uncalibrated · drift unmeasured · timing %1")
		.arg(status.inputTimestampQuality == InputTimestampQuality::Unavailable
			? QObject::tr("unavailable")
			: status.inputTimestampQuality == InputTimestampQuality::Discontinuous
				? QObject::tr("discontinuous") : QObject::tr("observed, not calibrated"));
}
}

RecordingRecorderView::RecordingRecorderView(RecordingServiceHost& host, RecordingSessionManager& manager,
	RecordingProjectIdentity& projectIdentity, QWidget* parent)
	: QWidget(parent), m_host(host), m_manager(manager), m_projectIdentity(projectIdentity),
	m_project(host.projectIdentity())
{
	m_preview = new RecordingTakePreview(*Engine::audioEngine(), this);
	setWindowTitle(tr("Vocal Recorder"));
	setObjectName("vocalRecorderView");
	auto* layout = new QFormLayout(this);

	m_rootLabel = new QLabel(this);
	m_rootLabel->setObjectName("recordingRootStatus");
	auto* rootButton = new QPushButton(tr("Choose recording folder…"), this);
	rootButton->setObjectName("chooseRecordingRoot");
	m_inputDevices = new QComboBox(this);
	m_inputDevices->setObjectName("recordingInputDevice");
	m_inputDevices->setCurrentIndex(-1);
	m_channels = new QComboBox(this);
	m_channels->setObjectName("recordingInputChannel");
	m_channels->setCurrentIndex(-1);
	m_sampleRates = new QComboBox(this);
	m_sampleRates->setObjectName("recordingInputRate");
	m_sampleRates->addItem(tr("44.1 kHz"), 44100);
	m_sampleRates->addItem(tr("48 kHz"), 48000);
	m_sampleRates->setCurrentIndex(-1);
	m_enumerateButton = new QPushButton(tr("Select microphone…"), this);
	m_enumerateButton->setObjectName("enumerateRecordingInputs");
	m_armButton = new QPushButton(tr("Arm locally"), this);
	m_armButton->setObjectName("armRecordingInput");
	m_countIn = new QComboBox(this);
	m_countIn->setObjectName("recordingCountInBars");
	for (int bars = 0; bars <= 2; ++bars)
	{
		m_countIn->addItem(tr("%1 bars").arg(bars), bars);
	}
	m_metronome = new QCheckBox(tr("Metronome"), this);
	m_metronome->setObjectName("recordingMetronome");
	m_takeLabel = new QLineEdit(tr("Take 1"), this);
	m_takeLabel->setObjectName("recordingTakeLabel");
	m_takeLabel->setMaxLength(96);
	m_startTick = new QLineEdit(QStringLiteral("0"), this);
	m_startTick->setObjectName("recordingStartTick");
	m_prepareButton = new QPushButton(tr("Prepare take"), this);
	m_prepareButton->setObjectName("prepareRecordingTake");
	m_startButton = new QPushButton(tr("Start take"), this);
	m_startButton->setObjectName("startRecordingTake");
	m_stopButton = new QPushButton(tr("Stop take"), this);
	m_stopButton->setObjectName("stopRecordingTake");
	m_disarmButton = new QPushButton(tr("Disarm"), this);
	m_disarmButton->setObjectName("disarmRecordingInput");
	m_statusLabel = new QLabel(this);
	m_statusLabel->setObjectName("recordingInputStatus");
	m_captureLabel = new QLabel(tr("No capture"), this);
	m_captureLabel->setObjectName("recordingCaptureCounters");
	m_recentTakes = new QListWidget(this);
	m_recentTakes->setObjectName("recordingRecentTakes");
	m_recentTakes->setMaximumHeight(110);
	m_sourceInTicks = new QLineEdit(QStringLiteral("0"), this);
	m_sourceInTicks->setObjectName("recordingSourceInTicks");
	m_targetTick = new QLineEdit(QStringLiteral("0"), this);
	m_targetTick->setObjectName("recordingTargetTick");
	m_placementLabel = new QLabel(tr("Placement is manual and uncalibrated. Preview the source-in before confirming."), this);
	m_placementLabel->setObjectName("recordingPlacementStatus");
	m_waveformView = new RecordingWaveformWidget(this);
	m_previewButton = new QPushButton(tr("Preview crop"), this);
	m_previewButton->setObjectName("previewRecordedTake");
	m_stopPreviewButton = new QPushButton(tr("Stop preview"), this);
	m_stopPreviewButton->setObjectName("stopTakePreview");
	m_confirmButton = new QPushButton(tr("Confirm placement"), this);
	m_confirmButton->setObjectName("confirmRecordedTake");
	m_diagnostics = new QPlainTextEdit(this);
	m_diagnostics->setObjectName("recordingStatusDetails");
	m_diagnostics->setReadOnly(true);
	m_diagnostics->setMaximumHeight(90);

	layout->addRow(tr("Recording library"), m_rootLabel);
	layout->addRow(rootButton);
	layout->addRow(m_enumerateButton, m_inputDevices);
	layout->addRow(tr("Input channel"), m_channels);
	layout->addRow(tr("Capture rate"), m_sampleRates);
	layout->addRow(m_armButton);
	layout->addRow(tr("Take label"), m_takeLabel);
	layout->addRow(tr("Musical pre-roll"), m_countIn);
	layout->addRow(m_metronome);
	layout->addRow(tr("Target tick"), m_startTick);
	layout->addRow(m_prepareButton, m_startButton);
	layout->addRow(m_stopButton, m_disarmButton);
	layout->addRow(tr("Status"), m_statusLabel);
	layout->addRow(tr("Capture"), m_captureLabel);
	layout->addRow(tr("Recent takes"), m_recentTakes);
	layout->addRow(tr("Source-in ticks"), m_sourceInTicks);
	layout->addRow(tr("Timeline target tick"), m_targetTick);
	layout->addRow(m_waveformView);
	layout->addRow(m_placementLabel);
	layout->addRow(m_previewButton, m_stopPreviewButton);
	layout->addRow(m_confirmButton);
	layout->addRow(m_diagnostics);

	connect(rootButton, &QPushButton::clicked, this, &RecordingRecorderView::chooseRecordingRoot);
	connect(m_enumerateButton, &QPushButton::clicked, this, &RecordingRecorderView::enumerateInputs);
	connect(m_inputDevices, qOverload<int>(&QComboBox::currentIndexChanged), this, &RecordingRecorderView::selectInput);
	connect(m_channels, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { updateControls(); });
	connect(m_sampleRates, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { updateControls(); });
	connect(m_armButton, &QPushButton::clicked, this, &RecordingRecorderView::armLocally);
	connect(m_prepareButton, &QPushButton::clicked, this, &RecordingRecorderView::prepareTake);
	connect(m_startButton, &QPushButton::clicked, this, &RecordingRecorderView::startTake);
	connect(m_stopButton, &QPushButton::clicked, this, &RecordingRecorderView::stopTake);
	connect(m_disarmButton, &QPushButton::clicked, this, &RecordingRecorderView::disarmLocally);
	connect(m_recentTakes, &QListWidget::currentRowChanged, this, [this] {
		clearReviewPreview();
		m_placementLabel->setText(tr("Placement is manual and uncalibrated. Preview the source-in before confirming."));
		updateControls();
	});
	connect(m_previewButton, &QPushButton::clicked, this, &RecordingRecorderView::reviewSelectedTake);
	connect(m_stopPreviewButton, &QPushButton::clicked, this, &RecordingRecorderView::stopPreviewPlayback);
	connect(m_confirmButton, &QPushButton::clicked, this, &RecordingRecorderView::confirmPlacement);
	const auto invalidateReview = [this] {
		if (m_placementRevision != std::numeric_limits<quint64>::max()) ++m_placementRevision;
		clearReviewPreview();
		m_placementLabel->setText(tr("Placement changed. Preview the source-in before confirming."));
		updateControls();
	};
	connect(m_sourceInTicks, &QLineEdit::editingFinished, this, invalidateReview);
	connect(m_targetTick, &QLineEdit::editingFinished, this, invalidateReview);
	m_refreshTimer = new QTimer(this);
	m_refreshTimer->setInterval(100);
	connect(m_refreshTimer, &QTimer::timeout, this, &RecordingRecorderView::refreshView);
	m_refreshTimer->start();
	refreshView();
}

RecordingRecorderView::~RecordingRecorderView()
{
	if (m_refreshTimer) m_refreshTimer->stop();
	stopPreview();
}

void RecordingRecorderView::showInRecorderWindow()
{
	if (auto* window = qobject_cast<QMdiSubWindow*>(parentWidget()))
	{
		window->show();
		show();
		window->raise();
		setFocus(Qt::OtherFocusReason);
		return;
	}
	show();
	raise();
	setFocus(Qt::OtherFocusReason);
}

void RecordingRecorderView::chooseRecordingRoot()
{
	if (!m_manager.quiescent())
	{
		std::ignore = m_manager.disarm();
		if (!m_manager.quiescent())
		{
			m_statusLabel->setText(tr("Stop and finalize the current take before changing its recording folder."));
			return;
		}
	}
	const auto folder = QFileDialog::getExistingDirectory(this, tr("Choose recording folder"));
	if (folder.isEmpty()) return;
	const auto selected = m_host.selectRecordingRoot(folder, m_projectIdentity);
	if (!selected.ok)
	{
		m_statusLabel->setText(selected.code);
		return;
	}
	m_project = selected.project;
	m_rootLabel->setText(folder);
	m_statusLabel->setText(tr("Recording folder authorized for this project"));
	updateControls();
}

void RecordingRecorderView::enumerateInputs()
{
	if (activePhase(m_manager.status().phase)) return;
	if (m_manager.status().armed)
	{
		std::ignore = m_manager.disarm();
		if (!m_manager.quiescent()) return;
	}
	m_inputDevices->clear();
	m_inputDevices->setCurrentIndex(-1);
	m_channels->clear();
	m_channels->setCurrentIndex(-1);
	m_devices.clear();
	const auto page = m_host.enumerateInputs(0, 64);
	m_diagnostics->setPlainText(page.diagnostic.localSummary());
	if (page.code != QStringLiteral("ok"))
	{
		m_statusLabel->setText(page.code);
		updateControls();
		return;
	}
	for (const auto& device : page.devices)
	{
		if (device.maxInputChannels == 0 || device.maxInputChannels > 32
			|| device.hostApiName.compare(QStringLiteral("WASAPI"), Qt::CaseInsensitive) != 0) continue;
		m_devices.push_back(device);
		m_inputDevices->addItem(device.name + QStringLiteral(" — ") + device.hostApiName, device.id);
	}
	m_inputDevices->setCurrentIndex(-1);
	m_statusLabel->setText(m_devices.empty() ? tr("No supported WASAPI input was listed")
		: tr("Choose an input and channel explicitly"));
	updateControls();
}

void RecordingRecorderView::selectInput(int index)
{
	if (activePhase(m_manager.status().phase)) return;
	if (m_manager.status().armed) std::ignore = m_manager.disarm();
	m_channels->clear();
	m_channels->setCurrentIndex(-1);
	if (index >= 0 && std::size_t(index) < m_devices.size())
	{
		const auto channels = std::min<std::uint32_t>(m_devices[std::size_t(index)].maxInputChannels, 32);
		for (std::uint32_t channel = 0; channel < channels; ++channel)
			m_channels->addItem(tr("Input channel %1").arg(channel + 1), int(channel));
	}
	m_channels->setCurrentIndex(-1);
	updateControls();
}

void RecordingRecorderView::armLocally()
{
	const auto deviceIndex = m_inputDevices->currentIndex();
	const auto channelIndex = m_channels->currentIndex();
	const auto rate = m_sampleRates->currentIndex() < 0 ? 0 : m_sampleRates->currentData().toUInt();
	if (m_project.id.isEmpty() || m_projectIdentity.cachedIdentity().id != m_project.id
		|| !m_host.rootRevision())
	{
		m_statusLabel->setText(tr("Choose a recording folder for the current project first."));
		return;
	}
	if (deviceIndex < 0 || channelIndex < 0 || !rate || std::size_t(deviceIndex) >= m_devices.size()) return;
	const auto generation = m_manager.status().projectGeneration;
	const auto& device = m_devices[std::size_t(deviceIndex)];
	SelectedInputProfile profile;
	profile.deviceId = device.id;
	profile.hostApiName = device.hostApiName;
	profile.wasapiShared = true;
	profile.input = {rate, device.maxInputChannels, std::uint32_t(m_channels->currentData().toInt()),
		RecordingRing::MaximumCallback, std::uint64_t(rate) * 120};
	auto ticket = m_host.localArmTicket(profile, generation);
	if (!ticket.ok)
	{
		m_statusLabel->setText(ticket.code);
		updateControls();
		return;
	}
	const auto armed = m_manager.arm(std::move(ticket.ticket));
	if (armed.ok) m_hadLocalArm = true;
	m_statusLabel->setText(armed.ok ? tr("Locally armed. Input remains closed until Start.") : armed.code);
	updateControls();
}

void RecordingRecorderView::prepareTake()
{
	bool tickOk = false;
	const auto tick = m_startTick->text().toLongLong(&tickOk);
	if (!tickOk || tick < 0 || tick > std::numeric_limits<int>::max())
	{
		m_statusLabel->setText(tr("Enter a valid target tick."));
		return;
	}
	const auto status = m_manager.status();
	PrepareTakeRequest request;
	request.startTick = tick;
	request.takeLabel = m_takeLabel->text();
	request.countInBars = m_countIn->currentData().toInt();
	request.metronomeEnabled = m_metronome->isChecked();
	request.expectedProjectGeneration = status.projectGeneration;
	const auto prepared = m_manager.prepare(request);
	m_statusLabel->setText(prepared.ok ? tr("Take prepared. Input remains closed until Start.") : prepared.code);
	updateControls();
}

void RecordingRecorderView::startTake()
{
	const auto status = m_manager.status();
	if (status.preparedId.isEmpty()) return;
	const StartTakeRequest request{status.preparedId, status.projectGeneration,
		QUuid::createUuid().toString(QUuid::WithoutBraces)};
	const auto started = m_manager.start(request);
	m_statusLabel->setText(started.ok ? tr("Start accepted. Opening the selected input.") : started.code);
	updateControls();
}

void RecordingRecorderView::stopTake()
{
	const auto takeId = m_manager.status().takeId;
	if (takeId.isEmpty()) return;
	const auto stopped = m_manager.stop(takeId);
	m_statusLabel->setText(stopped.code);
	updateControls();
}

void RecordingRecorderView::disarmLocally()
{
	const auto disarmed = m_manager.disarm();
	m_statusLabel->setText(disarmed.code);
	updateControls();
}

void RecordingRecorderView::confirmPlacement()
{
	if (!m_reviewedApproval || !m_recentTakes->currentItem()
		|| !m_recentTakes->currentItem()->data(Qt::UserRole + 1).toBool() || !m_manager.quiescent())
	{
		m_placementLabel->setText(tr("Preview and review the source-in before confirming placement."));
		return;
	}
	if (!m_recentTakes->currentItem())
	{
		m_reviewedApproval.reset();
		m_placementLabel->setText(tr("Select a completed take and preview its source-in before confirming."));
		updateControls();
		return;
	}
	bool sourceOk = false;
	bool targetOk = false;
	const auto sourceIn = m_sourceInTicks->text().toLongLong(&sourceOk);
	const auto target = m_targetTick->text().toLongLong(&targetOk);
	const auto status = m_manager.status();
	if (!sourceOk || !targetOk || m_reviewedApproval->takeId != m_recentTakes->currentItem()->data(Qt::UserRole).toString()
		|| m_reviewedApproval->expectedProjectGeneration != status.projectGeneration
		|| m_reviewedApproval->placementRevision != m_placementRevision
		|| m_reviewedApproval->sourceInTicks != sourceIn || m_reviewedApproval->targetTick != target)
	{
		clearReviewPreview();
		m_placementLabel->setText(tr("Placement changed. Preview the source-in before confirming."));
		return;
	}
	stopPreviewPlayback();
	const auto reply = m_manager.confirmPlacement(*m_reviewedApproval);
	m_statusLabel->setText(reply.code);
	if (reply.ok)
	{
		m_placementLabel->setText(tr("%1 · position uncalibrated · drift unmeasured")
			.arg(reply.status.placementState == PlacementState::Attached
				? tr("Vocal take attached; original WAV preserved") : reply.code));
	}
	else m_placementLabel->setText(tr("%1 · position remains uncalibrated · drift unmeasured").arg(reply.code));
	updateControls();
}

void RecordingRecorderView::reviewSelectedTake()
{
	if (!m_preview || !m_recentTakes->currentItem()) return;
	if (!m_recentTakes->currentItem()->data(Qt::UserRole + 1).toBool())
	{
		m_placementLabel->setText(m_recentTakes->currentItem()->toolTip());
		return;
	}
	bool sourceOk = false;
	bool targetOk = false;
	const auto sourceIn = m_sourceInTicks->text().toLongLong(&sourceOk);
	const auto target = m_targetTick->text().toLongLong(&targetOk);
	if (!sourceOk || !targetOk || sourceIn < 0 || target < 0
		|| sourceIn > std::numeric_limits<int>::max() || target > std::numeric_limits<int>::max())
	{
		m_placementLabel->setText(tr("Enter valid source-in and target ticks."));
		return;
	}
	const auto status = m_manager.status();
	const auto approval = approve(m_recentTakes->currentItem()->data(Qt::UserRole).toString(),
		status.projectGeneration, m_placementRevision, sourceIn, target);
	auto review = m_manager.reviewPlacement(approval);
	if (!review.plan || !review.source)
	{
		m_reviewedApproval.reset();
		m_placementLabel->setText(tr("Review unavailable: %1. No clip was attached.").arg(review.code));
		updateControls();
		return;
	}
	const auto played = m_preview->play(std::move(*review.source), *review.plan);
	if (played != QStringLiteral("ok"))
	{
		m_reviewedApproval.reset();
		m_placementLabel->setText(tr("Preview unavailable: %1. No clip was attached.").arg(played));
		updateControls();
		return;
	}
	m_reviewedApproval = approval;
	if (auto* waveform = dynamic_cast<RecordingWaveformWidget*>(m_waveformView))
		waveform->setSummary(m_preview->waveform());
	const auto& summary = m_preview->waveform();
	const double excludedMilliseconds = summary.sampleRate
		? (1000.0 * double(summary.sourceInFrame) / double(summary.sampleRate)) : 0.0;
	m_placementLabel->setText(tr("Preview starts at frame %1 (~%2 ms) · count-in remains in the original WAV · placement uncalibrated · drift unmeasured")
		.arg(qulonglong(summary.sourceInFrame)).arg(excludedMilliseconds, 0, 'f', 1));
	updateControls();
}

void RecordingRecorderView::stopPreview()
{
	clearReviewPreview();
}

void RecordingRecorderView::stopPreviewPlayback()
{
	if (m_preview) std::ignore = m_preview->stop();
	updateControls();
}

void RecordingRecorderView::clearReviewPreview()
{
	if (m_preview) std::ignore = m_preview->stop();
	m_reviewedApproval.reset();
	if (m_waveformView)
	{
		if (auto* waveform = dynamic_cast<RecordingWaveformWidget*>(m_waveformView))
			waveform->setSummary({});
	}
	updateControls();
}

void RecordingRecorderView::refreshView()
{
	const auto status = m_manager.status();
	if (m_hadLocalArm && !status.armed && !activePhase(status.phase))
	{
		m_hadLocalArm = false;
		m_devices.clear();
		m_inputDevices->clear();
		m_inputDevices->setCurrentIndex(-1);
		m_channels->clear();
		m_channels->setCurrentIndex(-1);
		m_sampleRates->setCurrentIndex(-1);
		clearReviewPreview();
		m_statusLabel->setText(tr("Input disarmed. Select the microphone and channel again before another take."));
	}
	if (activePhase(status.phase) && m_preview && m_preview->active()) clearReviewPreview();
	const auto currentProject = m_projectIdentity.cachedIdentity();
	const auto authorizedProject = m_host.projectIdentity();
	const bool rootMatches = !m_project.id.isEmpty() && m_project.id == currentProject.id
		&& m_project.id == authorizedProject.id && m_host.rootRevision() != 0;
	if (!rootMatches && (!m_project.id.isEmpty() || !authorizedProject.id.isEmpty()))
	{
		clearReviewPreview();
		m_project = {};
		m_devices.clear();
		m_inputDevices->clear();
		m_inputDevices->setCurrentIndex(-1);
		m_channels->clear();
		m_channels->setCurrentIndex(-1);
		m_sampleRates->setCurrentIndex(-1);
		m_rootLabel->setText(tr("Choose a recording folder for the current project"));
		m_statusLabel->setText(tr("Project changed. Local folder and input selection require review."));
	}
	else if (!rootMatches)
	{
		m_rootLabel->setText(tr("No recording folder selected"));
	}
	else if (m_rootLabel->text().isEmpty())
	{
		m_rootLabel->setText(tr("Authorized for the current project"));
	}
	const auto baseStatus = status.phase == Phase::Preroll
		? tr("Musical pre-roll · %1 ticks to target %2").arg(status.countInTicksRemaining).arg(status.intendedTargetTick)
		: activePhase(status.phase) || status.code.isEmpty() ? phaseText(status.phase) : status.code;
	m_statusLabel->setText(baseStatus + tr(" · input monitoring off · %1").arg(qualityText(status)));
	if (status.captureIntegrity != CaptureIntegrity::Pending || status.takeId.size())
	{
		m_captureLabel->setText(tr("Received %1 · accepted %2 · written %3 · dropped %4 · over limit %5 · clipped %6 · invalid %7 · hardware loss %8 · observations dropped %9 · peak %10")
			.arg(qulonglong(status.receivedFrames)).arg(qulonglong(status.acceptedFrames)).arg(qulonglong(status.writtenFrames))
			.arg(qulonglong(status.applicationDroppedFrames)).arg(qulonglong(status.intentionallyUnrecordedFrames))
			.arg(qulonglong(status.clippedSamples)).arg(qulonglong(status.invalidSamples))
			.arg(status.hardwareLossCountKnown ? tr("known %1").arg(qulonglong(status.hardwareLostFrames)) : tr("unknown"))
			.arg(qulonglong(status.observationsDropped))
			.arg(status.inputPeak, 0, 'f', 3));
	}
	if (!status.code.isEmpty() || !status.captureCode.isEmpty())
		m_diagnostics->setPlainText((status.code + QLatin1Char('\n') + status.captureCode).left(2048));
	populateTakes();
	updateControls();
}

void RecordingRecorderView::updateControls()
{
	const auto status = m_manager.status();
	const bool active = activePhase(status.phase);
	const auto currentProject = m_projectIdentity.cachedIdentity();
	const auto authorizedProject = m_host.projectIdentity();
	const bool rooted = !m_project.id.isEmpty() && m_project.id == currentProject.id
		&& m_project.id == authorizedProject.id && m_host.rootRevision() != 0;
	const bool selected = m_inputDevices->currentIndex() >= 0 && m_channels->currentIndex() >= 0
		&& m_sampleRates->currentIndex() >= 0;
	m_enumerateButton->setEnabled(!active);
	m_inputDevices->setEnabled(!active && !status.armed);
	m_channels->setEnabled(!active && !status.armed);
	m_sampleRates->setEnabled(!active && !status.armed);
	m_armButton->setEnabled(rooted && selected && !active && !status.armed);
	m_prepareButton->setEnabled(rooted && !active);
	m_startButton->setEnabled(status.phase == Phase::Prepared && status.armed && !active);
	m_stopButton->setEnabled(active);
	m_disarmButton->setEnabled(status.armed || active);
	const auto* selectedTake = m_recentTakes->currentItem();
	const bool canReviewTake = selectedTake && selectedTake->data(Qt::UserRole + 1).toBool();
	m_previewButton->setEnabled(canReviewTake && !active && m_manager.quiescent());
	m_confirmButton->setEnabled(canReviewTake && !active && m_manager.quiescent() && m_reviewedApproval.has_value());
	m_stopPreviewButton->setEnabled(m_preview && m_preview->active());
}

void RecordingRecorderView::populateTakes()
{
	struct Row
	{
		QString key;
		QString takeId;
		QString label;
		QString tooltip;
		bool previewable = false;
	};
	std::vector<Row> rows;
	QSet<QString> seen;
	QSet<QString> seenTakeIds;
	const auto currentGeneration = m_projectIdentity.revision();
	const auto page = m_manager.takes({0, 100});
	for (const auto& take : page.takes)
	{
		const auto key = QStringLiteral("session/") + take.takeId;
		if (seen.contains(key)) continue;
		seen.insert(key);
		seenTakeIds.insert(take.takeId);
		const bool currentProjectTake = take.projectGeneration == currentGeneration;
		const bool reviewable = currentProjectTake && take.captureIntegrity == CaptureIntegrity::Complete
			&& !take.cancelled && take.performanceReached;
		rows.push_back({key, take.takeId,
			currentProjectTake ? (take.takeLabel.isEmpty() ? take.takeId : take.takeLabel)
				: tr("Previous project · %1").arg(take.takeLabel.isEmpty() ? take.takeId : take.takeLabel),
			currentProjectTake ? take.code + QStringLiteral(" · ") + take.placementQuality + QStringLiteral(" · ") + take.driftQuality
				: tr("Take belongs to a previous project. The immutable original remains in its local library."),
			reviewable});
		if (rows.size() == 100) break;
	}
	if (rows.size() < 100)
	{
		if (auto* song = Engine::getSong())
		{
			for (auto* trackBase : song->tracks())
			{
				auto* track = dynamic_cast<SampleTrack*>(trackBase);
				if (!track) continue;
				for (int clipIndex = 0; clipIndex < track->numOfClips() && rows.size() < 100; ++clipIndex)
				{
					auto* clip = dynamic_cast<SampleClip*>(track->getClip(std::size_t(clipIndex)));
					if (!clip || !clip->recordedTakeReference()) continue;
					const auto& reference = *clip->recordedTakeReference();
					const auto key = reference.projectId + QLatin1Char('/') + reference.takeId;
					const bool available = clip->sourceLoadError().isEmpty() && clip->authorizedSourceInfo().has_value();
					if (seenTakeIds.contains(reference.takeId))
					{
						if (!available)
						{
							const auto existing = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
								return row.takeId == reference.takeId;
							});
						if (existing != rows.end())
							{
								existing->label = tr("Vocal source unavailable · %1").arg(reference.takeId);
								existing->tooltip = tr("The project link is retained. Local recording library status: %1")
									.arg(clip->sourceLoadError().isEmpty() ? tr("source unavailable") : clip->sourceLoadError());
								existing->previewable = false;
							}
						}
						continue;
					}
					if (seen.contains(key)) continue;
					seen.insert(key);
					seenTakeIds.insert(reference.takeId);
					rows.push_back({key, reference.takeId,
						available ? tr("Attached vocal · %1").arg(reference.takeId)
							: tr("Vocal source unavailable · %1").arg(reference.takeId),
						available ? tr("This source is attached on the timeline.")
							: tr("The project link is retained. Local recording library status: %1").arg(clip->sourceLoadError()),
						false});
				}
			}
		}
	}
	const auto previousKey = m_recentTakes->currentItem()
		? m_recentTakes->currentItem()->data(Qt::UserRole + 2).toString() : QString{};
	bool identical = rows.size() == std::size_t(m_recentTakes->count());
	for (int i = 0; identical && i < m_recentTakes->count(); ++i)
	{
		const auto* item = m_recentTakes->item(i);
		const auto& row = rows[std::size_t(i)];
		identical = item->data(Qt::UserRole + 2).toString() == row.key
			&& item->text() == row.label && item->toolTip() == row.tooltip
			&& item->data(Qt::UserRole + 1).toBool() == row.previewable;
	}
	if (identical) return;
	m_recentTakes->clear();
	for (const auto& row : rows)
	{
		auto* item = new QListWidgetItem(row.label, m_recentTakes);
		item->setData(Qt::UserRole, row.takeId);
		item->setData(Qt::UserRole + 1, row.previewable);
		item->setData(Qt::UserRole + 2, row.key);
		item->setToolTip(row.tooltip);
	}
	if (!previousKey.isEmpty())
	{
		for (int i = 0; i < m_recentTakes->count(); ++i)
			if (m_recentTakes->item(i)->data(Qt::UserRole + 2).toString() == previousKey) m_recentTakes->setCurrentRow(i);
	}
	else if (m_recentTakes->count()) m_recentTakes->setCurrentRow(0);
}

ManualPlacementApproval RecordingRecorderView::approve(const QString& take, quint64 generation, quint64 revision,
	qint64 sourceIn, qint64 target)
{
	ManualPlacementApproval result;
	result.takeId = take;
	result.expectedProjectGeneration = generation;
	result.placementRevision = revision;
	result.sourceInTicks = sourceIn;
	result.targetTick = target;
	return result;
}
}
