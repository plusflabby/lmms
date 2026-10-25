#ifndef LMMS_RECORDING_QUALIFICATION_VIEW_H
#define LMMS_RECORDING_QUALIFICATION_VIEW_H
#include "RecordingServiceHost.h"
#include <QElapsedTimer>
#include <QWidget>
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QPlainTextEdit;
class QTimer;
namespace lmms::recording {
class RecordingQualificationView final : public QWidget
{
 Q_OBJECT
public:
 RecordingQualificationView(RecordingServiceHost&,RecordingProjectIdentity&,QWidget* parent=nullptr);
 ~RecordingQualificationView() override;
 void showInQualificationWindow();
 void refreshInputs();
 int selectedInputIndex() const;
 int selectedChannelIndex() const;
 int selectedSampleRate() const;
private:
 void updateControls();
 void chooseRecordingRoot();
 void armLocally();
 void startTake();
 void stopTake();
 void disarmLocally();
 void pollHost();
 RecordingServiceHost& m_host;
 RecordingProjectIdentity& m_projectIdentity;
 QComboBox* m_inputDevices=nullptr;
 QComboBox* m_channels=nullptr;
 QComboBox* m_sampleRates=nullptr;
 QLineEdit* m_takeLabel=nullptr;
 QLabel* m_rootLabel=nullptr;
 QLabel* m_statusLabel=nullptr;
 QLabel* m_meterLabel=nullptr;
 QPlainTextEdit* m_diagnostics=nullptr;
 QPushButton* m_enumerateButton=nullptr;
 QPushButton* m_rootButton=nullptr;
 QPushButton* m_armButton=nullptr;
 QPushButton* m_startButton=nullptr;
 QPushButton* m_stopButton=nullptr;
 QPushButton* m_disarmButton=nullptr;
 std::vector<RecordingInputDevice> m_devices;
 ProjectIdentity m_project;
 InputEpoch m_epoch;
 std::uint64_t m_sessionRevision=0;
 bool m_localArm=false;
 QTimer* m_pollTimer=nullptr;
 QElapsedTimer m_clock;
};
}
#endif
