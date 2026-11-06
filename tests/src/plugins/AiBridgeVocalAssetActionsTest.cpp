#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QDomDocument>
#include <QFile>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QUuid>
#include <QtEndian>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <numbers>
#include <vector>
#include "AiBridgeAudioContext.h"
#include "AiBridgeVocalComp.h"
#include "AiBridgeVocalContext.h"
#include "AiBridgeProjectContext.h"
#include "AiBridgeVocalAssetActions.h"
#include "AiBridgeVocalJobs.h"
#include "AiBridgeVocalTrackActions.h"
#include "AiBridgeVocalVariantAttachment.h"
#include "AudioImportPolicy.h"
#include "AudioDevice.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "ManagedVocalAssetRoot.h"
#include "ManagedVocalAssetStore.h"
#include "ProjectJournal.h"
#include "RecordedTakeSource.h"
#include "RecordingServiceHost.h"
#include "RecordingWriter.h"
#include "SampleClip.h"
#include "SampleBuffer.h"
#include "SampleTrack.h"
#include "Song.h"

using namespace lmms;

class VocalAssetProbeDevice final : public AudioDevice
{
public:
	explicit VocalAssetProbeDevice(AudioEngine* engine) : AudioDevice(2, engine)
	{
		setSampleRate(48000);
	}

private:
	void startProcessingImpl() override {}
	void stopProcessingImpl() override {}
};

bool waitForWriterState(recording::RecordingWriter& writer, recording::WriterState state)
{
	QElapsedTimer timer;
	timer.start();
	while (timer.elapsed() < 3000)
	{
		if (writer.poll().state == state) return true;
		QTest::qWait(1);
	}
	return writer.poll().state == state;
}

bool writeSyntheticRecordedTake(recording::RecordingServiceHost& host,
	const recording::ProjectIdentity& project, const QString& takeId)
{
	constexpr std::uint32_t sampleRate = 48000;
	constexpr std::uint64_t maximumFrames = std::uint64_t(sampleRate) * 120;
	auto created = host.createUniqueTake({takeId, QStringLiteral("vocal bridge fixture")}, sampleRate, maximumFrames);
	if (created.code != QStringLiteral("ok") || !created.sink) return false;
	recording::RecordingRing ring{sampleRate, 131072, maximumFrames};
	recording::TakeDescriptor descriptor;
	descriptor.projectId = project.id;
	descriptor.takeId = takeId;
	descriptor.label = QStringLiteral("vocal bridge fixture");
	descriptor.epoch = {1, 1};
	descriptor.projectGeneration = 1;
	descriptor.maxRawFrames = maximumFrames;
	descriptor.sampleRate = sampleRate;
	recording::RecordingWriter writer;
	if (!writer.begin(descriptor, std::move(created.sink), ring).ok
		|| !waitForWriterState(writer, recording::WriterState::Ready)) return false;
	const float samples[]{0.0f, 0.25f, -0.5f, 0.125f};
	if (ring.push(samples, 4, 1, 0).acceptedFrames != 4) return false;
	recording::InputSnapshot end;
	end.epoch = descriptor.epoch;
	end.receivedFrames = 4;
	end.acceptedFrames = 4;
	end.hardwareLossCountKnown = true;
	writer.requestInputClosed(descriptor.epoch, end);
	return waitForWriterState(writer, recording::WriterState::Sealed) && writer.joinAfterExit().ok;
}

class AiBridgeVocalAssetActionsTest : public QObject
{
	Q_OBJECT
	struct RenderGate
	{
		std::mutex mutex;
		std::condition_variable condition;
		std::atomic_bool entered{false};
		bool open = false;
		void release()
		{
			std::lock_guard lock(mutex);
			open = true;
			condition.notify_all();
		}
		VocalVariantRenderResult run(const std::atomic_bool& cancelled)
		{
			entered.store(true, std::memory_order_release);
			std::unique_lock lock(mutex);
			condition.wait(lock, [&] { return open || cancelled.load(std::memory_order_acquire); });
			return {cancelled.load(std::memory_order_acquire) ? QStringLiteral("cancelled")
				: QStringLiteral("ok"), {}, {}, 1024};
		}
	};
	std::unique_ptr<QTemporaryDir> m_audioRoot;
	std::unique_ptr<QTemporaryDir> m_assetRoot;
	std::unique_ptr<QTemporaryDir> m_projectDirectory;
	std::unique_ptr<QTemporaryDir> m_identityDirectory;
	std::unique_ptr<recording::RecordingLibrary> m_identityLibrary;
	std::vector<std::unique_ptr<QTemporaryDir>> m_recordingRoots;
	std::unique_ptr<SampleTrack> m_track;
	SampleClip* m_clip = nullptr;
	QStringList m_oldAudioSettings;
	QString m_oldManagedRoot;
	QString m_sourcePath;

	static QString uuid()
	{
		return QUuid::createUuid().toString(QUuid::WithoutBraces);
	}

	static QByteArray sourceWave(std::array<qint16, 4> samples = {0, 8192, 16384, -8192})
	{
		QByteArray bytes(44 + qsizetype(samples.size() * sizeof(qint16)), 0);
		std::memcpy(bytes.data(), "RIFF", 4);
		std::memcpy(bytes.data() + 8, "WAVEfmt ", 8);
		std::memcpy(bytes.data() + 36, "data", 4);
		qToLittleEndian<quint32>(quint32(bytes.size() - 8), bytes.data() + 4);
		qToLittleEndian<quint32>(16, bytes.data() + 16);
		qToLittleEndian<quint16>(1, bytes.data() + 20);
		qToLittleEndian<quint16>(1, bytes.data() + 22);
		qToLittleEndian<quint32>(48000, bytes.data() + 24);
		qToLittleEndian<quint32>(96000, bytes.data() + 28);
		qToLittleEndian<quint16>(2, bytes.data() + 32);
		qToLittleEndian<quint16>(16, bytes.data() + 34);
		qToLittleEndian<quint32>(quint32(samples.size() * sizeof(qint16)), bytes.data() + 40);
		for (size_t i = 0; i < samples.size(); ++i)
		{
			qToLittleEndian<qint16>(samples[i], bytes.data() + 44 + qsizetype(i * sizeof(qint16)));
		}
		return bytes;
	}

	static QByteArray timingImpulseWave()
	{
		constexpr quint32 rate = 48000;
		constexpr quint32 frames = rate;
		QByteArray bytes(44 + qsizetype(frames * sizeof(qint16)), 0);
		std::memcpy(bytes.data(), "RIFF", 4);
		std::memcpy(bytes.data() + 8, "WAVEfmt ", 8);
		std::memcpy(bytes.data() + 36, "data", 4);
		qToLittleEndian<quint32>(quint32(bytes.size() - 8), bytes.data() + 4);
		qToLittleEndian<quint32>(16, bytes.data() + 16);
		qToLittleEndian<quint16>(1, bytes.data() + 20);
		qToLittleEndian<quint16>(1, bytes.data() + 22);
		qToLittleEndian<quint32>(rate, bytes.data() + 24);
		qToLittleEndian<quint32>(rate * sizeof(qint16), bytes.data() + 28);
		qToLittleEndian<quint16>(sizeof(qint16), bytes.data() + 32);
		qToLittleEndian<quint16>(16, bytes.data() + 34);
		qToLittleEndian<quint32>(frames * sizeof(qint16), bytes.data() + 40);
		qToLittleEndian<qint16>(22937, bytes.data() + 44 + 24000 * qsizetype(sizeof(qint16)));
		return bytes;
	}

	static QString fileHash(const QString& path)
	{
		QFile file(path);
		return file.open(QIODevice::ReadOnly)
			? QString::fromLatin1(QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex())
			: QString{};
	}

