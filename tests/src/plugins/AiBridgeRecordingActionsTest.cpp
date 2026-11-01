#include "AiBridgeRecordingActions.h"
#include "AudioDevice.h"
#include "Engine.h"
#include "RecordingRing.h"
#include "RecordingSessionManager.h"
#include "RecordingServiceHost.h"
#include "Song.h"
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <array>

using namespace lmms;
using namespace lmms::recording;

namespace
{
const QString idA = QStringLiteral("00000000-0000-4000-8000-000000000001");
const QString idB = QStringLiteral("00000000-0000-4000-8000-000000000002");

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
		std::array<float, 64> frames{};
		frames.fill(0.25f);
		if (callback) std::ignore = callback(context, {frames.data(), frames.size(), 1, 0, 1.0, 1.0 + frames.size() / 48000.0});
		return {"ok", false};
	}
	RecordingDriverResult stop() noexcept override { ++stops; return {"ok", true}; }
	RecordingDriverResult close() noexcept override { ++closes; return {"ok", true}; }
	RecordingDriverResult terminate() noexcept override { ++terminates; return {"ok", true}; }
	int initializes = 0, enumerations = 0, opens = 0, starts = 0, stops = 0, closes = 0, terminates = 0;
	RecordingInputCallback callback = nullptr;
	void* context = nullptr;
};

class SyntheticOutput final : public AudioDevice
{
public:
	explicit SyntheticOutput(int rate) : AudioDevice(2, Engine::audioEngine()) { setSampleRate(rate); }
private:
	void startProcessingImpl() override {}
	void stopProcessingImpl() override {}
};

std::unique_ptr<SyntheticInput> makeInput(SyntheticInput*& observed)
{
	auto input = std::make_unique<SyntheticInput>();
	observed = input.get();
	return input;
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
		world.sampleRate = 48000;
		world.tempo = 120;
		world.numerator = 4;
		world.denominator = 4;
		world.ticksPerBar = 192;
	}
	RecordingTransportSnapshot snapshot() const override { return world; }
	RecordingTransportResult seekSong(qint64 tick) override { world.songTick = tick; ++world.seekRevision; return {true, "ok", {}}; }
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
	RecordingServiceHost host;
	SyntheticTransport transport;
	std::unique_ptr<RecordingSessionManager> manager;
	std::unique_ptr<AiBridgeRecordingActions> actions;

	Fixture() : identity(Engine::getSong()->recordingProjectIdentity()), host(makeInput(observedInput))
	{
		if (!directory.isValid()) qFatal("Recording action test library unavailable");
		const auto root = host.selectRecordingRoot(directory.path(), identity);
		if (!root.ok) qFatal("Recording action test library not authorized");
		transport.world.projectId = root.project.id;
		transport.world.projectGeneration = identity.revision();
		manager = std::make_unique<RecordingSessionManager>(host, transport);
		actions = std::make_unique<AiBridgeRecordingActions>(manager.get());
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
			if (!manager->quiescent()) qFatal("Recording action fixture did not quiesce");
		}
		std::ignore = host.requestShutdown();
	}
};

AiBridgeProtocolRequest request(const QString& action, const QJsonObject& parameters = {})
{
	return {2, QStringLiteral("recording-action-test"), action, parameters};
}

QJsonObject prepareParameters(quint64 generation)
{
	return {{"start_tick", 0}, {"take_label", "Synthetic take"}, {"count_in_bars", 0},
		{"metronome_enabled", false}, {"expected_project_generation", QString::number(generation)}};
}

bool waitForOpens(Fixture& fixture, int expected)
{
	QElapsedTimer elapsed;
	elapsed.start();
	while (elapsed.elapsed() < 5000 && fixture.observedInput->opens < expected)
	{
		std::ignore = fixture.manager->poll(quint64(elapsed.elapsed()));
		QTest::qWait(1);
	}
	return fixture.observedInput->opens == expected;
}
}

