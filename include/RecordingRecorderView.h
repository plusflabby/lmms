#ifndef LMMS_RECORDING_RECORDER_VIEW_H
#define LMMS_RECORDING_RECORDER_VIEW_H

#include "RecordingProjectIdentity.h"
#include "RecordingServiceHost.h"
#include "RecordingSessionManager.h"
#include <QWidget>

class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTimer;

namespace lmms::recording
{
class RecordingTakePreview;

class RecordingRecorderView final : public QWidget
{
	Q_OBJECT
public:
	RecordingRecorderView(RecordingServiceHost&, RecordingSessionManager&, RecordingProjectIdentity&,
		QWidget* parent = nullptr);
	~RecordingRecorderView() override;
	void showInRecorderWindow();
	void refreshView();
	void stopPreview();
	static ManualPlacementApproval approve(const QString& take, quint64 generation, quint64 revision,
		qint64 sourceIn, qint64 target);

private:
	void chooseRecordingRoot();
	void enumerateInputs();
	void selectInput(int);
	void armLocally();
	void prepareTake();
	void startTake();
	void stopTake();
	void disarmLocally();
	void reviewSelectedTake();
	void confirmPlacement();
	void stopPreviewPlayback();
	void clearReviewPreview();
	void updateControls();
	void populateTakes();

	RecordingServiceHost& m_host;
	RecordingSessionManager& m_manager;
	RecordingProjectIdentity& m_projectIdentity;
	ProjectIdentity m_project;
	std::vector<RecordingInputDevice> m_devices;
	RecordingTakePreview* m_preview = nullptr;
	QComboBox* m_inputDevices = nullptr;
	QComboBox* m_channels = nullptr;
	QComboBox* m_sampleRates = nullptr;
	QComboBox* m_countIn = nullptr;
	QCheckBox* m_metronome = nullptr;
	QLineEdit* m_takeLabel = nullptr;
	QLineEdit* m_startTick = nullptr;
	QLineEdit* m_sourceInTicks = nullptr;
	QLineEdit* m_targetTick = nullptr;
	QLabel* m_rootLabel = nullptr;
	QLabel* m_statusLabel = nullptr;
	QLabel* m_captureLabel = nullptr;
	QLabel* m_placementLabel = nullptr;
	QPlainTextEdit* m_diagnostics = nullptr;
	QWidget* m_waveformView = nullptr;
	QListWidget* m_recentTakes = nullptr;
	QPushButton* m_enumerateButton = nullptr;
	QPushButton* m_armButton = nullptr;
	QPushButton* m_prepareButton = nullptr;
	QPushButton* m_startButton = nullptr;
	QPushButton* m_stopButton = nullptr;
	QPushButton* m_disarmButton = nullptr;
	QPushButton* m_previewButton = nullptr;
	QPushButton* m_stopPreviewButton = nullptr;
	QPushButton* m_confirmButton = nullptr;
	QTimer* m_refreshTimer = nullptr;
	quint64 m_placementRevision = 1;
	bool m_hadLocalArm = false;
	std::optional<ManualPlacementApproval> m_reviewedApproval;
};
}

#endif
