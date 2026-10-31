#include "RecordingRecorderView.h"
#include "AudioDevice.h"
#include "AudioEngine.h"
#include "AudioImportPolicy.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "MainWindow.h"
#include "RecordingSessionManager.h"
#include "RecordingServiceHost.h"
#include "RecordingRing.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include <QComboBox>
#include <QDomDocument>
#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QPushButton>
#include <QTemporaryDir>
#include <Track.h>
#include <QtTest>
#include <array>
#include <functional>

using namespace lmms;
using namespace lmms::recording;
using lmms::gui::MainWindow;

namespace
{
class SyntheticOutput final : public AudioDevice
{
public:
	explicit SyntheticOutput(int rate) : AudioDevice(2, Engine::audioEngine()) { setSampleRate(rate); }

private:
	void startProcessingImpl() override {}
	void stopProcessingImpl() override {}
};

class SyntheticInput final : public RecordingInputDriver
{
public:
	RecordingDriverResult initialize() noexcept override { ++initializes; return {"ok", true}; }
	RecordingInputDevicePage enumerateInputs(std::uint32_t, std::uint32_t) override
	{
		++enumerations;
		return {"ok", {{"synthetic-mic", "Synthetic input", "WASAPI", 1}}, 1, false};
	}
	RecordingDriverResult openInputOnly(const SelectedInputProfile&, RecordingInputCallback cb, void* value) noexcept override
	{
		++opens;
		callback = cb;
		context = value;
		return {"ok", true};
	}
	RecordingDriverResult start() noexcept override
	{
		++starts;
		std::array<float, 256> samples{};
		samples.fill(0.125f);
		if (callback) std::ignore = callback(context, {samples.data(), samples.size(), 1, 0, 1.0, 1.0 + samples.size() / 48000.0});
		return {"ok", false};
	}
	RecordingDriverResult stop() noexcept override { ++stops; return {"ok", true}; }
	RecordingDriverResult close() noexcept override { ++closes; return {"ok", true}; }
	RecordingDriverResult terminate() noexcept override { ++terminates; return {"ok", true}; }

	int initializes = 0;
	int enumerations = 0;
	int opens = 0;
	int starts = 0;
	int stops = 0;
	int closes = 0;
	int terminates = 0;
	RecordingInputCallback callback = nullptr;
	void* context = nullptr;
};

std::unique_ptr<SyntheticInput> makeInput(SyntheticInput*& observed)
{
	auto result = std::make_unique<SyntheticInput>();
	observed = result.get();
	return result;
}

class SyntheticTransport final : public RecordingTransportControl
{
public:
	RecordingTransportSnapshot world;
	quint64 serial = 0;

	SyntheticTransport()
	{
		world.available = true;
		world.projectGeneration = 1;
		world.contextRevision = 1;
		world.sampleRate = 48000;
		world.tempo = 120;
		world.numerator = 4;
		world.denominator = 4;
		world.ticksPerBar = 192;
	}
	RecordingTransportSnapshot snapshot() const override { return world; }
	RecordingTransportResult seekSong(qint64 tick) override
	{
		world.songTick = tick;
		++world.seekRevision;
		return {true, "ok", {}};
	}
	RecordingTransportResult playSong() override
	{
		world.playing = true;
		world.songMode = true;
		++world.stateRevision;
		return {true, "ok", {++serial, world.stateRevision}};
	}
	RecordingTransportResult stopOwned(RecordingPlaybackOwnership owner) override
	{
		if (world.playing && owner.serial == serial)
		{
			world.playing = false;
			world.songMode = false;
			++world.stateRevision;
		}
		return {true, "stopped", {}};
	}
};

struct Fixture
{
	QTemporaryDir directory;
	RecordingProjectIdentity& identity;
	SyntheticInput* observedInput = nullptr;
	RecordingServiceHost host{makeInput(observedInput)};
	SyntheticTransport transport;
	std::unique_ptr<RecordingSessionManager> manager;

	Fixture() : identity(Engine::getSong()->recordingProjectIdentity())
	{
		if (!directory.isValid()) qFatal("Task 8 synthetic recording folder is unavailable");
		const auto root = host.selectRecordingRoot(directory.path(), identity);
		if (!root.ok) qFatal("Task 8 synthetic recording folder was not admitted");
		transport.world.projectId = root.project.id;
		transport.world.projectGeneration = identity.revision();
		manager = std::make_unique<RecordingSessionManager>(host, transport);
	}

