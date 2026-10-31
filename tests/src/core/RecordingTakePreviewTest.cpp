#include "RecordingTakePreview.h"
#include "RecordingRecorderView.h"
#include "AudioDevice.h"
#include "AudioEngine.h"
#include "AudioImportPolicy.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "RecordingPlacement.h"
#include "RecordingProjectIdentity.h"
#include "RecordingRing.h"
#include "RecordingServiceHost.h"
#include "RecordingWriter.h"
#include "SampleFrame.h"
#include "Song.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <algorithm>
#include <array>
#include <cmath>

using namespace lmms;
using namespace lmms::recording;

namespace
{
class NoDeviceDriver;
std::unique_ptr<NoDeviceDriver> makeDriver(NoDeviceDriver*& observed);
class SyntheticOutput final : public AudioDevice
{
public:
	explicit SyntheticOutput(int rate) : AudioDevice(2, Engine::audioEngine()) { setSampleRate(rate); }

private:
	void startProcessingImpl() override {}
	void stopProcessingImpl() override {}
};

class NoDeviceDriver final : public RecordingInputDriver
{
public:
	int opens = 0;
	RecordingDriverResult initialize() noexcept override { return {"ok", true}; }
	RecordingInputDevicePage enumerateInputs(std::uint32_t, std::uint32_t) override { return {"ok", {}, 0, false}; }
	RecordingDriverResult openInputOnly(const SelectedInputProfile&, RecordingInputCallback, void*) noexcept override
	{
		++opens;
		return {"physical_input_forbidden_in_synthetic_test", false};
	}
	RecordingDriverResult start() noexcept override { return {"physical_input_forbidden_in_synthetic_test", false}; }
	RecordingDriverResult stop() noexcept override { return {"ok", true}; }
	RecordingDriverResult close() noexcept override { return {"ok", true}; }
	RecordingDriverResult terminate() noexcept override { return {"ok", true}; }
};

std::unique_ptr<NoDeviceDriver> makeDriver(NoDeviceDriver*& observed)
{
	auto driver = std::make_unique<NoDeviceDriver>();
	observed = driver.get();
	return driver;
}

bool waitForWriter(RecordingWriter& writer, WriterState state)
{
	QElapsedTimer elapsed;
	elapsed.start();
	while (elapsed.elapsed() < 5000)
	{
		if (writer.poll().state == state) return true;
		QTest::qWait(1);
	}
	return writer.poll().state == state;
}

bool writeSyntheticTake(RecordingServiceHost& host, const ProjectIdentity& project, const QString& takeId,
	std::uint64_t frames)
{
	constexpr std::uint32_t rate = 48000;
	constexpr std::uint64_t maximum = std::uint64_t(rate) * 120;
	auto created = host.createUniqueTake({takeId, QStringLiteral("preview fixture")}, rate, maximum);
	if (!created.sink) return false;
	RecordingRing ring{rate, RecordingRing::Capacity, maximum};
	TakeDescriptor descriptor;
	descriptor.projectId = project.id;
	descriptor.takeId = takeId;
	descriptor.label = QStringLiteral("preview fixture");
	descriptor.epoch = {1, 1};
	descriptor.projectGeneration = 1;
	descriptor.maxRawFrames = maximum;
	descriptor.sampleRate = rate;
	RecordingWriter writer;
	if (!writer.begin(descriptor, std::move(created.sink), ring).ok || !waitForWriter(writer, WriterState::Ready)) return false;
	std::array<float, RecordingRing::MaximumCallback> samples{};
	std::uint64_t at = 0;
	while (at < frames)
	{
		const auto count = std::size_t(std::min<std::uint64_t>(samples.size(), frames - at));
		for (std::size_t i = 0; i < count; ++i) samples[i] = at + i < 128 ? 0.75f : 0.20f;
		if (ring.push(samples.data(), count, 1, 0).acceptedFrames != count) return false;
		at += count;
	}
	InputSnapshot final;
	final.epoch = descriptor.epoch;
	final.receivedFrames = frames;
	final.acceptedFrames = frames;
	final.hardwareLossCountKnown = true;
	writer.requestInputClosed(descriptor.epoch, final);
	return waitForWriter(writer, WriterState::Sealed) && writer.joinAfterExit().ok;
}

QByteArray sha256(const QString& path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) return {};
	return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

struct Fixture
{
	QTemporaryDir directory;
	RecordingProjectIdentity identity{Engine::getSong()};
	NoDeviceDriver* driver = nullptr;
	RecordingServiceHost host{makeDriver(driver)};
	ProjectIdentity project;
	QString takeId = QUuid::createUuid().toString(QUuid::WithoutBraces);
	QString audioPath;
	QByteArray originalHash;

	Fixture()
	{
		if (!directory.isValid()) qFatal("Synthetic preview take directory unavailable");
		const auto selected = host.selectRecordingRoot(directory.path(), identity);
		if (!selected.ok) qFatal("Synthetic preview library was not admitted");
		project = selected.project;
		if (!writeSyntheticTake(host, project, takeId, 7000)) qFatal("Synthetic preview WAV did not seal");
		audioPath = directory.path() + QLatin1Char('/') + project.id + QStringLiteral("/take-")
			+ takeId + QStringLiteral(".wav");
		originalHash = sha256(audioPath);
		if (originalHash.size() != 32) qFatal("Synthetic preview WAV hash unavailable");
	}