	VocalEditLimits limits() const
	{
		VocalEditLimits value;
		value.maxSelections = 8;
		value.maxTimingAnchors = 256;
		value.maxGainFadePoints = 1024;
		value.maxCompSegments = 8;
		value.maxCrossfadeFrames = 12000;
		value.minimumGainDb = -48.0;
		value.maximumGainDb = 24.0;
		value.minimumLocalStretchRatio = 0.80;
		value.maximumLocalStretchRatio = 1.25;
		value.maximumAnchorErrorFrames = 1.0;
		return value;
	}

	VocalEditWorld world() const
	{
		auto* song = Engine::getSong();
		return {song->getTempo(), int(Engine::audioEngine()->outputSampleRate()), 48,
			song->tempoModel().isAutomatedOrControlled(), song->getTimeSigModel().getNumerator(),
			song->getTimeSigModel().getDenominator()};
	}

	VocalEditRequest request() const
	{
		AiBridgeProjectContext ids;
		const auto clipId = ids.idForClip(m_clip);
		const auto captured = AiBridgeAudioContext{}.clip(clipId);
		Q_ASSERT(captured.value);
		const auto capturedWorld = world();
		VocalEditRequest result;
		result.expectedProjectGeneration = ids.projectGeneration();
		result.expectedWorldHash = vocalEditWorldHash(capturedWorld);
		result.selections.push_back({clipId, captured.value->stateHash,
			VocalEditSourceBasis::SourcePcm, {0, 4}});
		result.input = VocalDirectInput{0};
		result.operations.push_back(VocalGainEnvelope{{{0, 6.0}, {4, 6.0}}});
		return result;
	}

	void installPersistentProjectIdentity()
	{
		m_identityDirectory = std::make_unique<QTemporaryDir>();
		m_projectDirectory = std::make_unique<QTemporaryDir>();
		QVERIFY(m_identityDirectory->isValid() && m_projectDirectory->isValid());
		auto approval = recording::RecordingLibrary::authorizeLocalRoot(m_identityDirectory->path());
		QVERIFY(approval.library);
		m_identityLibrary = std::move(approval.library);
		auto* song = Engine::getSong();
		song->recordingProjectIdentity().resetForNewProject();
		const auto identity = song->recordingProjectIdentity().getOrCreateForCurrentProject(*m_identityLibrary);
		QCOMPARE(identity.code, QStringLiteral("ok"));
		QVERIFY(song->guiSaveProjectAs(m_projectDirectory->filePath(QStringLiteral("project.mmp"))));
	}

private slots:
	void initTestCase()
	{
		Engine::init(true);
		Engine::audioEngine()->audioDev()->stopProcessing();
		Engine::audioEngine()->setAudioDevice(new VocalAssetProbeDevice(Engine::audioEngine()), false);
	}

	void cleanupTestCase()
	{
		Engine::destroy();
		m_recordingRoots.clear();
	}