class AiBridgeRecordingActionsTest final : public QObject
{
	Q_OBJECT
private slots:
	void initTestCase()
	{
		Engine::init(true);
		Engine::audioEngine()->audioDev()->stopProcessing();
		Engine::audioEngine()->setAudioDevice(new SyntheticOutput(48000), false);
		Engine::getSong()->setTempo(120);
	}
	void cleanupTestCase() { Engine::destroy(); }

	void prepareNoDeviceCalls()
	{
		Fixture fixture;
		const auto response = fixture.actions->execute(request(QStringLiteral("prepare_recording_take"),
			prepareParameters(fixture.transport.world.projectGeneration)));
		QVERIFY2(response.success, qPrintable(response.code));
		QCOMPARE(fixture.observedInput->initializes, 0);
		QCOMPARE(fixture.observedInput->enumerations, 0);
		QCOMPARE(fixture.observedInput->opens, 0);
		QCOMPARE(fixture.observedInput->starts, 0);
	}

	void statusAndCapabilitiesAreDeviceFreeAndBounded()
	{
		Fixture fixture;
		const auto capabilities = fixture.actions->execute(request(QStringLiteral("get_recording_capabilities")));
		const auto status = fixture.actions->execute(request(QStringLiteral("get_recording_status")));
		QVERIFY(capabilities.success && status.success);
		QVERIFY(capabilities.data.toObject().value(QStringLiteral("requires_local_arm")).toBool());
		QCOMPARE(capabilities.data.toObject().value(QStringLiteral("maximum_take_seconds")).toInt(), 120);
		QVERIFY(status.data.toObject().value(QStringLiteral("received_frames")).isString());
		const auto wire = AiBridgeProtocol::serializeResponse(status);
		for (const auto& forbidden : {"path", "pcm", "digest", "sha256", "serial", "exception", "device_id"})
			QVERIFY(!wire.contains(forbidden));
		QCOMPARE(fixture.observedInput->initializes, 0);
		QCOMPARE(fixture.observedInput->enumerations, 0);
		QCOMPARE(fixture.observedInput->opens, 0);
	}

	void metronomeRequestIsExplicitlyUnsupported()
	{
		Fixture fixture;
		auto parameters = prepareParameters(fixture.transport.world.projectGeneration);
		parameters.insert(QStringLiteral("metronome_enabled"), true);
		const auto response = fixture.actions->execute(request(QStringLiteral("prepare_recording_take"), parameters));
		QVERIFY(!response.success);
		QCOMPARE(response.code, QStringLiteral("unsupported_metronome"));
		QCOMPARE(fixture.observedInput->opens, 0);
	}

	void startNeedsCurrentLocalArmAndDuplicateRequestDoesNotReplay()
	{
		Fixture fixture;
		const auto prepared = fixture.actions->execute(request(QStringLiteral("prepare_recording_take"),
			prepareParameters(fixture.transport.world.projectGeneration)));
		QVERIFY(prepared.success);
		const auto preparedId = prepared.data.toObject().value(QStringLiteral("prepared_take_id")).toString();
		QVERIFY(!preparedId.isEmpty());
		const auto start = request(QStringLiteral("start_recording_take"), {{"prepared_take_id", preparedId},
			{"expected_project_generation", QString::number(fixture.transport.world.projectGeneration)},
			{"start_request_id", idB}});
		const auto unauthorized = fixture.actions->execute(start);
		QVERIFY(!unauthorized.success);
		QCOMPARE(unauthorized.code, QStringLiteral("not_armed"));
		QCOMPARE(fixture.observedInput->opens, 0);

		QCOMPARE(fixture.host.enumerateInputs(0, 64).code, QStringLiteral("ok"));
		SelectedInputProfile profile;
		profile.deviceId = QStringLiteral("synthetic-mic");
		profile.hostApiName = QStringLiteral("WASAPI");
		profile.wasapiShared = true;
		profile.input = {48000, 1, 0, RecordingRing::MaximumCallback, std::uint64_t(48000) * 120};
		auto ticket = fixture.host.localArmTicket(profile, fixture.transport.world.projectGeneration);
		QVERIFY(ticket.ok);
		QVERIFY(fixture.manager->arm(std::move(ticket.ticket)).ok);
		const auto accepted = fixture.actions->execute(start);
		QVERIFY(accepted.success);
		QCOMPARE(fixture.observedInput->opens, 0);
		const auto duplicate = fixture.actions->execute(start);
		QVERIFY(duplicate.success);
		QVERIFY(waitForOpens(fixture, 1));
		QCOMPARE(fixture.observedInput->opens, 1);
		const auto stop = request(QStringLiteral("stop_recording_take"),
			{{"take_id", accepted.data.toObject().value(QStringLiteral("take_id")).toString()}});
		QVERIFY(fixture.actions->execute(stop).success);
		QVERIFY(fixture.actions->execute(stop).success);
		QCOMPARE(fixture.observedInput->opens, 1);
		QVERIFY(fixture.manager->quiescent() || fixture.manager->disarm().ok);
	}