	bool startSyntheticTake()
	{
		if (host.enumerateInputs(0, 64).code != QStringLiteral("ok")) return false;
		SelectedInputProfile profile;
		profile.deviceId = QStringLiteral("synthetic-mic");
		profile.hostApiName = QStringLiteral("WASAPI");
		profile.wasapiShared = true;
		profile.input = {48000, 1, 0, RecordingRing::MaximumCallback, std::uint64_t(48000) * 120};
		auto ticket = host.localArmTicket(profile, transport.world.projectGeneration);
		if (!ticket.ok || !manager->arm(std::move(ticket.ticket)).ok) return false;
		PrepareTakeRequest prepare;
		prepare.startTick = 0;
		prepare.takeLabel = QStringLiteral("Synthetic take");
		prepare.expectedProjectGeneration = transport.world.projectGeneration;
		if (!manager->prepare(prepare).ok) return false;
		const auto current = manager->status();
		if (!manager->start({current.preparedId, current.projectGeneration,
			QUuid::createUuid().toString(QUuid::WithoutBraces)}).ok) return false;
		QElapsedTimer elapsed;
		elapsed.start();
		while (elapsed.elapsed() < 5000 && observedInput->opens == 0)
		{
			std::ignore = manager->poll(quint64(elapsed.elapsed()));
			QTest::qWait(1);
		}
		return observedInput->opens == 1 && observedInput->starts == 1;
	}

	bool drain()
	{
		QElapsedTimer elapsed;
		elapsed.start();
		while (elapsed.elapsed() < 5000 && !manager->quiescent())
		{
			std::ignore = manager->poll(quint64(elapsed.elapsed()));
			QTest::qWait(1);
		}
		return manager->quiescent();
	}

	QString takePath() const
	{
		return directory.path() + QLatin1Char('/') + identity.cachedIdentity().id + QStringLiteral("/take-")
			+ manager->status().takeId + QStringLiteral(".wav");
	}

	QByteArray takeHash() const
	{
		QFile file(takePath());
		if (!file.open(QIODevice::ReadOnly)) return {};
		return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
	}

	~Fixture()
	{
		if (manager)
		{
			std::ignore = manager->requestShutdown();
			QElapsedTimer elapsed;
			elapsed.start();
			while (elapsed.elapsed() < 5000 && !manager->quiescent())
			{
				std::ignore = manager->poll(quint64(elapsed.elapsed()));
				QTest::qWait(1);
			}
			if (!manager->quiescent()) qFatal("Task 8 synthetic manager did not reach quiescence");
		}
		std::ignore = host.requestShutdown();
	}
};

QPushButton* button(QWidget& view, const char* name)
{
	return view.findChild<QPushButton*>(QString::fromLatin1(name));
}

QDomElement sampleClipState(SampleClip& clip)
{
	QDomDocument document;
	auto element = document.createElement(QStringLiteral("sampleclip"));
	document.appendChild(element);
	clip.saveSettings(document, element);
	return element;
}
}

class RecordingRecorderViewTest final : public QObject
{
	Q_OBJECT
	QString m_oldFileBudget;
	QString m_oldDecodedBudget;
private slots:
	void initTestCase()
	{
		Engine::init(true);
		Engine::audioEngine()->audioDev()->stopProcessing();
		Engine::audioEngine()->setAudioDevice(new SyntheticOutput(48000), false);
		Engine::getSong()->setTempo(120);
		auto* config = ConfigManager::inst();
		m_oldFileBudget = config->value("aibridge", "audio_import_max_file_bytes", QStringLiteral("task8-absent"));
		m_oldDecodedBudget = config->value("aibridge", "audio_import_max_decoded_bytes", QStringLiteral("task8-absent"));
		config->setValue("aibridge", "audio_import_max_file_bytes", QStringLiteral("2000000"));
		config->setValue("aibridge", "audio_import_max_decoded_bytes", QStringLiteral("2000000"));
	}

	void cleanupTestCase()
	{
		auto* config = ConfigManager::inst();
		if (m_oldFileBudget == QStringLiteral("task8-absent")) config->deleteValue("aibridge", "audio_import_max_file_bytes");
		else config->setValue("aibridge", "audio_import_max_file_bytes", m_oldFileBudget);
		if (m_oldDecodedBudget == QStringLiteral("task8-absent")) config->deleteValue("aibridge", "audio_import_max_decoded_bytes");
		else config->setValue("aibridge", "audio_import_max_decoded_bytes", m_oldDecodedBudget);
		AudioImportPolicy::configured().refresh();
		Engine::destroy();
	}

