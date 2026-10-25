#include "RecordingQualificationView.h"
#include "Engine.h"
#include "Song.h"
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QUuid>
#include <limits>
namespace lmms::recording {
namespace {
QString stateName(RecordingHostState state)
{
 switch(state){case RecordingHostState::Closed:return QStringLiteral("Closed");case RecordingHostState::Armed:return QStringLiteral("Armed locally");
  case RecordingHostState::WriterStarting:return QStringLiteral("Preparing WAV");case RecordingHostState::OpeningInput:return QStringLiteral("Opening selected input");
  case RecordingHostState::Capturing:return QStringLiteral("Capturing");case RecordingHostState::Stopping:return QStringLiteral("Stopping input");
  case RecordingHostState::Finalizing:return QStringLiteral("Finalizing WAV");case RecordingHostState::FailedRetained:return QStringLiteral("Input retained after error");}
 return QStringLiteral("Unavailable");
}
QString timestampQualityName(InputTimestampQuality quality)
{
 switch(quality){case InputTimestampQuality::Observed:return QStringLiteral("Observed, not calibrated");
  case InputTimestampQuality::Unavailable:return QStringLiteral("Unavailable");
  case InputTimestampQuality::Discontinuous:return QStringLiteral("Discontinuous");}
 return QStringLiteral("Unavailable");
}
QString writerFaultName(WriterFault fault)
{
 switch(fault){case WriterFault::None:return {};case WriterFault::InvalidInput:return QStringLiteral("invalid input block");
  case WriterFault::InputIncomplete:return QStringLiteral("input ended with loss or mismatched counts");
  case WriterFault::WriteFailure:return QStringLiteral("storage write failed");case WriterFault::CheckpointFailure:return QStringLiteral("durable checkpoint failed");
  case WriterFault::SealFailure:return QStringLiteral("WAV seal failed");case WriterFault::JournalFailure:return QStringLiteral("take journal failed");
  case WriterFault::CounterOverflow:return QStringLiteral("capture counter overflow");}
 return QStringLiteral("unknown writer error");
}
}
RecordingQualificationView::RecordingQualificationView(RecordingServiceHost& host,RecordingProjectIdentity& projectIdentity,QWidget* parent)
 :QWidget(parent),m_host(host),m_projectIdentity(projectIdentity)
{
 setWindowTitle(tr("Vocal Recorder Qualification"));
 auto* form=new QFormLayout(this);
 m_rootLabel=new QLabel(tr("No recording folder selected"),this);m_rootLabel->setObjectName("recordingRootStatus");
 m_rootButton=new QPushButton(tr("Choose recording folder…"),this);m_rootButton->setObjectName("chooseRecordingRoot");
 m_inputDevices=new QComboBox(this);m_inputDevices->setObjectName("recordingInputDevice");m_inputDevices->setCurrentIndex(-1);
 m_enumerateButton=new QPushButton(tr("Select microphone…"),this);m_enumerateButton->setObjectName("enumerateRecordingInputs");
 m_channels=new QComboBox(this);m_channels->setObjectName("recordingInputChannel");m_channels->setCurrentIndex(-1);
 m_sampleRates=new QComboBox(this);m_sampleRates->setObjectName("recordingInputRate");
 m_sampleRates->addItem(tr("44.1 kHz"),44100);m_sampleRates->addItem(tr("48 kHz"),48000);m_sampleRates->setCurrentIndex(-1);
 m_takeLabel=new QLineEdit(this);m_takeLabel->setObjectName("recordingTakeLabel");m_takeLabel->setMaxLength(96);m_takeLabel->setPlaceholderText(tr("Take label"));
 m_statusLabel=new QLabel(tr("Input is closed. Microphone monitoring is off; timeline placement is manual and uncalibrated."),this);m_statusLabel->setObjectName("recordingInputStatus");
 m_meterLabel=new QLabel(tr("No capture"),this);m_meterLabel->setObjectName("recordingCaptureCounters");
 m_armButton=new QPushButton(tr("Arm locally"),this);m_armButton->setObjectName("armRecordingInput");
 m_startButton=new QPushButton(tr("Start take"),this);m_startButton->setObjectName("startRecordingTake");
 m_stopButton=new QPushButton(tr("Stop take"),this);m_stopButton->setObjectName("stopRecordingTake");
 m_disarmButton=new QPushButton(tr("Disarm"),this);m_disarmButton->setObjectName("disarmRecordingInput");
 form->addRow(tr("Recording library"),m_rootLabel);form->addRow(m_rootButton);form->addRow(m_enumerateButton,m_inputDevices);
 form->addRow(tr("Input channel"),m_channels);form->addRow(tr("Capture rate"),m_sampleRates);form->addRow(tr("Take label"),m_takeLabel);
 form->addRow(m_armButton,m_startButton);form->addRow(m_stopButton,m_disarmButton);form->addRow(tr("Status"),m_statusLabel);form->addRow(tr("Capture"),m_meterLabel);
 connect(m_rootButton,&QPushButton::clicked,this,&RecordingQualificationView::chooseRecordingRoot);
 connect(m_enumerateButton,&QPushButton::clicked,this,&RecordingQualificationView::refreshInputs);
 connect(m_inputDevices,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int index){
  if(m_localArm){std::ignore=m_host.disarm();m_localArm=false;}
  m_channels->clear();m_channels->setCurrentIndex(-1);
  if(index>=0 && std::size_t(index)<m_devices.size())
   for(std::uint32_t channel=0;channel<std::min<std::uint32_t>(m_devices[std::size_t(index)].maxInputChannels,32);++channel)
    m_channels->addItem(tr("Input channel %1").arg(channel+1),int(channel));
  m_channels->setCurrentIndex(-1);updateControls();
 });
 connect(m_channels,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{if(m_localArm){std::ignore=m_host.disarm();m_localArm=false;}updateControls();});
 connect(m_sampleRates,qOverload<int>(&QComboBox::currentIndexChanged),this,[this]{if(m_localArm){std::ignore=m_host.disarm();m_localArm=false;}updateControls();});
 connect(m_armButton,&QPushButton::clicked,this,&RecordingQualificationView::armLocally);
 connect(m_startButton,&QPushButton::clicked,this,&RecordingQualificationView::startTake);
 connect(m_stopButton,&QPushButton::clicked,this,&RecordingQualificationView::stopTake);
 connect(m_disarmButton,&QPushButton::clicked,this,&RecordingQualificationView::disarmLocally);
 m_pollTimer=new QTimer(this);m_pollTimer->setInterval(25);connect(m_pollTimer,&QTimer::timeout,this,&RecordingQualificationView::pollHost);
 m_clock.start();updateControls();
}
RecordingQualificationView::~RecordingQualificationView(){if(m_pollTimer)m_pollTimer->stop();}
void RecordingQualificationView::refreshInputs()
{
 m_inputDevices->clear();m_inputDevices->setCurrentIndex(-1);m_channels->clear();m_channels->setCurrentIndex(-1);m_devices.clear();
 const auto page=m_host.enumerateInputs(0,64);
 if(page.code!="ok"){m_statusLabel->setText(page.code);updateControls();return;}
 for(const auto& device:page.devices)
 {
  if(device.maxInputChannels==0 || device.maxInputChannels>32)continue;
  m_devices.push_back(device);m_inputDevices->addItem(device.name+QStringLiteral(" — ")+device.hostApiName,device.id);
 }
 m_inputDevices->setCurrentIndex(-1);
 m_statusLabel->setText(m_devices.empty()?tr("No supported WASAPI input was listed"):tr("Choose an input and channel explicitly"));updateControls();
}
int RecordingQualificationView::selectedInputIndex() const{return m_inputDevices->currentIndex();}
int RecordingQualificationView::selectedChannelIndex() const{return m_channels->currentIndex();}
int RecordingQualificationView::selectedSampleRate() const{return m_sampleRates->currentIndex()<0?0:m_sampleRates->currentData().toInt();}
void RecordingQualificationView::chooseRecordingRoot()
{
 const auto folder=QFileDialog::getExistingDirectory(this,tr("Choose recording folder"));if(folder.isEmpty())return;
 const auto selected=m_host.selectRecordingRoot(folder,m_projectIdentity);
 if(!selected.ok){m_statusLabel->setText(selected.code);return;}
 m_project=selected.project;m_localArm=false;m_rootLabel->setText(folder);m_statusLabel->setText(tr("Recording folder authorized for this project"));updateControls();
}
void RecordingQualificationView::armLocally()
{
 const auto deviceIndex=m_inputDevices->currentIndex(),channelIndex=m_channels->currentIndex(),sampleRate=selectedSampleRate();
 if(m_project.id.isEmpty() || m_projectIdentity.cachedIdentity().id!=m_project.id)
 {m_statusLabel->setText(tr("Select a recording folder for the current project"));m_localArm=false;updateControls();return;}
 if(deviceIndex<0 || channelIndex<0 || !sampleRate || std::size_t(deviceIndex)>=m_devices.size())return;
 if(m_sessionRevision==std::numeric_limits<std::uint64_t>::max()){m_statusLabel->setText(tr("Session revision limit reached"));return;}
 const auto projectRevision=m_projectIdentity.revision();if(!projectRevision){m_statusLabel->setText(tr("Project identity is unavailable"));return;}
 m_epoch={projectRevision,++m_sessionRevision};const auto& device=m_devices[std::size_t(deviceIndex)];
 SelectedInputProfile selected;selected.deviceId=device.id;selected.hostApiName=device.hostApiName;selected.wasapiShared=true;
 selected.input={std::uint32_t(sampleRate),std::min<std::uint32_t>(device.maxInputChannels,32),std::uint32_t(m_channels->currentData().toInt()),
  RecordingRing::MaximumCallback,std::uint64_t(sampleRate)*120};
 const auto result=m_host.arm(selected,m_epoch,m_host.rootRevision());
 if(!result.ok){m_statusLabel->setText(result.code);m_localArm=false;updateControls();return;}
 m_localArm=true;m_statusLabel->setText(tr("Locally armed. Input remains closed until Start."));updateControls();
}
void RecordingQualificationView::startTake()
{
 if(!m_localArm || m_project.id.isEmpty())return;
 const auto rate=std::uint32_t(selectedSampleRate());if(!rate)return;
 TakeIdentity identity{QUuid::createUuid().toString(QUuid::WithoutBraces),m_takeLabel->text()};
 if(identity.label.toUtf8().size()>96){m_statusLabel->setText(tr("Take label is too long"));return;}
 auto created=m_host.createUniqueTake(identity,rate,std::uint64_t(rate)*120);
 if(!created.sink){m_statusLabel->setText(created.code);return;}
 TakeDescriptor descriptor;descriptor.projectId=m_project.id;descriptor.takeId=identity.id;descriptor.label=identity.label;
 descriptor.epoch=m_epoch;descriptor.projectGeneration=m_epoch.projectGeneration;descriptor.maxRawFrames=std::uint64_t(rate)*120;
 descriptor.targetTick=0;descriptor.sampleRate=rate;
 const auto started=m_host.startTake(std::move(descriptor),std::move(created.sink),std::uint64_t(m_clock.elapsed()));
 if(!started.ok){m_statusLabel->setText(started.code);return;}
 m_localArm=false;m_statusLabel->setText(tr("Preparing writer; microphone opens only after the WAV header is ready."));m_pollTimer->start();updateControls();
}
void RecordingQualificationView::stopTake()
{
 const auto stopped=m_host.requestStop(m_epoch);m_statusLabel->setText(stopped.code);m_pollTimer->start();updateControls();
}
void RecordingQualificationView::disarmLocally()
{
 const auto result=m_host.disarm();m_localArm=false;m_statusLabel->setText(result.code);m_pollTimer->start();updateControls();
}
void RecordingQualificationView::pollHost()
{
 const auto status=m_host.poll(std::uint64_t(m_clock.elapsed()));
 const auto quality=timestampQualityName(status.input.input.timestampQuality);
 auto error=status.errorCode;if(error.isEmpty())error=status.input.errorCode;
 if(error.isEmpty() && status.writer.fault!=WriterFault::None)error=writerFaultName(status.writer.fault);
 m_statusLabel->setText((error.isEmpty()?stateName(status.state):error)
  +tr(" · input monitoring off · timeline placement manual and uncalibrated · timestamp quality %1 · drift unmeasured").arg(quality));
 if(status.input.input.epoch || status.input.input.receivedFrames || status.writer.writtenFrames)
  m_meterLabel->setText(tr("Received %1 · accepted %2 · written %3 · dropped %4 · beyond limit %5 · clipped %6 · invalid %7 · hardware loss %8 · input peak %9")
   .arg(qulonglong(status.input.input.receivedFrames)).arg(qulonglong(status.input.input.acceptedFrames)).arg(qulonglong(status.writer.writtenFrames))
   .arg(qulonglong(status.input.input.applicationDroppedFrames)).arg(qulonglong(status.input.input.intentionallyUnrecordedFrames))
   .arg(qulonglong(status.writer.clippedSamples)).arg(qulonglong(status.input.input.invalidSamples))
   .arg(status.input.input.hardwareLossCountKnown?tr("known %1 frames").arg(qulonglong(status.input.input.hardwareLostFrames)):tr("unknown"))
   .arg(status.input.input.inputPeak,0,'f',3));
 if(status.quiescent){m_pollTimer->stop();m_localArm=false;}
 updateControls();
}
void RecordingQualificationView::updateControls()
{
 const bool selected=m_inputDevices->currentIndex()>=0 && m_channels->currentIndex()>=0 && selectedSampleRate()!=0;
 const bool rooted=!m_project.id.isEmpty() && m_host.rootRevision()!=0;
 m_armButton->setEnabled(rooted && selected && !m_localArm);
 m_startButton->setEnabled(m_localArm);
 m_stopButton->setEnabled(m_localArm || m_pollTimer->isActive());m_disarmButton->setEnabled(m_localArm || m_pollTimer->isActive());
}
}