	void stopIsIdempotentAndClosesSyntheticInputOnce()
	{
		Fixture fixture;
		const auto prepared = fixture.actions->execute(request(QStringLiteral("prepare_recording_take"),
			prepareParameters(fixture.transport.world.projectGeneration)));
		QVERIFY(prepared.success);
		const auto preparedId = prepared.data.toObject().value(QStringLiteral("prepared_take_id")).toString();
		QCOMPARE(fixture.host.enumerateInputs(0, 64).code, QStringLiteral("ok"));
		SelectedInputProfile profile;
		profile.deviceId = QStringLiteral("synthetic-mic");
		profile.hostApiName = QStringLiteral("WASAPI");
		profile.wasapiShared = true;
		profile.input = {48000, 1, 0, RecordingRing::MaximumCallback, std::uint64_t(48000) * 120};
		auto ticket = fixture.host.localArmTicket(profile, fixture.transport.world.projectGeneration);
		QVERIFY(ticket.ok);
		QVERIFY(fixture.manager->arm(std::move(ticket.ticket)).ok);
		const auto start = fixture.actions->execute(request(QStringLiteral("start_recording_take"),
			{{"prepared_take_id", preparedId},
			 {"expected_project_generation", QString::number(fixture.transport.world.projectGeneration)},
			 {"start_request_id", idB}}));
		QVERIFY(start.success);
		QVERIFY(waitForOpens(fixture, 1));
		const auto takeId = start.data.toObject().value(QStringLiteral("take_id")).toString();
		QVERIFY(!takeId.isEmpty());
		const auto stop = request(QStringLiteral("stop_recording_take"), {{"take_id", takeId}});
		QVERIFY(fixture.actions->execute(stop).success);
		QVERIFY(fixture.actions->execute(stop).success);

		QElapsedTimer elapsed;
		elapsed.start();
		while (elapsed.elapsed() < 5000 && !fixture.manager->quiescent())
		{
			std::ignore = fixture.manager->poll(quint64(elapsed.elapsed()));
			QTest::qWait(1);
		}
		QVERIFY(fixture.manager->quiescent());
		QCOMPARE(fixture.observedInput->stops, 1);
		QCOMPARE(fixture.observedInput->closes, 1);
		QCOMPARE(fixture.observedInput->terminates, 1);
	}