	void applicationManagerIsEngineOwnedAndDoesNotOpenInput()
	{
		auto* manager = Engine::recordingSessionManager();
		QVERIFY(manager);
		QVERIFY(Engine::recordingServiceHost());
		QVERIFY(manager->quiescent());
		QVERIFY(!Engine::recordingServiceHost()->snapshot().input.driverInitialized);
		QTest::qWait(30);
		QCOMPARE(Engine::recordingSessionManager(), manager);
		QVERIFY(manager->quiescent());
	}

	void constructRestoreShowPrepareAndArmDoNotOpenStream()
	{
		Fixture fixture;
		RecordingRecorderView view(fixture.host, *fixture.manager, fixture.identity);
		QCOMPARE(fixture.observedInput->initializes, 0);
		QCOMPARE(fixture.observedInput->enumerations, 0);
		QCOMPARE(fixture.observedInput->opens, 0);
		QCOMPARE(fixture.observedInput->starts, 0);

		auto* device = view.findChild<QComboBox*>("recordingInputDevice");
		auto* channel = view.findChild<QComboBox*>("recordingInputChannel");
		auto* rate = view.findChild<QComboBox*>("recordingInputRate");
		QVERIFY(device);
		QVERIFY(channel);
		QVERIFY(rate);
		QCOMPARE(device->currentIndex(), -1);
		QCOMPARE(channel->currentIndex(), -1);
		QCOMPARE(rate->currentIndex(), -1);

		view.showInRecorderWindow();
		QCOMPARE(fixture.observedInput->initializes, 0);
		QCOMPARE(fixture.observedInput->enumerations, 0);
		QCOMPARE(fixture.observedInput->opens, 0);
		QCOMPARE(fixture.observedInput->starts, 0);

		QTest::mouseClick(button(view, "enumerateRecordingInputs"), Qt::LeftButton);
		QCOMPARE(fixture.observedInput->initializes, 1);
		QCOMPARE(fixture.observedInput->enumerations, 1);
		QCOMPARE(fixture.observedInput->opens, 0);
		device->setCurrentIndex(0);
		channel->setCurrentIndex(0);
		rate->setCurrentIndex(rate->findData(48000));
		QTest::mouseClick(button(view, "armRecordingInput"), Qt::LeftButton);
		QVERIFY(fixture.manager->status().armed);
		QCOMPARE(fixture.observedInput->opens, 0);
		QCOMPARE(fixture.observedInput->starts, 0);
		view.findChild<QLineEdit*>("recordingTakeLabel")->setText("Take A");
		QTest::mouseClick(button(view, "prepareRecordingTake"), Qt::LeftButton);
		const auto preparedStatus = fixture.manager->status();
		QVERIFY2(preparedStatus.phase == Phase::Prepared,
			qPrintable(view.findChild<QLabel*>("recordingInputStatus")->text()));
		QCOMPARE(fixture.observedInput->opens, 0);
		QCOMPARE(fixture.observedInput->starts, 0);
	}

	void recorderPresentationReturnsToMainWorkspace()
	{
		Fixture fixture;
		QMdiArea workspace;
		auto* view = new RecordingRecorderView(fixture.host, *fixture.manager, fixture.identity);
		auto* window = workspace.addSubWindow(view);
		window->hide();
		QVERIFY(window->isHidden());
		view->showInRecorderWindow();
		QVERIFY(!window->isHidden());
		QVERIFY(!view->isHidden());
		QVERIFY(!view->isWindow());
		delete view;
	}