	~Fixture()
	{
		std::ignore = host.requestShutdown();
	}

	PlacementPlan planFor(const SealedTakeToken& source) const
	{
		RecordingTransportSnapshot transport;
		transport.available = true;
		transport.projectId = project.id;
		transport.projectGeneration = 1;
		transport.contextRevision = 1;
		transport.sampleRate = 48000;
		transport.tempo = 120;
		transport.numerator = 4;
		transport.denominator = 4;
		transport.ticksPerBar = 192;
		NativeGeometrySnapshot geometry;
		geometry.current = transport;
		geometry.reviewed = transport;
		geometry.reviewedSource = source.facts();
		geometry.placementRevision = 1;
		geometry.sourceFramesPerTick = Engine::framesPerTick(48000);
		const auto approval = RecordingRecorderView::approve(takeId, 1, 1, 2, 0);
		const auto result = RecordingPlacement::plan(source.facts(), approval, geometry);
		if (!result.plan) qFatal("Synthetic preview source-in plan was rejected");
		return *result.plan;
	}
};
}

class RecordingTakePreviewTest final : public QObject
{
	Q_OBJECT
	QStringList m_oldSettings;
private slots:
	void initTestCase()
	{
		Engine::init(true);
		Engine::audioEngine()->audioDev()->stopProcessing();
		Engine::audioEngine()->setAudioDevice(new SyntheticOutput(48000), false);
		Engine::getSong()->clearProject();
		Engine::getSong()->recordingProjectIdentity().resetForNewProject();
		auto* config = ConfigManager::inst();
		for (const auto& key : {QStringLiteral("audio_import_max_file_bytes"), QStringLiteral("audio_import_max_decoded_bytes")})
			m_oldSettings.push_back(config->value(QStringLiteral("aibridge"), key, QStringLiteral("task8-absent")));
		config->setValue(QStringLiteral("aibridge"), QStringLiteral("audio_import_max_file_bytes"), QStringLiteral("2000000"));
		config->setValue(QStringLiteral("aibridge"), QStringLiteral("audio_import_max_decoded_bytes"), QStringLiteral("2000000"));
	}
	void cleanupTestCase()
	{
		auto* config = ConfigManager::inst();
		const QStringList keys{QStringLiteral("audio_import_max_file_bytes"), QStringLiteral("audio_import_max_decoded_bytes")};
		for (int i = 0; i < keys.size(); ++i)
		{
			if (m_oldSettings[i] == QStringLiteral("task8-absent")) config->deleteValue(QStringLiteral("aibridge"), keys[i]);
			else config->setValue(QStringLiteral("aibridge"), keys[i], m_oldSettings[i]);
		}
		AudioImportPolicy::configured().refresh();
		Engine::destroy();
	}

	void manualSourceInPreviewExcludesPrefix()
	{
		Fixture fixture;
		auto opened = fixture.host.openSealedTake(fixture.project, fixture.takeId);
		QVERIFY(opened.source);
		const auto plan = fixture.planFor(*opened.source);
		QVERIFY(plan.effectiveSourceInFrame > 128);
		RecordingTakePreview preview(*Engine::audioEngine());
		QCOMPARE(preview.play(std::move(*opened.source), plan), QStringLiteral("ok"));
		QVERIFY(preview.active());
		QCOMPARE(preview.waveform().sourceInFrame, plan.effectiveSourceInFrame);
		QCOMPARE(preview.waveform().frameCount, quint64(7000));
		QVERIFY(!preview.waveform().peakEnvelope.empty());
		const auto output = Engine::audioEngine()->renderNextPeriod();
		QVERIFY(!output.empty());
		QVERIFY2(std::abs(output.front().left()) < 0.45f, "preview included the distinguishable pre-roll prefix");
		QCOMPARE(sha256(fixture.audioPath), fixture.originalHash);
		QCOMPARE(fixture.driver->opens, 0);
		QCOMPARE(preview.stop(), QStringLiteral("stopped"));
		QVERIFY(!preview.active());
	}

	void previewOwnerRetiresOnGui()
	{
		Fixture fixture;
		auto opened = fixture.host.openSealedTake(fixture.project, fixture.takeId);
		QVERIFY(opened.source);
		const auto plan = fixture.planFor(*opened.source);
		RecordingTakePreview preview(*Engine::audioEngine());
		QCOMPARE(preview.play(std::move(*opened.source), plan), QStringLiteral("ok"));
		QCOMPARE(preview.ownerThread(), QThread::currentThread());
		QCOMPARE(preview.stop(), QStringLiteral("stopped"));
		QCOMPARE(preview.stop(), QStringLiteral("already_stopped"));
		QVERIFY(!preview.active());
		QCOMPARE(sha256(fixture.audioPath), fixture.originalHash);
		QCOMPARE(fixture.driver->opens, 0);
	}
};

QTEST_MAIN(RecordingTakePreviewTest)
#include "RecordingTakePreviewTest.moc"