	void completedTakeStatusWaitsForLocalPlacement()
	{
		Fixture fixture;
		const auto prepared = fixture.actions->execute(request(QStringLiteral("prepare_recording_take"),
			prepareParameters(fixture.transport.world.projectGeneration)));
		QVERIFY(prepared.success);
		const auto preparedId = prepared.data.toObject().value(QStringLiteral("prepared_take_id")).toString();
		QCOMPARE(fixture.host.enumerateInputs(0, 64).code, QStringLiteral("ok"));
		SelectedInputProfile profile;
		profile.deviceId = QStringLiteral("synthetic-mic");
		profile.hostApiName = QStringLiteral("WASAPI");
		profile.wasapiShared = true;
		profile.input = {48000, 1, 0, RecordingRing::MaximumCallback, std::uint64_t(48000) * 120};
		auto ticket = fixture.host.localArmTicket(profile, fixture.transport.world.projectGeneration);
		QVERIFY(ticket.ok);
		QVERIFY(fixture.manager->arm(std::move(ticket.ticket)).ok);
		const auto started = fixture.actions->execute(request(QStringLiteral("start_recording_take"),
			{{"prepared_take_id", preparedId},
			 {"expected_project_generation", QString::number(fixture.transport.world.projectGeneration)},
			 {"start_request_id", idB}}));
		QVERIFY(started.success);
		QVERIFY(waitForOpens(fixture, 1));
		std::ignore = fixture.manager->poll(100);
		const auto takeId = started.data.toObject().value(QStringLiteral("take_id")).toString();
		QVERIFY(fixture.actions->execute(request(QStringLiteral("stop_recording_take"), {{"take_id", takeId}})).success);

		QElapsedTimer elapsed;
		elapsed.start();
		while (elapsed.elapsed() < 5000 && !fixture.manager->quiescent())
		{
			std::ignore = fixture.manager->poll(quint64(elapsed.elapsed()) + 100);
			QTest::qWait(1);
		}
		QVERIFY(fixture.manager->quiescent());
		const auto status = fixture.actions->execute(request(QStringLiteral("get_recording_status")));
		QVERIFY(status.success);
		QCOMPARE(status.data.toObject().value(QStringLiteral("placement_state")).toString(),
			QStringLiteral("awaiting_local_placement"));
		QCOMPARE(status.data.toObject().value(QStringLiteral("placement_quality")).toString(),
			QStringLiteral("uncalibrated"));
		QCOMPARE(status.data.toObject().value(QStringLiteral("drift_quality")).toString(),
			QStringLiteral("unmeasured"));
		const auto page = fixture.actions->execute(request(QStringLiteral("list_recorded_takes"),
			{{"offset", 0}, {"limit", 1}}));
		QVERIFY2(page.success, qPrintable(page.code));
		const auto takes = page.data.toObject().value(QStringLiteral("takes")).toArray();
		QCOMPARE(takes.size(), 1);
		QCOMPARE(takes[0].toObject().value(QStringLiteral("take_id")).toString(), takeId);
		QCOMPARE(takes[0].toObject().value(QStringLiteral("placement_state")).toString(),
			QStringLiteral("awaiting_local_placement"));
	}

	void fullRecordingPageEnvelopeStaysBounded()
	{
		RecordingCapabilities capabilities;
		QVERIFY(!AiBridgeRecordingTypes::capabilitiesJson(capabilities).isEmpty());
		TakePage page;
		page.code = QStringLiteral("ok");
		for (int i = 0; i < 100; ++i)
		{
			RecordingStatus status;
			status.takeLabel = QString(96, QChar('x'));
			status.takeId = idA;
			status.projectGeneration = std::numeric_limits<quint64>::max();
			status.receivedFrames = std::numeric_limits<quint64>::max();
			page.takes.push_back(status);
		}
		page.nextOffset = 100;
		const auto data = AiBridgeRecordingTypes::takePageJson(page, {0, 100});
		const auto frame = AiBridgeProtocol::serializeResponse({2, QStringLiteral("large-page"), true,
			QStringLiteral("ok"), QStringLiteral("page"), data});
		QVERIFY(frame.size() <= AiBridgeProtocol::MaxFrameBytes);

		const auto oversized = AiBridgeProtocol::serializeResponse({2, QStringLiteral("oversized"), true,
			QStringLiteral("ok"), QStringLiteral("large"), QJsonObject{{"payload", QString(70000, QChar('x'))}}});
		QVERIFY(oversized.size() <= AiBridgeProtocol::MaxFrameBytes);
		const auto fallback = QJsonDocument::fromJson(oversized).object();
		QCOMPARE(fallback.value(QStringLiteral("code")).toString(), QStringLiteral("result_too_large"));
	}
};

QTEST_MAIN(AiBridgeRecordingActionsTest)
#include "AiBridgeRecordingActionsTest.moc"