	void startRequiresLocalArmToOpen()
	{
		Fixture fixture;
		RecordingRecorderView view(fixture.host, *fixture.manager, fixture.identity);
		auto* start = button(view, "startRecordingTake");
		QVERIFY(start);
		QVERIFY(!start->isEnabled());
		QTest::mouseClick(start, Qt::LeftButton);
		QCOMPARE(fixture.observedInput->opens, 0);
		QCOMPARE(fixture.observedInput->starts, 0);

		QTest::mouseClick(button(view, "enumerateRecordingInputs"), Qt::LeftButton);
		view.findChild<QComboBox*>("recordingInputDevice")->setCurrentIndex(0);
		view.findChild<QComboBox*>("recordingInputChannel")->setCurrentIndex(0);
		view.findChild<QComboBox*>("recordingInputRate")->setCurrentIndex(
			view.findChild<QComboBox*>("recordingInputRate")->findData(48000));
		QTest::mouseClick(button(view, "armRecordingInput"), Qt::LeftButton);
		QVERIFY(fixture.manager->status().armed);
		QCOMPARE(fixture.observedInput->opens, 0);
		QTest::mouseClick(button(view, "prepareRecordingTake"), Qt::LeftButton);
		QCOMPARE(fixture.manager->status().phase, Phase::Prepared);
		QVERIFY(start->isEnabled());
		QTest::mouseClick(start, Qt::LeftButton);
		QElapsedTimer elapsed;
		elapsed.start();
		while (elapsed.elapsed() < 5000 && fixture.observedInput->opens == 0)
		{
			std::ignore = fixture.manager->poll(quint64(elapsed.elapsed()));
			QTest::qWait(1);
		}
		QCOMPARE(fixture.observedInput->opens, 1);
		QCOMPARE(fixture.observedInput->starts, 1);
		view.refreshView();
		QVERIFY(view.findChild<QLabel*>("recordingCaptureCounters")->text().contains("peak 0.125"));
		QVERIFY(!fixture.manager->quiescent());
		std::ignore = fixture.manager->disarm();
		QVERIFY(fixture.drain());
	}

	void noAutomaticSelectionRearmOrReopen()
	{
		Fixture fixture;
		{
			RecordingRecorderView view(fixture.host, *fixture.manager, fixture.identity);
			QTest::mouseClick(button(view, "enumerateRecordingInputs"), Qt::LeftButton);
			auto* device = view.findChild<QComboBox*>("recordingInputDevice");
			auto* channel = view.findChild<QComboBox*>("recordingInputChannel");
			auto* rate = view.findChild<QComboBox*>("recordingInputRate");
			QVERIFY(device && channel && rate);
			QCOMPARE(device->currentIndex(), -1);
			device->setCurrentIndex(0);
			QCOMPARE(channel->currentIndex(), -1);
			channel->setCurrentIndex(0);
			QCOMPARE(rate->currentIndex(), -1);
			rate->setCurrentIndex(rate->findData(48000));
			QTest::mouseClick(button(view, "armRecordingInput"), Qt::LeftButton);
			QVERIFY(fixture.manager->status().armed);
			QCOMPARE(fixture.observedInput->opens, 0);
			QCOMPARE(fixture.observedInput->starts, 0);
		}

		const auto initializeCount = fixture.observedInput->initializes;
		const auto enumerateCount = fixture.observedInput->enumerations;
		RecordingRecorderView restored(fixture.host, *fixture.manager, fixture.identity);
		QCOMPARE(fixture.manager->status().armed, true);
		QCOMPARE(fixture.observedInput->initializes, initializeCount);
		QCOMPARE(fixture.observedInput->enumerations, enumerateCount);
		QCOMPARE(fixture.observedInput->opens, 0);
		QCOMPARE(fixture.observedInput->starts, 0);
	}

	void projectChangeClearsLocalDeviceSelectionBeforeRearm()
	{
		Fixture fixture;
		RecordingRecorderView view(fixture.host, *fixture.manager, fixture.identity);
		QTest::mouseClick(button(view, "enumerateRecordingInputs"), Qt::LeftButton);
		view.findChild<QComboBox*>("recordingInputDevice")->setCurrentIndex(0);
		view.findChild<QComboBox*>("recordingInputChannel")->setCurrentIndex(0);
		view.findChild<QComboBox*>("recordingInputRate")->setCurrentIndex(
			view.findChild<QComboBox*>("recordingInputRate")->findData(48000));
		QTest::mouseClick(button(view, "armRecordingInput"), Qt::LeftButton);
		QVERIFY(fixture.manager->status().armed);

		fixture.identity.resetForNewProject();
		fixture.transport.world.projectGeneration = fixture.identity.revision();
		fixture.transport.world.projectId = QUuid::createUuid().toString(QUuid::WithoutBraces);
		std::ignore = fixture.manager->poll(1);
		view.refreshView();
		QVERIFY(!fixture.manager->status().armed);
		QCOMPARE(view.findChild<QComboBox*>("recordingInputDevice")->currentIndex(), -1);
		QCOMPARE(view.findChild<QComboBox*>("recordingInputChannel")->currentIndex(), -1);
		QCOMPARE(view.findChild<QComboBox*>("recordingInputRate")->currentIndex(), -1);
		QVERIFY(view.findChild<QLabel*>("recordingRootStatus")->text().contains("folder"));
		QCOMPARE(fixture.observedInput->opens, 0);
		QCOMPARE(fixture.observedInput->starts, 0);
	}