	void init()
	{
		const QStringList keys{QStringLiteral("audio_import_roots"),
			QStringLiteral("audio_import_max_file_bytes"), QStringLiteral("audio_import_max_decoded_bytes")};
		for (const auto& key : keys)
		{
			m_oldAudioSettings.push_back(ConfigManager::inst()->value(QStringLiteral("aibridge"), key,
				QStringLiteral("__unset__")));
		}
		m_oldManagedRoot = ConfigManager::inst()->value(QStringLiteral("vocal_assets"),
			QStringLiteral("managed_root"), QStringLiteral("__unset__"));
		m_audioRoot = std::make_unique<QTemporaryDir>();
		m_assetRoot = std::make_unique<QTemporaryDir>();
		QVERIFY(m_audioRoot->isValid() && m_assetRoot->isValid());
		const auto sourceBytes = sourceWave();
		m_sourcePath = m_audioRoot->filePath(QStringLiteral("original.wav"));
		QFile source(m_sourcePath);
		QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::NewOnly));
		QCOMPARE(source.write(sourceBytes), qint64(sourceBytes.size()));
		source.close();
		ConfigManager::inst()->setValue(QStringLiteral("aibridge"), QStringLiteral("audio_import_roots"),
			QString::fromUtf8(QJsonDocument(QJsonArray{m_audioRoot->path()}).toJson(QJsonDocument::Compact)));
		ConfigManager::inst()->setValue(QStringLiteral("aibridge"), QStringLiteral("audio_import_max_file_bytes"),
			QStringLiteral("10000"));
		ConfigManager::inst()->setValue(QStringLiteral("aibridge"), QStringLiteral("audio_import_max_decoded_bytes"),
			QStringLiteral("10000"));
		ConfigManager::inst()->setValue(QStringLiteral("vocal_assets"), QStringLiteral("managed_root"),
			QDir::fromNativeSeparators(m_assetRoot->path()));
		AudioImportPolicy::configured().refresh();
		m_track = std::make_unique<SampleTrack>(Engine::getSong());
		m_clip = new SampleClip(m_track.get());
		auto imported = AudioImportPolicy::configured().prepare(m_sourcePath);
		QCOMPARE(imported.code, QStringLiteral("ok"));
		QCOMPARE(m_clip->adoptAuthorizedAudio(std::move(imported.audio)), QStringLiteral("ok"));
		if (QByteArray(QTest::currentTestFunction()) != QByteArrayLiteral("unsavedProjectCannotPublishEvenWithRecordingIdentity"))
			installPersistentProjectIdentity();
	}

	void cleanup()
	{
		m_track.reset();
		m_clip = nullptr;
		m_identityLibrary.reset();
		m_identityDirectory.reset();
		m_projectDirectory.reset();
		const QStringList keys{QStringLiteral("audio_import_roots"),
			QStringLiteral("audio_import_max_file_bytes"), QStringLiteral("audio_import_max_decoded_bytes")};
		for (int i = 0; i < keys.size(); ++i)
		{
			if (m_oldAudioSettings[i] == QStringLiteral("__unset__"))
				ConfigManager::inst()->deleteValue(QStringLiteral("aibridge"), keys[i]);
			else
				ConfigManager::inst()->setValue(QStringLiteral("aibridge"), keys[i], m_oldAudioSettings[i]);
		}
		m_oldAudioSettings.clear();
		if (m_oldManagedRoot == QStringLiteral("__unset__"))
			ConfigManager::inst()->deleteValue(QStringLiteral("vocal_assets"), QStringLiteral("managed_root"));
		else
			ConfigManager::inst()->setValue(QStringLiteral("vocal_assets"), QStringLiteral("managed_root"),
				m_oldManagedRoot);
		AudioImportPolicy::configured().refresh();
		m_assetRoot.reset();
		m_audioRoot.reset();
		m_sourcePath.clear();
		Engine::projectJournal()->clearJournal();
		Engine::projectJournal()->setJournalling(false);
	}

	void unsavedProjectCannotPublishEvenWithRecordingIdentity()
	{
		auto* song = Engine::getSong();
		QVERIFY(song->projectFileName().isEmpty());
		song->recordingProjectIdentity().resetForNewProject();
		QTemporaryDir recordingDirectory;
		QVERIFY(recordingDirectory.isValid());
		auto library = recording::RecordingLibrary::authorizeLocalRoot(recordingDirectory.path());
		QVERIFY(library.library);
		const auto identity = song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);
		QCOMPARE(identity.code, QStringLiteral("ok"));
		const auto plan = AiBridgeVocalAssetActions::plan(request(), limits());
		QCOMPARE(plan.code, QStringLiteral("ok"));
		QVERIFY(plan.value);
		QCOMPARE(AiBridgeVocalAssetActions::prepareRender(*plan.value).code,
			QStringLiteral("project_identity_unverified"));
	}

	void dryRunIsSideEffectFreeAndRenderPublishesAProjectBoundDetachedVariant()
	{
		const auto sourceHash = fileHash(m_sourcePath);
		QVERIFY(!sourceHash.isEmpty());
		const auto before = QDir(m_assetRoot->path()).entryList(QDir::Files | QDir::NoDotAndDotDot);
		const auto plan = AiBridgeVocalAssetActions::plan(request(), limits());
		QCOMPARE(plan.code, QStringLiteral("ok"));
		QVERIFY(plan.value);
		QCOMPARE(QDir(m_assetRoot->path()).entryList(QDir::Files | QDir::NoDotAndDotDot), before);
		auto tamperedSource = *plan.value;
		tamperedSource.sources.sources.front().reference.append(QStringLiteral(".redirected"));
		QCOMPARE(AiBridgeVocalAssetActions::dryRun(tamperedSource).code, QStringLiteral("plan_stale"));

		const auto prepared = AiBridgeVocalAssetActions::prepareRender(*plan.value);
		QCOMPARE(prepared.code, QStringLiteral("ok"));
		QVERIFY(prepared.value);
		const auto beforeDryRun = QDir(m_assetRoot->path()).entryList(QDir::Files | QDir::NoDotAndDotDot);
		const auto estimate = AiBridgeVocalAssetActions::dryRun(*plan.value);
		QCOMPARE(estimate.code, QStringLiteral("ok"));
		QCOMPARE(estimate.outputFrames, quint64(4));
		QCOMPARE(QDir(m_assetRoot->path()).entryList(QDir::Files | QDir::NoDotAndDotDot), beforeDryRun);

		std::atomic_bool cancelled{false};
		const auto result = std::async(std::launch::async, [&] {
			return AiBridgeVocalAssetActions::render(*prepared.value, cancelled, [](VocalJobPhase) {});
		}).get();
		QCOMPARE(result.code, QStringLiteral("ok"));
		QVERIFY(result.variant);
		QCOMPARE(result.variant->frames, quint64(4));
		QCOMPARE(result.variant->sampleRate, 48000);
		QCOMPARE(fileHash(m_sourcePath), sourceHash);

		auto root = ManagedVocalAssetRoot::authorizeConfiguredRoot();
		QVERIFY(root.root);
		ManagedVocalAssetStore store(root.root);
		const auto opened = store.openVerified(*result.variant);
		QCOMPARE(opened.code, QStringLiteral("ok"));
		QCOMPARE(opened.manifest->projectId,
			Engine::getSong()->recordingProjectIdentity().cachedIdentity().id);
		QCOMPARE(opened.manifest->rendererOptions.value(QStringLiteral("gain_interpolation")).toString(),
			QStringLiteral("linear_db"));
		QVERIFY(opened.manifest->realizedFrameMaps.at(0).toObject()
			.value(QStringLiteral("source_measurements")).toObject().contains(QStringLiteral("rms")));
		const auto gain = std::pow(10.0f, 6.0f / 20.0f);
		QCOMPARE(opened.samples.size(), size_t(4));
		QVERIFY(std::abs(opened.samples[0]) < 1e-7f);
		QVERIFY(std::abs(opened.samples[1] - 0.25f * gain) < 1e-5f);
		QVERIFY(std::abs(opened.samples[2] - 0.5f * gain) < 1e-5f);
		QVERIFY(std::abs(opened.samples[3] + 0.25f * gain) < 1e-5f);
		const auto sourceRms = std::sqrt((0.0 + 0.25 * 0.25 + 0.5 * 0.5 + 0.25 * 0.25) / 4.0);
		QVERIFY(std::abs(result.measurements.sourceRms - sourceRms) < 1e-12);
		QVERIFY(std::abs(result.measurements.rms - sourceRms * gain) < 1e-5);
		QVERIFY(std::abs(result.measurements.sourcePeak - 0.5) < 1e-12);
		QVERIFY(result.measurements.peak < 1.0);
		QVERIFY(!result.measurements.overrange);
	}

	void compRenderCrossfadesTwoPreservedSourceSelections()
	{
		const auto sourceHash = fileHash(m_sourcePath);
		const auto secondBytes = sourceWave({0, 4096, -16384, 8192});
		const auto secondPath = m_audioRoot->filePath(QStringLiteral("take-two.wav"));
		QFile secondFile(secondPath);
		QVERIFY(secondFile.open(QIODevice::WriteOnly | QIODevice::NewOnly));
		QCOMPARE(secondFile.write(secondBytes), qint64(secondBytes.size()));
		secondFile.close();
		std::unique_ptr<SampleClip> second(new SampleClip(m_track.get()));
		QVERIFY(second);
		auto secondImport = AudioImportPolicy::configured().prepare(secondPath);
		QCOMPARE(secondImport.code, QStringLiteral("ok"));
		QCOMPARE(second->adoptAuthorizedAudio(std::move(secondImport.audio)), QStringLiteral("ok"));
		const auto secondSourceHash = fileHash(secondPath);
		AiBridgeProjectContext ids;
		const auto secondId = ids.idForClip(second.get());
		const auto secondState = AiBridgeAudioContext{}.clip(secondId);
		QVERIFY(secondState.value);

		auto compRequest = request();
		compRequest.selections.push_back({secondId, secondState.value->stateHash,
			VocalEditSourceBasis::SourcePcm, {0, 4}});
		compRequest.input = VocalCompRecipe{
			{{0, {0, 4}, {0, 4}}, {1, {0, 4}, {2, 6}}},
			{{0, 1, 2, VocalCrossfadeLaw::EqualPower}}};
		compRequest.operations.clear();
		const auto plan = AiBridgeVocalAssetActions::plan(compRequest, limits());
		QCOMPARE(plan.code, QStringLiteral("ok"));
		QVERIFY(plan.value);
		const auto estimate = AiBridgeVocalAssetActions::dryRun(*plan.value);
		QCOMPARE(estimate.code, QStringLiteral("ok"));
		QCOMPARE(estimate.outputFrames, quint64(6));
		const auto directPlan = AiBridgeVocalAssetActions::plan(request(), limits());
		QVERIFY(directPlan.value);
		const auto directEstimate = AiBridgeVocalAssetActions::dryRun(*directPlan.value);
		QCOMPARE(directEstimate.code, QStringLiteral("ok"));
		QVERIFY(estimate.jobBytes > directEstimate.jobBytes);
		const auto prepared = AiBridgeVocalAssetActions::prepareRender(*plan.value);
		QVERIFY(prepared.value);
		std::atomic_bool cancelled{false};
		const auto rendered = std::async(std::launch::async, [&] {
			return AiBridgeVocalAssetActions::render(*prepared.value, cancelled, {});
		}).get();
		QCOMPARE(rendered.code, QStringLiteral("ok"));
		QVERIFY(rendered.variant);
		QCOMPARE(rendered.variant->frames, quint64(6));
		QCOMPARE(fileHash(m_sourcePath), sourceHash);
		QCOMPARE(fileHash(secondPath), secondSourceHash);

		ManagedVocalAssetStore store(prepared.value->assetRoot);
		const auto opened = store.openVerified(*rendered.variant);
		QCOMPARE(opened.code, QStringLiteral("ok"));
		QCOMPARE(opened.samples.size(), size_t(6));
		const auto firstAngle = std::numbers::pi_v<double> / 8.0;
		const auto secondAngle = 3.0 * std::numbers::pi_v<double> / 8.0;
		QVERIFY(std::abs(opened.samples[2] - float(0.5 * std::cos(firstAngle))) < 1e-6f);
		QVERIFY(std::abs(opened.samples[3] - float(-0.25 * std::cos(secondAngle) + 0.125 * std::sin(secondAngle))) < 1e-6f);
		const auto recipe = opened.manifest->recipe;
		QCOMPARE(recipe.value(QStringLiteral("input_kind")).toString(), QStringLiteral("comp"));
		QCOMPARE(recipe.value(QStringLiteral("comp")).toObject()
			.value(QStringLiteral("segments")).toArray().size(), 2);
		QCOMPARE(opened.manifest->realizedFrameMaps.size(), 2);
	}

	void compBlockUsesExplicitOneFrameCrossfadeLawsAndStereoSources()
	{
		const std::vector<SampleFrame> left{SampleFrame(0.2f, 0.4f), SampleFrame(0.8f, 0.6f)};
		const std::vector<SampleFrame> right{SampleFrame(0.4f, -0.2f), SampleFrame(-0.6f, 0.8f)};
		const std::array<VocalCompSourceView, 2> sources{{{10, left}, {0, right}}};
		VocalCompRecipe recipe{{{0, {10, 12}, {0, 2}}, {1, {0, 2}, {1, 3}}},
			{{0, 1, 1, VocalCrossfadeLaw::LinearAmplitude}}};
		std::array<SampleFrame, 3> output{};
		QCOMPARE(AiBridgeVocalComp::process(recipe, sources, 0, output), QStringLiteral("ok"));
		QCOMPARE(output[0].left(), 0.2f);
		QVERIFY(std::abs(output[1].left() - 0.6f) < 1e-6f);
		QVERIFY(std::abs(output[1].right() - 0.2f) < 1e-6f);
		QCOMPARE(output[2].left(), -0.6f);

		recipe.boundaries.front().law = VocalCrossfadeLaw::EqualPower;
		QCOMPARE(AiBridgeVocalComp::process(recipe, sources, 1, std::span<SampleFrame>(output).subspan(1, 1)),
			QStringLiteral("ok"));
		const auto weight = std::sqrt(0.5f);
		QVERIFY(std::abs(output[1].left() - (0.8f + 0.4f) * weight) < 1e-6f);
		QVERIFY(std::abs(output[1].right() - (0.6f - 0.2f) * weight) < 1e-6f);
	}

	void renderPreflightRejectsTamperedResolvedTimingMap()
	{
		auto timingRequest = request();
		timingRequest.operations = {VocalTimingMap{{{0, 0, {}}, {4, 4, {}}}}};
		const auto plan = AiBridgeVocalAssetActions::plan(timingRequest, limits());
		QCOMPARE(plan.code, QStringLiteral("ok"));
		QVERIFY(plan.value);
		QCOMPARE(plan.value->plan.resolvedAnchors.size(), size_t(2));
		auto tampered = *plan.value;
		++tampered.plan.resolvedAnchors.back().outputFrame;
		QCOMPARE(AiBridgeVocalAssetActions::dryRun(tampered).code, QStringLiteral("plan_stale"));
	}

	void timingRenderPublishesExactAnchorsWithoutTouchingTheSourceWave()
	{
		ConfigManager::inst()->setValue(QStringLiteral("aibridge"), QStringLiteral("audio_import_max_file_bytes"),
			QStringLiteral("1000000"));
		ConfigManager::inst()->setValue(QStringLiteral("aibridge"), QStringLiteral("audio_import_max_decoded_bytes"),
			QStringLiteral("1000000"));
		QCOMPARE(AudioImportPolicy::configured().refresh(), QStringLiteral("ok"));
		const auto timingPath = m_audioRoot->filePath(QStringLiteral("timing-impulse.wav"));
		const auto sourceBytes = timingImpulseWave();
		QFile sourceFile(timingPath);
		QVERIFY(sourceFile.open(QIODevice::WriteOnly | QIODevice::NewOnly));
		QCOMPARE(sourceFile.write(sourceBytes), qint64(sourceBytes.size()));
		sourceFile.close();
		const auto sourceHash = fileHash(timingPath);

		std::unique_ptr<SampleClip> clip(new SampleClip(m_track.get()));
		auto imported = AudioImportPolicy::configured().prepare(timingPath);
		QCOMPARE(imported.code, QStringLiteral("ok"));
		QCOMPARE(clip->adoptAuthorizedAudio(std::move(imported.audio)), QStringLiteral("ok"));
		AiBridgeProjectContext ids;
		const auto clipId = ids.idForClip(clip.get());
		const auto state = AiBridgeAudioContext{}.clip(clipId);
		QVERIFY(state.value);
		const auto currentWorld = world();
		VocalEditRequest timingRequest;
		timingRequest.expectedProjectGeneration = ids.projectGeneration();
		timingRequest.expectedWorldHash = vocalEditWorldHash(currentWorld);
		timingRequest.selections.push_back({clipId, state.value->stateHash,
			VocalEditSourceBasis::SourcePcm, {0, 48000}});
		timingRequest.input = VocalDirectInput{0};
		timingRequest.operations = {
			VocalGainEnvelope{{{0, -6.0}, {48000, -6.0}}},
			VocalTimingMap{{{0, 0, {}}, {24000, 26400, {}, 840, VocalCrossfadeLaw::LinearAmplitude},
				{48000, 56400, {}}}}};

		const auto plan = AiBridgeVocalAssetActions::plan(timingRequest, limits());
		QCOMPARE(plan.code, QStringLiteral("ok"));
		QVERIFY(plan.value);
		QCOMPARE(plan.value->plan.resolvedAnchors.size(), size_t(3));
		const auto estimate = AiBridgeVocalAssetActions::dryRun(*plan.value);
		QCOMPARE(estimate.code, QStringLiteral("ok"));
		QCOMPARE(estimate.renderInputFrames, quint64(48000));
		QCOMPARE(estimate.outputFrames, quint64(56400));
		QVERIFY(estimate.jobBytes > 8ULL * 1024 * 1024);
		const auto prepared = AiBridgeVocalAssetActions::prepareRender(*plan.value);
		QCOMPARE(prepared.code, QStringLiteral("ok"));
		QVERIFY(prepared.value);
		std::atomic_bool cancelled{false};
		const auto rendered = std::async(std::launch::async, [&] {
			return AiBridgeVocalAssetActions::render(*prepared.value, cancelled, {});
		}).get();
		QCOMPARE(rendered.code, QStringLiteral("ok"));
		QVERIFY(rendered.variant);
		QCOMPARE(rendered.variant->frames, quint64(56400));
		QCOMPARE(fileHash(timingPath), sourceHash);

		ManagedVocalAssetStore store(prepared.value->assetRoot);
		const auto opened = store.openVerified(*rendered.variant);
		QCOMPARE(opened.code, QStringLiteral("ok"));
		QCOMPARE(opened.manifest->rendererId, QStringLiteral("lmms-gain-fade-signalsmith-stretch"));
		QCOMPARE(opened.manifest->rendererOptions.value(QStringLiteral("gain_interpolation")).toString(),
			QStringLiteral("linear_db"));
		const auto operationRecipe = opened.manifest->recipe.value(QStringLiteral("operations")).toArray();
		QCOMPARE(operationRecipe.size(), 2);
		const auto timingRecipe = operationRecipe.at(1).toObject();
		QCOMPARE(timingRecipe.value(QStringLiteral("kind")).toString(), QStringLiteral("timing_map"));
		QCOMPARE(opened.manifest->realizedFrameMaps.size(), 4);
		QVERIFY(opened.manifest->media.rms
			< opened.manifest->realizedFrameMaps.at(0).toObject()
				.value(QStringLiteral("source_measurements")).toObject().value(QStringLiteral("rms")).toDouble());
		const auto peak = std::max_element(opened.samples.begin(), opened.samples.end(), [](float a, float b) {
			return std::abs(a) < std::abs(b);
		});
		const auto peakFrame = qint64(std::distance(opened.samples.begin(), peak));
		QVERIFY(std::abs(peakFrame - 26400) <= 1);
	}

	void renderDryRunRejectsTimingSegmentsWithoutBackendContext()
	{
		auto timingRequest = request();
		timingRequest.operations = {VocalTimingMap{{{0, 0, {}}, {4, 5, {}}}}};
		const auto plan = AiBridgeVocalAssetActions::plan(timingRequest, limits());
		QCOMPARE(plan.code, QStringLiteral("ok"));
		QVERIFY(plan.value);
		QCOMPARE(AiBridgeVocalAssetActions::dryRun(*plan.value).code,
			QStringLiteral("stretch_segment_too_short"));
	}

	void renderUsesImmutableRecordedBufferSnapshotWithoutImportRoots()
	{
		auto plan = AiBridgeVocalAssetActions::plan(request(), limits());
		QVERIFY(plan.value);
		auto ticket = *plan.value;
		auto& source = ticket.sources.sources.front();
		source.roots.clear();
		source.recordedTakeProjectId = uuid();
		source.recordedTakeId = uuid();
		source.reference = QStringLiteral("recorded_take:") + source.recordedTakeProjectId
			+ QLatin1Char('/') + source.recordedTakeId;
		source.authorizedSourceBuffer = m_clip->sample().buffer();
		QVERIFY(source.authorizedSourceBuffer);
		source.authorizedSourceBufferRevision = source.authorizedSourceBuffer->contentRevision();
		source.limits.maxFileBytes = source.expectedInfo.fileBytes;
		source.limits.maxDecodedBytes = source.expectedInfo.decodedBytes;
		ticket.sources.inputHash = vocalInputHash(ticket.sources);
		QVERIFY(!ticket.sources.inputHash.isEmpty());
		const auto estimate = AiBridgeVocalAssetActions::dryRun(ticket);
		QCOMPARE(estimate.code, QStringLiteral("ok"));

		auto root = ManagedVocalAssetRoot::authorizeConfiguredRoot();
		QVERIFY(root.root);
		const auto identity = Engine::getSong()->recordingProjectIdentity().cachedIdentity();
		VocalEditRenderInput renderInput{ticket, root.root, identity.id};
		std::atomic_bool cancelled{false};
		const auto rendered = std::async(std::launch::async, [&] {
			return AiBridgeVocalAssetActions::render(renderInput, cancelled, {});
		}).get();
		QCOMPARE(rendered.code, QStringLiteral("ok"));
		QVERIFY(rendered.variant);
		ManagedVocalAssetStore store(root.root);
		const auto opened = store.openVerified(*rendered.variant);
		QCOMPARE(opened.code, QStringLiteral("ok"));
		QCOMPARE(opened.manifest->sources.size(), size_t(1));
		QCOMPARE(opened.manifest->sources[0].recordedTakeProjectId, source.recordedTakeProjectId);
		QCOMPARE(opened.manifest->sources[0].recordedTakeId, source.recordedTakeId);
	}

	void plansAndRendersARealSealedTakeWithImportRootsEmpty()
	{
		auto recordingRoot = std::make_unique<QTemporaryDir>(
			QCoreApplication::applicationDirPath() + QStringLiteral("/vocal-recording-XXXXXX"));
		QVERIFY(recordingRoot->isValid());
		auto* recordingDirectory = recordingRoot.get();
		m_recordingRoots.push_back(std::move(recordingRoot));
		auto* song = Engine::getSong();
		song->recordingProjectIdentity().resetForNewProject();
		const auto selected = Engine::recordingServiceHost()->selectRecordingRoot(
			recordingDirectory->path(), song->recordingProjectIdentity());
		QVERIFY(selected.ok);
		const auto takeId = uuid();
		QVERIFY(writeSyntheticRecordedTake(*Engine::recordingServiceHost(), selected.project, takeId));
		const auto takePath = recordingDirectory->filePath(selected.project.id + QStringLiteral("/take-")
			+ takeId + QStringLiteral(".wav"));
		const auto takeHash = fileHash(takePath);
		QVERIFY(!takeHash.isEmpty());
		QVERIFY(song->saveProjectFile(recordingDirectory->filePath(QStringLiteral("source-identity.mmp"))));

		auto opened = Engine::recordingServiceHost()->openSealedTake(selected.project, takeId);
		QCOMPARE(opened.code, QStringLiteral("ok"));
		QVERIFY(opened.source);
		auto reference = recording::RecordedTakeSource::fromFacts(opened.source->facts());
		QVERIFY(reference);
		reference->trackId = uuid();
		reference->clipId = uuid();
		reference->placementRevision = 1;
		QDomDocument document;
		auto node = document.createElement(QStringLiteral("sampleclip"));
		document.appendChild(node);
		node.setAttribute(QStringLiteral("pos"), QStringLiteral("0"));
		node.setAttribute(QStringLiteral("len"), QStringLiteral("192"));
		node.setAttribute(QStringLiteral("off"), QStringLiteral("0"));
		node.setAttribute(QStringLiteral("autoresize"), QStringLiteral("0"));
		node.appendChild(recording::RecordedTakeSource::writeReference(document, *reference));
		ConfigManager::inst()->setValue(QStringLiteral("aibridge"), QStringLiteral("audio_import_roots"),
			QStringLiteral("[]"));
		QCOMPARE(AudioImportPolicy::configured().refresh(), QStringLiteral("import_path_denied"));

		SampleTrack recordedTrack(song);
		SampleClip recordedClip(&recordedTrack);
		recordedClip.loadSettings(node);
		QCOMPARE(recordedClip.sourceLoadError(), QString{});
		QVERIFY(recordedClip.isRecordedTake());
		QVERIFY(recordedClip.recordedTakeSourceCurrent());
		QCOMPARE(recordedClip.sample().sampleSize(), size_t(4));

		AiBridgeProjectContext ids;
		const auto clipId = ids.idForClip(&recordedClip);
		const auto state = AiBridgeAudioContext{}.clip(clipId);
		QVERIFY(state.value);
		VocalAnalyzeRequest sourceRequest;
		sourceRequest.selections.push_back({clipId, state.value->stateHash, {0, 4}});
		QCOMPARE(captureVocalInputs(sourceRequest).code, QStringLiteral("ok"));
		VocalEditRequest edit;
		edit.expectedProjectGeneration = ids.projectGeneration();
		edit.expectedWorldHash = vocalEditWorldHash(world());
		edit.selections.push_back({clipId, state.value->stateHash, VocalEditSourceBasis::SourcePcm, {0, 4}});
		edit.input = VocalDirectInput{0};
		edit.operations.push_back(VocalGainEnvelope{{{0, -3.0}, {4, -3.0}}});
		const auto plan = AiBridgeVocalAssetActions::plan(edit, limits());
		QCOMPARE(plan.code, QStringLiteral("ok"));
		QVERIFY(plan.value);
		QCOMPARE(plan.value->sources.sources.front().roots.size(), 0);
		QVERIFY(!plan.value->sources.sources.front().expectedIdentity.isEmpty());
		QCOMPARE(plan.value->sources.sources.front().recordedTakeProjectId, selected.project.id);
		QCOMPARE(plan.value->sources.sources.front().recordedTakeId, takeId);
		const auto prepared = AiBridgeVocalAssetActions::prepareRender(*plan.value);
		QCOMPARE(prepared.code, QStringLiteral("ok"));
		QVERIFY(prepared.value);
		std::atomic_bool cancelled{false};
		const auto rendered = std::async(std::launch::async, [&] {
			return AiBridgeVocalAssetActions::render(*prepared.value, cancelled, {});
		}).get();
		QCOMPARE(rendered.code, QStringLiteral("ok"));
		QVERIFY(rendered.variant);
		ManagedVocalAssetStore store(prepared.value->assetRoot);
		const auto openedVariant = store.openVerified(*rendered.variant);
		QCOMPARE(openedVariant.code, QStringLiteral("ok"));
		QCOMPARE(openedVariant.manifest->sources.front().recordedTakeId, takeId);
		const auto originalTrack = captureVocalTrack(state.value->state.trackId);
		QVERIFY(originalTrack.value);
		VocalVariantApplyRequest replace;
		replace.variantId = rendered.variant->assetId;
		replace.clipId = clipId;
		replace.mode = QStringLiteral("replace");
		replace.expectedProjectGeneration = ids.projectGeneration();
		replace.expectedWorldHash = edit.expectedWorldHash;
		replace.expectedAudioStateHash = state.value->stateHash;
		replace.expectedTrackStateHash = originalTrack.value->hash;
		VocalVariantApplyResult replaced;
		for (int attempt = 0; attempt < 250; ++attempt)
		{
			replaced = AiBridgeVocalVariantAttachment::apply(replace);
			if (replaced.code != QStringLiteral("asset_loading")) break;
			QTest::qWait(20);
		}
		QCOMPARE(replaced.code, QStringLiteral("ok"));
		QVERIFY(recordedClip.isRecordedTake());
		QCOMPARE(recordedClip.recordedTakeReference()->takeId, takeId);
		const auto replacedState = AiBridgeAudioContext{}.clip(clipId);
		QVERIFY(replacedState.value);
		const auto replacedTrack = captureVocalTrack(state.value->state.trackId);
		QVERIFY(replacedTrack.value);
		VocalVariantApplyRequest revert;
		revert.variantId = rendered.variant->assetId;
		revert.clipId = clipId;
		revert.mode = QStringLiteral("revert");
		revert.expectedProjectGeneration = ids.projectGeneration();
		revert.expectedWorldHash = edit.expectedWorldHash;
		revert.expectedAudioStateHash = replacedState.value->stateHash;
		revert.expectedTrackStateHash = replacedTrack.value->hash;
		QCOMPARE(AiBridgeVocalVariantAttachment::apply(revert).code, QStringLiteral("ok"));
		QCOMPARE(AiBridgeAudioContext{}.clip(clipId).value->stateHash, state.value->stateHash);
		QCOMPARE(fileHash(takePath), takeHash);
	}

	void cancellationRemovesOnlyTheIncompleteRenderStage()
	{
		const auto plan = AiBridgeVocalAssetActions::plan(request(), limits());
		QVERIFY(plan.value);
		const auto prepared = AiBridgeVocalAssetActions::prepareRender(*plan.value);
		QVERIFY(prepared.value);
		std::atomic_bool cancelled{false};
		const auto result = std::async(std::launch::async, [&] {
			return AiBridgeVocalAssetActions::render(*prepared.value, cancelled,
				[&cancelled](VocalJobPhase phase) {
					if (phase == VocalJobPhase::Rendering) cancelled.store(true, std::memory_order_release);
				});
		}).get();
		QCOMPARE(result.code, QStringLiteral("cancelled"));
		const auto root = ManagedVocalAssetRoot::authorizeConfiguredRoot();
		QVERIFY(root.root);
		ManagedVocalAssetStore store(root.root);
		const auto recovery = store.scanRecovery();
		QCOMPARE(recovery.code, QStringLiteral("ok"));
		QCOMPARE(recovery.totalItems, size_t(0));
	}

	void refusesRenderWithoutPersistentProjectIdentity()
	{
		const auto plan = AiBridgeVocalAssetActions::plan(request(), limits());
		QVERIFY(plan.value);
		Engine::getSong()->recordingProjectIdentity().resetForNewProject();
		QCOMPARE(AiBridgeVocalAssetActions::prepareRender(*plan.value).code,
			QStringLiteral("project_identity_unverified"));
	}

	void admitsFullDurationStereoRenderWithinSeparateMemoryAndStagingCaps()
	{
		constexpr qint64 frames = 48000LL * 120;
		const QString clipId = QStringLiteral("clip-full-length");
		const QString sourceHash(64, QChar('a'));
		const VocalEditWorld renderWorld{120, 48000, 48, false, 4, 4};
		VocalEditRequest edit;
		edit.expectedProjectGeneration = 7;
		edit.expectedWorldHash = vocalEditWorldHash(renderWorld);
		edit.selections.push_back({clipId, sourceHash, VocalEditSourceBasis::SourcePcm, {0, frames}});
		edit.input = VocalDirectInput{0};
		edit.operations.push_back(VocalGainEnvelope{{{0, 0.0}, {frames, 0.0}}});
		VocalEditLimits editLimits = limits();
		VocalEditPlanningSnapshot planning;
		planning.generation = 7;
		planning.worldHash = edit.expectedWorldHash;
		planning.world = renderWorld;
		planning.sources.push_back({clipId, sourceHash, quint64(frames), 48000, 2, true});
		const auto planned = AiBridgeVocalEditPlanner::plan(edit, planning, editLimits);
		QCOMPARE(planned.code, QStringLiteral("ok"));
		QVERIFY(planned.value);

		VocalEditPlanTicket ticket;
		ticket.request = edit;
		ticket.planning = planning;
		ticket.plan = *planned.value;
		ticket.limits = editLimits;
		ticket.sources.generation = 7;
		ticket.sources.policyRevision = 1;
		ticket.sources.request.selections.push_back({clipId, sourceHash, {0, frames}});
		VocalSourceTicket source;
		source.clipId = clipId;
		source.reference = QStringLiteral("C:/Authorized/lead.wav");
		source.roots = QStringList{QStringLiteral("C:/Authorized")};
		source.policyRevision = 1;
		source.limits = {20000000, 100000000};
		source.expectedInfo = {quint64(frames), quint64(11'520'044), quint64(frames) * 8,
			2, 48000, QByteArray(32, 'x')};
		source.expectedIdentity = QByteArray(16, 'i');
		source.sourceRevision = QString(64, QChar('b'));
		source.sourceStatus = QStringLiteral("available_unloaded");
	ticket.sources.sources.push_back(source);
		ticket.sources.inputHash = vocalInputHash(ticket.sources);
		QVERIFY(!ticket.sources.inputHash.isEmpty());

		const auto estimate = AiBridgeVocalAssetActions::dryRun(ticket);
		QCOMPARE(estimate.code, QStringLiteral("ok"));
		QCOMPARE(estimate.outputFrames, quint64(frames));
		QCOMPARE(estimate.stagedBytes, quint64(frames) * 2 * sizeof(float) + 56);
		QVERIFY(estimate.sessionBytes < VocalLimits::sessionBytes);
		QVERIFY(estimate.jobBytes < VocalLimits::jobBytes);
	}

	void renderAndAnalysisShareOneWorkerAndRetainChargeUntilWorkerExit()
	{
		const auto plan = AiBridgeVocalAssetActions::plan(request(), limits());
		QVERIFY(plan.value);
		const auto prepared = AiBridgeVocalAssetActions::prepareRender(*plan.value);
		QVERIFY(prepared.value);
		RenderGate gate;
		const auto runner = [&gate](const VocalEditRenderInput&, const std::atomic_bool& cancelled,
			const VocalPhaseCallback&) { return gate.run(cancelled); };
		AiBridgeVocalJobs jobs(nullptr, runVocalAnalysis, {}, runner);
		const auto unblock = qScopeGuard([&] { gate.release(); });
		const auto render = jobs.startRender(*prepared.value);
		QCOMPARE(render.code, QStringLiteral("ok"));
		QTRY_VERIFY(gate.entered.load(std::memory_order_acquire));
		QVERIFY(jobs.chargedWorkerBytes() > 0);
		QVERIFY(jobs.chargedStagedBytes() > 0);
		VocalAnalyzeRequest analysis;
		for (const auto& selection : prepared.value->ticket.request.selections)
		{
			analysis.selections.push_back({selection.clipId, selection.expectedAudioHash,
				{selection.sourceFrames.inFrame, selection.sourceFrames.outFrame}});
		}
		QCOMPARE(jobs.start(analysis).code, QStringLiteral("job_busy"));
		const auto cancelled = jobs.cancel(render.jobId);
		QVERIFY(cancelled.cancelApplied);
		QVERIFY(jobs.chargedWorkerBytes() > 0);
		QVERIFY(jobs.chargedStagedBytes() > 0);
		QCOMPARE(jobs.start(analysis).code, QStringLiteral("job_busy"));
		gate.release();
		QTRY_COMPARE(jobs.job(render.jobId).value->state, VocalJobState::Cancelled);
		QTRY_COMPARE(jobs.chargedWorkerBytes(), quint64(0));
		QTRY_COMPARE(jobs.chargedStagedBytes(), quint64(0));
		QCOMPARE(jobs.release(render.jobId).code, QStringLiteral("ok"));
		const auto started = jobs.start(analysis);
		QCOMPARE(started.code, QStringLiteral("ok"));
		QTRY_COMPARE(jobs.job(started.jobId).value->state, VocalJobState::Succeeded);
	}

	void renderInvalidationRetainsWorkerChargeUntilWorkerExit()
	{
		for (const auto projectChange : {false, true})
		{
			const auto plan = AiBridgeVocalAssetActions::plan(request(), limits());
			QVERIFY(plan.value);
			const auto prepared = AiBridgeVocalAssetActions::prepareRender(*plan.value);
			QVERIFY(prepared.value);
			RenderGate gate;
			const auto runner = [&gate](const VocalEditRenderInput&, const std::atomic_bool& cancelled,
				const VocalPhaseCallback&) { return gate.run(cancelled); };
			AiBridgeVocalJobs jobs(nullptr, runVocalAnalysis, {}, runner);
			const auto unblock = qScopeGuard([&] { gate.release(); });
			const auto started = jobs.startRender(*prepared.value);
			QCOMPARE(started.code, QStringLiteral("ok"));
			QTRY_VERIFY(gate.entered.load(std::memory_order_acquire));
			if (projectChange)
			{
				emit Engine::getSong()->projectAboutToChange();
			}
			else
			{
				ConfigManager::inst()->setValue(QStringLiteral("aibridge"),
					QStringLiteral("audio_import_max_file_bytes"), QStringLiteral("9999"));
			}
			QCOMPARE(jobs.job(started.jobId).value->state, VocalJobState::Invalidated);
			QVERIFY(jobs.chargedWorkerBytes() > 0);
			QVERIFY(jobs.chargedStagedBytes() > 0);
			gate.release();
			QTRY_COMPARE(jobs.chargedWorkerBytes(), quint64(0));
			QTRY_COMPARE(jobs.chargedStagedBytes(), quint64(0));
			QCOMPARE(jobs.job(started.jobId).value->state, VocalJobState::Invalidated);
			QCOMPARE(jobs.release(started.jobId).code, QStringLiteral("ok"));
			if (!projectChange)
			{
				ConfigManager::inst()->setValue(QStringLiteral("aibridge"),
					QStringLiteral("audio_import_max_file_bytes"), QStringLiteral("10000"));
			}
		}
	}

	void renderAdmissionObserverCanShutdownWithoutReportingSuccess()
	{
		const auto plan = AiBridgeVocalAssetActions::plan(request(), limits());
		QVERIFY(plan.value);
		const auto prepared = AiBridgeVocalAssetActions::prepareRender(*plan.value);
		QVERIFY(prepared.value);
		AiBridgeVocalJobs jobs(nullptr, runVocalAnalysis, {},
			[](const VocalEditRenderInput&, const std::atomic_bool&, const VocalPhaseCallback&) {
				return VocalVariantRenderResult{QStringLiteral("cancelled"), {}, {}, 0};
			});
		connect(&jobs, &AiBridgeVocalJobs::jobsChanged, this, [&jobs] { jobs.shutdown(); });
		const auto started = jobs.startRender(*prepared.value);
		QCOMPARE(started.code, QStringLiteral("job_invalidated"));
		QVERIFY(started.jobId.isEmpty());
		QCOMPARE(jobs.chargedSessionBytes(), quint64(0));
		QCOMPARE(jobs.chargedWorkerBytes(), quint64(0));
		QCOMPARE(jobs.chargedStagedBytes(), quint64(0));
	}

	void projectOrPolicyInvalidationCancelsBeforePublishingAndRemovesOwnedStage()
	{
		for (const auto invalidateProject : {false, true})
		{
			const auto sourceHash = fileHash(m_sourcePath);
			const auto plan = AiBridgeVocalAssetActions::plan(request(), limits());
			QVERIFY(plan.value);
			const auto prepared = AiBridgeVocalAssetActions::prepareRender(*plan.value);
			QVERIFY(prepared.value);
			std::mutex mutex;
			std::condition_variable condition;
			std::atomic_bool publishing{false};
			bool open = false;
			const auto runner = [&](const VocalEditRenderInput& renderInput,
				const std::atomic_bool& cancelled, const VocalPhaseCallback& phase) {
				return AiBridgeVocalAssetActions::render(renderInput, cancelled,
					[&](VocalJobPhase current) {
						phase(current);
						if (current == VocalJobPhase::Publishing)
						{
							std::unique_lock lock(mutex);
						publishing.store(true, std::memory_order_release);
						condition.notify_all();
						condition.wait(lock, [&] { return open; });
						}
					});
			};
			AiBridgeVocalJobs jobs(nullptr, runVocalAnalysis, {}, runner);
			const auto releaseGate = qScopeGuard([&] {
				std::lock_guard lock(mutex);
				open = true;
				condition.notify_all();
			});
			const auto started = jobs.startRender(*prepared.value);
			QCOMPARE(started.code, QStringLiteral("ok"));
			QTRY_VERIFY(publishing.load(std::memory_order_acquire));
			if (invalidateProject)
			{
				emit Engine::getSong()->projectAboutToChange();
			}
			else
			{
				ConfigManager::inst()->setValue(QStringLiteral("aibridge"),
					QStringLiteral("audio_import_max_file_bytes"), QStringLiteral("9998"));
				QCOMPARE(jobs.job(started.jobId).value->state, VocalJobState::Invalidated);
			}
			{
				std::lock_guard lock(mutex);
				open = true;
			}
			condition.notify_all();
			QTRY_COMPARE(jobs.job(started.jobId).value->state, VocalJobState::Invalidated);
			QTRY_COMPARE(jobs.chargedWorkerBytes(), quint64(0));
			QTRY_COMPARE(jobs.chargedStagedBytes(), quint64(0));
			ManagedVocalAssetStore store(prepared.value->assetRoot);
			const auto recovery = store.scanRecovery();
			QCOMPARE(recovery.code, QStringLiteral("ok"));
			QCOMPARE(recovery.totalItems, size_t(0));
			QCOMPARE(fileHash(m_sourcePath), sourceHash);
			QCOMPARE(jobs.release(started.jobId).code, QStringLiteral("ok"));
			if (!invalidateProject)
			{
				ConfigManager::inst()->setValue(QStringLiteral("aibridge"),
					QStringLiteral("audio_import_max_file_bytes"), QStringLiteral("10000"));
			}
		}
	}

	void defaultRenderJobPublishesDetachedVariantAndRetainsOnlyItsBoundedResult()
	{
		const auto sourceHash = fileHash(m_sourcePath);
		const auto plan = AiBridgeVocalAssetActions::plan(request(), limits());
		QVERIFY(plan.value);
		const auto prepared = AiBridgeVocalAssetActions::prepareRender(*plan.value);
		QVERIFY(prepared.value);
		AiBridgeVocalJobs jobs;
		const auto started = jobs.startRender(*prepared.value);
		QCOMPARE(started.code, QStringLiteral("ok"));
		QTRY_COMPARE(jobs.job(started.jobId).value->state, VocalJobState::Succeeded);
		const auto state = jobs.job(started.jobId);
		QVERIFY(state.value);
		QCOMPARE(state.value->kind, VocalJobKind::Render);
		QVERIFY(!state.value->variantId.isEmpty());
		QCOMPARE(jobs.chargedWorkerBytes(), quint64(0));
		QCOMPARE(jobs.chargedStagedBytes(), quint64(0));
		QVERIFY(jobs.chargedSessionBytes() > 0);

		const auto rendered = jobs.renderedVariant(started.jobId);
		QCOMPARE(rendered.code, QStringLiteral("ok"));
		QVERIFY(rendered.variant);
		QCOMPARE(rendered.variant->assetId, state.value->variantId);
		ManagedVocalAssetStore store(prepared.value->assetRoot);
		const auto opened = store.openVerified(*rendered.variant);
		QCOMPARE(opened.code, QStringLiteral("ok"));
		QCOMPARE(opened.manifest->projectId, prepared.value->projectId);
		QCOMPARE(fileHash(m_sourcePath), sourceHash);
		QCOMPARE(jobs.release(started.jobId).code, QStringLiteral("ok"));
		QCOMPARE(jobs.chargedSessionBytes(), quint64(0));
		QCOMPARE(jobs.renderedVariant(started.jobId).code, QStringLiteral("stale_id"));
	}

	void applyRejectsByteIdenticalAudioFromDifferentFileIdentity()
	{
		const auto sourceHash = fileHash(m_sourcePath);
		const auto duplicatePath = m_audioRoot->filePath(QStringLiteral("identical-copy.wav"));
		QVERIFY(QFile::copy(m_sourcePath, duplicatePath));
		SampleTrack duplicateTrack(Engine::getSong());
		SampleClip duplicateClip(&duplicateTrack);
		auto duplicateImport = AudioImportPolicy::configured().prepare(duplicatePath);
		QCOMPARE(duplicateImport.code, QStringLiteral("ok"));
		QCOMPARE(duplicateClip.adoptAuthorizedAudio(std::move(duplicateImport.audio)), QStringLiteral("ok"));
		duplicateClip.setAutoResize(false);

		AiBridgeProjectContext ids;
		const auto duplicateClipId = ids.idForClip(&duplicateClip);
		const auto duplicateTrackId = ids.idForTrack(&duplicateTrack);
		const auto targetBefore = AiBridgeAudioContext{}.clip(duplicateClipId);
		const auto sourceBefore = AiBridgeAudioContext{}.clip(ids.idForClip(m_clip));
		QVERIFY(targetBefore.value && sourceBefore.value);
		QCOMPARE(targetBefore.value->state.sourceDigest, sourceBefore.value->state.sourceDigest);
		QVERIFY(targetBefore.value->state.sourceIdentity != sourceBefore.value->state.sourceIdentity);
		const auto targetTrackBefore = captureVocalTrack(duplicateTrackId);
		QVERIFY(targetTrackBefore.value);

		const auto plan = AiBridgeVocalAssetActions::plan(request(), limits());
		QCOMPARE(plan.code, QStringLiteral("ok"));
		QVERIFY(plan.value);
		const auto prepared = AiBridgeVocalAssetActions::prepareRender(*plan.value);
		QCOMPARE(prepared.code, QStringLiteral("ok"));
		QVERIFY(prepared.value);
		std::atomic_bool cancelled{false};
		const auto rendered = std::async(std::launch::async, [&] {
			return AiBridgeVocalAssetActions::render(*prepared.value, cancelled, {});
		}).get();
		QCOMPARE(rendered.code, QStringLiteral("ok"));
		QVERIFY(rendered.variant);

		VocalVariantApplyRequest apply;
		apply.variantId = rendered.variant->assetId;
		apply.clipId = duplicateClipId;
		apply.mode = QStringLiteral("replace");
		apply.expectedProjectGeneration = plan.value->plan.projectGeneration;
		apply.expectedWorldHash = plan.value->plan.worldHash;
		apply.expectedAudioStateHash = targetBefore.value->stateHash;
		apply.expectedTrackStateHash = targetTrackBefore.value->hash;
		VocalVariantApplyResult result;
		for (int attempt = 0; attempt < 250; ++attempt)
		{
			result = AiBridgeVocalVariantAttachment::apply(apply);
			if (result.code != QStringLiteral("asset_loading")) break;
			QTest::qWait(20);
		}
		QCOMPARE(result.code, QStringLiteral("source_changed"));
		const auto targetAfter = AiBridgeAudioContext{}.clip(duplicateClipId);
		QVERIFY(targetAfter.value);
		QCOMPARE(targetAfter.value->stateHash, targetBefore.value->stateHash);
		QVERIFY(duplicateClip.managedVocalVariantAssetId().isEmpty());
		QCOMPARE(fileHash(m_sourcePath), sourceHash);
		QCOMPARE(fileHash(duplicatePath), sourceHash);
	}

	void renderRejectsSourceReplacementBeforePublishing()
	{
		const auto originalHash = fileHash(m_sourcePath);
		QVERIFY(!originalHash.isEmpty());
		const auto plan = AiBridgeVocalAssetActions::plan(request(), limits());
		QCOMPARE(plan.code, QStringLiteral("ok"));
		QVERIFY(plan.value);
		const auto prepared = AiBridgeVocalAssetActions::prepareRender(*plan.value);
		QCOMPARE(prepared.code, QStringLiteral("ok"));
		QVERIFY(prepared.value);
		std::atomic_bool cancelled{false};
		std::atomic_bool sourceWasReplaced{false};
		const auto rendered = std::async(std::launch::async, [&] {
			return AiBridgeVocalAssetActions::render(*prepared.value, cancelled, [&](VocalJobPhase phase) {
				if (phase != VocalJobPhase::Publishing || sourceWasReplaced.exchange(true)) return;
				QFile source(m_sourcePath);
				if (!source.open(QIODevice::ReadWrite) || !source.seek(44))
				{
					sourceWasReplaced.store(false);
					return;
				}
			QByteArray changedSample(sizeof(qint16), 0);
			qToLittleEndian<qint16>(123, changedSample.data());
			if (source.write(changedSample) != qint64(changedSample.size()) || !source.flush())
				sourceWasReplaced.store(false);
			});
		}).get();
		QVERIFY(sourceWasReplaced.load());
		QCOMPARE(rendered.code, QStringLiteral("source_changed"));
		QVERIFY(!rendered.variant);
		ManagedVocalAssetStore store(prepared.value->assetRoot);
		const auto recovery = store.scanRecovery();
		QCOMPARE(recovery.code, QStringLiteral("ok"));
		QCOMPARE(recovery.totalItems, size_t(0));
		QVERIFY(fileHash(m_sourcePath) != originalHash);
	}

	void savedProjectWithUnpersistedRecordingIdentityCannotPublish()
	{
		auto* song = Engine::getSong();
		QVERIFY(!song->projectFileName().isEmpty());
		song->recordingProjectIdentity().resetForNewProject();
		QTemporaryDir recordingDirectory;
		QVERIFY(recordingDirectory.isValid());
		auto library = recording::RecordingLibrary::authorizeLocalRoot(recordingDirectory.path());
		QVERIFY(library.library);
		const auto identity = song->recordingProjectIdentity().getOrCreateForCurrentProject(*library.library);
		QCOMPARE(identity.code, QStringLiteral("ok"));
		QVERIFY(!song->recordingProjectIdentity().sourceIdentityTrusted());
		const auto plan = AiBridgeVocalAssetActions::plan(request(), limits());
		QCOMPARE(plan.code, QStringLiteral("ok"));
		QVERIFY(plan.value);
		QCOMPARE(AiBridgeVocalAssetActions::prepareRender(*plan.value).code,
			QStringLiteral("project_identity_unverified"));
	}

	void savedImportedProjectCanRenderWithoutRecordingLibrarySetup()
	{
		auto* song = Engine::getSong();
		song->recordingProjectIdentity().resetForNewProject();
		QVERIFY(song->recordingProjectIdentity().cachedIdentity().id.isEmpty());
		QTemporaryDir projectDirectory;
		QVERIFY(projectDirectory.isValid());
		const auto projectPath = projectDirectory.filePath(QStringLiteral("fresh-project.mmp"));
		QVERIFY(song->guiSaveProjectAs(projectPath));
		const auto originalHash = fileHash(m_sourcePath);
		const auto plan = AiBridgeVocalAssetActions::plan(request(), limits());
		QCOMPARE(plan.code, QStringLiteral("ok"));
		QVERIFY(plan.value);
		const auto prepared = AiBridgeVocalAssetActions::prepareRender(*plan.value);
		QCOMPARE(prepared.code, QStringLiteral("ok"));
		QVERIFY(prepared.value);
		std::atomic_bool cancelled{false};
		const auto rendered = std::async(std::launch::async, [&] {
			return AiBridgeVocalAssetActions::render(*prepared.value, cancelled, [](VocalJobPhase) {});
		}).get();
		QCOMPARE(rendered.code, QStringLiteral("ok"));
		QVERIFY(rendered.variant);
		QCOMPARE(fileHash(m_sourcePath), originalHash);
	}
};

QTEST_MAIN(AiBridgeVocalAssetActionsTest)
#include "AiBridgeVocalAssetActionsTest.moc"