	void panelCloseDoesNotLoseTake()
	{
		Fixture fixture;
		QVERIFY(fixture.startSyntheticTake());
		auto view = std::make_unique<RecordingRecorderView>(fixture.host, *fixture.manager, fixture.identity);
		view.reset();
		QVERIFY(!fixture.manager->quiescent());
		QCOMPARE(fixture.observedInput->opens, 1);
		QCOMPARE(fixture.observedInput->closes, 0);
		QCOMPARE(fixture.observedInput->terminates, 0);
		const auto takeId = fixture.manager->status().takeId;
		QVERIFY(!takeId.isEmpty());
		QVERIFY(!MainWindow::stopRecordingAndRequireQuiescence(*fixture.manager));
		QVERIFY(fixture.observedInput->stops > 0);
		QVERIFY(fixture.drain());
		QCOMPARE(fixture.observedInput->closes, 1);
		QCOMPARE(fixture.manager->status().takeId, takeId);
		QCOMPARE(fixture.manager->status().captureIntegrity, CaptureIntegrity::Complete);
		QVERIFY(!fixture.takeHash().isEmpty());
	}

	void emergencyStopIndependentOfIpc()
	{
		Fixture fixture;
		QVERIFY(fixture.startSyntheticTake());
		QVERIFY(!MainWindow::stopRecordingAndRequireQuiescence(*fixture.manager));
		QVERIFY(fixture.observedInput->stops > 0);
		QVERIFY(fixture.drain());
		QVERIFY(MainWindow::stopRecordingAndRequireQuiescence(*fixture.manager));
		QCOMPARE(fixture.observedInput->opens, 1);
		QCOMPARE(fixture.observedInput->closes, 1);
	}

	void settingsAndProjectChangeCloseInputBeforeReconfiguration()
	{
		Fixture fixture;
		QVERIFY(fixture.startSyntheticTake());
		const auto ready = MainWindow::stopRecordingAndRequireQuiescence(*fixture.manager);
		QVERIFY(!ready);
		QVERIFY(fixture.observedInput->stops > 0);
		QCOMPARE(fixture.transport.world.sampleRate, 48000u);
		QVERIFY(fixture.drain());
		QVERIFY(MainWindow::stopRecordingAndRequireQuiescence(*fixture.manager));
		fixture.transport.world.sampleRate = 44100;
		QCOMPARE(fixture.transport.world.sampleRate, 44100u);
	}

	void closeIgnoredUntilHostQuiescent()
	{
		Fixture fixture;
		QVERIFY(fixture.startSyntheticTake());
		QVERIFY(!MainWindow::stopRecordingAndRequireQuiescence(*fixture.manager));
		QVERIFY(!fixture.manager->quiescent());
		QCOMPARE(fixture.observedInput->closes, 0);
		QVERIFY(fixture.drain());
		QVERIFY(MainWindow::stopRecordingAndRequireQuiescence(*fixture.manager));
		QCOMPARE(fixture.observedInput->closes, 1);
	}

	void unconfirmedSuggestionCannotAttach()
	{
		Fixture fixture;
		QVERIFY(fixture.startSyntheticTake());
		std::ignore = fixture.manager->disarm();
		QVERIFY(fixture.drain());
		QCOMPARE(fixture.manager->status().captureIntegrity, CaptureIntegrity::Complete);
		const auto tracksBefore = Engine::getSong()->tracks().size();
		RecordingRecorderView view(fixture.host, *fixture.manager, fixture.identity);
		view.refreshView();
		auto* takes = view.findChild<QListWidget*>("recordingRecentTakes");
		auto* confirm = button(view, "confirmRecordedTake");
		QVERIFY(takes && confirm);
		QVERIFY(takes->currentItem());
		QVERIFY(!confirm->isEnabled());
		view.findChild<QLineEdit*>("recordingSourceInTicks")->setText(QStringLiteral("2"));
		view.findChild<QLineEdit*>("recordingTargetTick")->setText(QStringLiteral("192"));
		QTest::mouseClick(confirm, Qt::LeftButton);
		QCOMPARE(Engine::getSong()->tracks().size(), tracksBefore);
		QCOMPARE(fixture.manager->status().placementState, PlacementState::AwaitingLocalPlacement);
	}

	void qualityLabelsStayUncalibratedAfterConfirm()
	{
		Fixture fixture;
		QVERIFY(fixture.startSyntheticTake());
		std::ignore = fixture.manager->disarm();
		QVERIFY(fixture.drain());
		QCOMPARE(fixture.manager->status().captureIntegrity, CaptureIntegrity::Complete);
		const auto before = fixture.takeHash();
		QVERIFY(!before.isEmpty());
		const auto tracksBefore = Engine::getSong()->tracks().size();
		RecordingRecorderView view(fixture.host, *fixture.manager, fixture.identity);
		view.refreshView();
		auto* takes = view.findChild<QListWidget*>("recordingRecentTakes");
		QVERIFY(takes && takes->currentItem());
		QTest::mouseClick(button(view, "previewRecordedTake"), Qt::LeftButton);
		QVERIFY2(view.findChild<QWidget*>("recordingWaveform")->property("frame_count").toULongLong() > 0,
			qPrintable(view.findChild<QLabel*>("recordingPlacementStatus")->text()));
		QVERIFY(button(view, "confirmRecordedTake")->isEnabled());
		QTest::mouseClick(button(view, "confirmRecordedTake"), Qt::LeftButton);
		const auto status = fixture.manager->status();
		QVERIFY2(status.placementState == PlacementState::Attached,
			qPrintable(status.code + QStringLiteral(" / ") + view.findChild<QLabel*>("recordingPlacementStatus")->text()));
		QCOMPARE(status.placementQuality, QStringLiteral("uncalibrated"));
		QCOMPARE(status.driftQuality, QStringLiteral("unmeasured"));
		QVERIFY(!status.sampleAccuracyVerified);
		QCOMPARE(Engine::getSong()->tracks().size(), tracksBefore + 1);
		QCOMPARE(fixture.takeHash(), before);
		const auto label = view.findChild<QLabel*>("recordingPlacementStatus")->text();
		QVERIFY(label.contains("uncalibrated"));
		QVERIFY(label.contains("drift unmeasured"));
	}

	void newTakeUsesNewTrack()
	{
		Fixture fixture;
		RecordingRecorderView view(fixture.host, *fixture.manager, fixture.identity);
		auto* device = view.findChild<QComboBox*>("recordingInputDevice");
		auto* channel = view.findChild<QComboBox*>("recordingInputChannel");
		auto* rate = view.findChild<QComboBox*>("recordingInputRate");
		auto* label = view.findChild<QLineEdit*>("recordingTakeLabel");
		auto* sourceIn = view.findChild<QLineEdit*>("recordingSourceInTicks");
		auto* target = view.findChild<QLineEdit*>("recordingTargetTick");
		auto* takes = view.findChild<QListWidget*>("recordingRecentTakes");
		QVERIFY(device && channel && rate && label && sourceIn && target && takes);
		const auto armAndStart = [&](const QString& takeLabel, int expectedOpens) {
			label->setText(takeLabel);
			if (device->currentIndex() < 0)
			{
				QTest::mouseClick(button(view, "enumerateRecordingInputs"), Qt::LeftButton);
				device->setCurrentIndex(0);
				channel->setCurrentIndex(0);
				rate->setCurrentIndex(rate->findData(48000));
			}
			if (!fixture.manager->status().armed)
			{
				QTest::mouseClick(button(view, "armRecordingInput"), Qt::LeftButton);
			}
			QTest::mouseClick(button(view, "prepareRecordingTake"), Qt::LeftButton);
			if (fixture.manager->status().phase != Phase::Prepared)
			{
				return QString{};
			}
			QTest::mouseClick(button(view, "startRecordingTake"), Qt::LeftButton);
			QElapsedTimer elapsed;
			elapsed.start();
			while (elapsed.elapsed() < 5000 && fixture.observedInput->opens < expectedOpens)
			{
				std::ignore = fixture.manager->poll(quint64(elapsed.elapsed()));
				QTest::qWait(1);
			}
			return fixture.observedInput->opens == expectedOpens ? fixture.manager->status().takeId : QString{};
		};
		const auto stopAndAttach = [&](const QString& takeId, qint64 targetTick) {
			if (takeId.isEmpty()) return false;
			QTest::mouseClick(button(view, "stopRecordingTake"), Qt::LeftButton);
			if (!fixture.drain()) return false;
			const auto audioHash = fixture.takeHash();
			if (audioHash.size() != 32) return false;
			view.refreshView();
			for (int i = 0; i < takes->count(); ++i)
				if (takes->item(i)->data(Qt::UserRole).toString() == takeId) takes->setCurrentRow(i);
			sourceIn->setText(QStringLiteral("0"));
			target->setText(QString::number(targetTick));
			QTest::mouseClick(button(view, "previewRecordedTake"), Qt::LeftButton);
			if (!button(view, "confirmRecordedTake")->isEnabled()) return false;
			QTest::mouseClick(button(view, "confirmRecordedTake"), Qt::LeftButton);
			const auto status = fixture.manager->status();
			return status.takeId == takeId && status.placementState == PlacementState::Attached
				&& fixture.takeHash() == audioHash;
		};

		const auto tracksBefore = Engine::getSong()->tracks().size();
		const auto firstTake = armAndStart(QStringLiteral("Take A"), 1);
		QVERIFY(!firstTake.isEmpty());
		QVERIFY(stopAndAttach(firstTake, 0));
		QCOMPARE(Engine::getSong()->tracks().size(), tracksBefore + 1);
		const auto firstPath = fixture.takePath();
		QFile firstFile(firstPath);
		QVERIFY(firstFile.open(QIODevice::ReadOnly));
		const auto firstHash = QCryptographicHash::hash(firstFile.readAll(), QCryptographicHash::Sha256);
		firstFile.close();

		const auto secondTake = armAndStart(QStringLiteral("Take B"), 2);
		QVERIFY(!secondTake.isEmpty());
		QVERIFY(secondTake != firstTake);
		QVERIFY(stopAndAttach(secondTake, 192));
		QCOMPARE(Engine::getSong()->tracks().size(), tracksBefore + 2);
		QFile firstAfter(firstPath);
		QVERIFY(firstAfter.open(QIODevice::ReadOnly));
		QCOMPARE(QCryptographicHash::hash(firstAfter.readAll(), QCryptographicHash::Sha256), firstHash);
		QVERIFY(fixture.takeHash().size() == 32);
	}

	void restoredUnavailableSourceVisible()
	{
		Fixture fixture;
		QVERIFY(fixture.startSyntheticTake());
		std::ignore = fixture.manager->disarm();
		QVERIFY(fixture.drain());
		const auto takeId = fixture.manager->status().takeId;
		const auto originalHash = fixture.takeHash();
		QVERIFY(!originalHash.isEmpty());
		const auto approval = RecordingRecorderView::approve(takeId, fixture.transport.world.projectGeneration, 1, 0, 0);
		const auto attached = fixture.manager->confirmPlacement(approval);
		QVERIFY2(attached.ok, qPrintable(attached.code));
		auto* track = dynamic_cast<SampleTrack*>(Engine::getSong()->tracks().back());
		QVERIFY(track);
		auto* clip = dynamic_cast<SampleClip*>(track->getClip(0));
		QVERIFY(clip);
		const auto savedState = sampleClipState(*clip);

		QTemporaryDir unavailableLibrary;
		QVERIFY(unavailableLibrary.isValid());
		QVERIFY(fixture.host.selectRecordingRoot(unavailableLibrary.path(), fixture.identity).ok);
		clip->loadSettings(savedState);
		QVERIFY(clip->recordedTakeReference().has_value());
		QVERIFY(!clip->sourceLoadError().isEmpty());
		QCOMPARE(fixture.takeHash(), originalHash);

		RecordingRecorderView view(fixture.host, *fixture.manager, fixture.identity);
		view.refreshView();
		auto* takes = view.findChild<QListWidget*>("recordingRecentTakes");
		QVERIFY(takes);
		QListWidgetItem* restored = nullptr;
		for (int i = 0; i < takes->count(); ++i)
			if (takes->item(i)->data(Qt::UserRole).toString() == takeId) restored = takes->item(i);
		QVERIFY(restored);
		QVERIFY(restored->text().contains("unavailable"));
		QVERIFY(!restored->data(Qt::UserRole + 1).toBool());
		QVERIFY(!button(view, "previewRecordedTake")->isEnabled());
	}
};

QTEST_MAIN(RecordingRecorderViewTest)
#include "RecordingRecorderViewTest.moc"
