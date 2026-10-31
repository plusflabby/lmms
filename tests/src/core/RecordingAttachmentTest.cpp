#include "AudioDevice.h"
#include "AudioImportPolicy.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "ProjectJournal.h"
#include "RecordingAttachment.h"
#include "RecordingPlacement.h"
#include "RecordingServiceHost.h"
#include "RecordingWriter.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <QFile>
#include <QUuid>
#include <QSignalBlocker>
#include <QElapsedTimer>
#include <array>

using namespace lmms;
using namespace lmms::recording;

namespace lmms::recording {
class RecordingRecorderView
{
public:
	static ManualPlacementApproval approve(const QString& take, quint64 generation, quint64 revision, qint64 sourceIn, qint64 target)
	{
		ManualPlacementApproval value;
		value.takeId = take;
		value.expectedProjectGeneration = generation;
		value.placementRevision = revision;
		value.sourceInTicks = sourceIn;
		value.targetTick = target;
		return value;
	}
};
}

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

class NoDeviceDriver final : public RecordingInputDriver
{
public:
	int opens = 0;
	RecordingDriverResult initialize() noexcept override { return {QStringLiteral("ok"), true}; }
	RecordingInputDevicePage enumerateInputs(std::uint32_t, std::uint32_t) override
		{return {QStringLiteral("ok"), {{QStringLiteral("synthetic"), QStringLiteral("Synthetic WASAPI profile"), QStringLiteral("WASAPI"), 1}}, 1, false};}
	RecordingDriverResult openInputOnly(const SelectedInputProfile&, RecordingInputCallback, void*) noexcept override
	{++opens; return {QStringLiteral("device_access_forbidden_in_test"), false};}
	RecordingDriverResult start() noexcept override { return {QStringLiteral("device_access_forbidden_in_test"), false}; }
	RecordingDriverResult stop() noexcept override { return {QStringLiteral("ok"), true}; }
	RecordingDriverResult close() noexcept override { return {QStringLiteral("ok"), true}; }
	RecordingDriverResult terminate() noexcept override { return {QStringLiteral("ok"), true}; }
};

QString uuid()
{
	return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QByteArray fileHash(const QString& path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) return {};
	return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

bool waitForWriter(RecordingWriter& writer, WriterState state)
{
	QElapsedTimer timer;
	timer.start();
	while (timer.elapsed() < 5000)
	{
		if (writer.poll().state == state) return true;
		QTest::qWait(1);
	}
	return writer.poll().state == state;
}

bool writeTake(RecordingServiceHost& host, const ProjectIdentity& project, const QString& takeId, std::uint64_t frames)
{
	constexpr std::uint32_t rate = 48000;
	constexpr std::uint64_t maximum = std::uint64_t(rate) * 120;
	auto created = host.createUniqueTake({takeId, QStringLiteral("attachment test")}, rate, maximum);
	if (created.code != QStringLiteral("ok") || !created.sink) return false;
	RecordingRing ring{rate, RecordingRing::Capacity, maximum};
	TakeDescriptor descriptor;
	descriptor.projectId = project.id;
	descriptor.takeId = takeId;
	descriptor.label = QStringLiteral("attachment test");
	descriptor.epoch = {1, 1};
	descriptor.projectGeneration = 1;
	descriptor.maxRawFrames = maximum;
	descriptor.sampleRate = rate;
	RecordingWriter writer;
	if (!writer.begin(descriptor, std::move(created.sink), ring).ok || !waitForWriter(writer, WriterState::Ready)) return false;
	std::array<float, RecordingRing::MaximumCallback> block{};
	std::uint64_t at = 0;
	constexpr std::uint64_t sourceInFrame = 6240;
	while (at < frames)
	{
		const auto count = std::size_t(std::min<std::uint64_t>(block.size(), frames - at));
		QElapsedTimer spaceWait;
		spaceWait.start();
		while (ring.acceptedFrames() - writer.poll().removedFrames + count > ring.capacityFrames())
		{
			if (spaceWait.elapsed() > 5000) return false;
			QTest::qWait(1);
		}
		for (std::size_t i = 0; i < count; ++i)
		{
			const auto frame = at + i;
			block[i] = frame < sourceInFrame ? 0.75f : frame >= frames - 128 ? -0.25f : 0.2f;
		}
		if (ring.push(block.data(), count, 1, 0).acceptedFrames != count) return false;
		at += count;
	}
	InputSnapshot end;
	end.epoch = descriptor.epoch;
	end.receivedFrames = frames;
	end.acceptedFrames = frames;
	end.hardwareLossCountKnown = true;
	writer.requestInputClosed(descriptor.epoch, end);
	return waitForWriter(writer, WriterState::Sealed) && writer.joinAfterExit().ok;
}

class TestTransport final : public RecordingTransportControl
{
public:
	RecordingTransportSnapshot world;
	RecordingTransportSnapshot snapshot() const override { return world; }
	RecordingTransportResult seekSong(qint64) override { return {false, QStringLiteral("not_used"), {}}; }
	RecordingTransportResult playSong() override { return {false, QStringLiteral("not_used"), {}}; }
	RecordingTransportResult stopOwned(RecordingPlaybackOwnership) override { return {false, QStringLiteral("not_used"), {}}; }
};

SampleClip* findRecordedClip(Song* song, const QString& takeId, SampleTrack** owner = nullptr)
{
	for (auto* track : song->tracks())
	{
		auto* sampleTrack = dynamic_cast<SampleTrack*>(track);
		if (!sampleTrack) continue;
		for (int i = 0; i < sampleTrack->numOfClips(); ++i)
		{
			auto* clip = dynamic_cast<SampleClip*>(sampleTrack->getClip(i));
			if (clip && clip->recordedTakeReference() && clip->recordedTakeReference()->takeId == takeId)
			{
				if (owner) *owner = sampleTrack;
				return clip;
			}
		}
	}
	return nullptr;
}
}

class RecordingAttachmentTest : public QObject
{
	Q_OBJECT
	struct Fixture
	{
		ProjectIdentity project;
		QString takeId;
		QString root;
		QByteArray originalSha;
		std::unique_ptr<SealedTakeToken> source;
		TestTransport transport;
		std::unique_ptr<RecordingAttachment> attachment;
	};
	std::vector<std::unique_ptr<QTemporaryDir>> m_roots;
	QStringList m_previousSettings;
	const QStringList m_settingNames{QStringLiteral("audio_import_roots"), QStringLiteral("audio_import_max_file_bytes"),
		QStringLiteral("audio_import_max_decoded_bytes")};

	QTemporaryDir* temporaryRoot()
	{
		auto root = std::make_unique<QTemporaryDir>(QCoreApplication::applicationDirPath() + QStringLiteral("/attachment-library-XXXXXX"));
		if (!root->isValid()) return nullptr;
		m_roots.emplace_back(std::move(root));
		return m_roots.back().get();
	}

	bool prepare(Fixture& fixture, std::uint64_t frames = 7000)
	{
		auto* song = Engine::getSong();
		song->clearProject();
		song->recordingProjectIdentity().resetForNewProject();
		auto* directory = temporaryRoot();
		if (!directory) return false;
		fixture.root = directory->path();
		const auto selected = Engine::recordingServiceHost()->selectRecordingRoot(fixture.root, song->recordingProjectIdentity());
		if (!selected.ok) return false;
		fixture.project = selected.project;
		fixture.takeId = uuid();
		if (!writeTake(*Engine::recordingServiceHost(), fixture.project, fixture.takeId, frames)) return false;
		auto opened = Engine::recordingServiceHost()->openSealedTake(fixture.project, fixture.takeId);
		if (!opened.source) return false;
		fixture.originalSha = opened.source->facts().sourceDigest;
		fixture.source = std::move(opened.source);
		song->setTempo(125);
		fixture.transport.world.available = true;
		fixture.transport.world.projectId = fixture.project.id;
		fixture.transport.world.projectGeneration = 1;
		fixture.transport.world.contextRevision = 1;
		fixture.transport.world.seekRevision = 0;
		fixture.transport.world.stateRevision = 0;
		fixture.transport.world.tempo = 125;
		fixture.transport.world.numerator = 4;
		fixture.transport.world.denominator = 4;
		fixture.transport.world.ticksPerBar = 192;
		fixture.transport.world.sampleRate = 48000;
		fixture.transport.world.constantTempo = true;
		fixture.transport.world.stableMeter = true;
		fixture.attachment = std::make_unique<RecordingAttachment>(*Engine::recordingServiceHost(), fixture.transport);
		return true;
	}

	std::optional<PlacementPlan> plan(const Fixture& fixture, qint64 sourceIn = 13, qint64 target = 384, quint64 revision = 1)
	{
		const auto& facts = fixture.source->facts();
		NativeGeometrySnapshot geometry;
		geometry.current = fixture.transport.world;
		geometry.reviewed = fixture.transport.world;
		geometry.reviewedSource = facts;
		geometry.placementRevision = revision;
		geometry.sourceFramesPerTick = Engine::framesPerTick(facts.sampleRate);
		const auto approval = RecordingRecorderView::approve(facts.takeId, fixture.transport.world.projectGeneration,
			revision, sourceIn, target);
		return RecordingPlacement::plan(facts, approval, geometry).plan;
	}

	QString takePath(const Fixture& fixture) const
	{
		return fixture.root + QLatin1Char('/') + fixture.project.id + QStringLiteral("/take-") + fixture.takeId + QStringLiteral(".wav");
	}

private slots:
	void initTestCase()
	{
		Engine::init(true);
		Engine::audioEngine()->audioDev()->stopProcessing();
		Engine::audioEngine()->setAudioDevice(new SyntheticOutput(48000), false);
		for (const auto& key : m_settingNames) m_previousSettings.push_back(ConfigManager::inst()->value("aibridge", key, QStringLiteral("task7-absent")));
		ConfigManager::inst()->setValue("aibridge", m_settingNames[1], QStringLiteral("20000000"));
		ConfigManager::inst()->setValue("aibridge", m_settingNames[2], QStringLiteral("60000000"));
	}

	void init()
	{
		Engine::projectJournal()->clearJournal();
		ConfigManager::inst()->setValue("aibridge", m_settingNames[1], QStringLiteral("20000000"));
		ConfigManager::inst()->setValue("aibridge", m_settingNames[2], QStringLiteral("60000000"));
	}

	void cleanup()
	{
		Engine::getSong()->stop();
		Engine::getSong()->clearProject();
		Engine::projectJournal()->clearJournal();
		ConfigManager::inst()->setValue("aibridge", m_settingNames[1], QStringLiteral("20000000"));
		ConfigManager::inst()->setValue("aibridge", m_settingNames[2], QStringLiteral("60000000"));
	}

	void cleanupTestCase()
	{
		auto* config = ConfigManager::inst();
		for (int i = 0; i < m_settingNames.size(); ++i)
		{
			if (m_previousSettings[i] == QStringLiteral("task7-absent")) config->deleteValue("aibridge", m_settingNames[i]);
			else config->setValue("aibridge", m_settingNames[i], m_previousSettings[i]);
		}
		AudioImportPolicy::configured().refresh();
		Engine::destroy();
		m_roots.clear();
	}

	void requiresStoppedTransportSealedSourceAndLocalCrop()
	{
		Fixture fixture;
		QVERIFY(prepare(fixture));
		auto planValue = plan(fixture);
		QVERIFY(planValue);
		const auto tracksBefore = Engine::getSong()->tracks().size();
		fixture.transport.world.playing = true;
		const auto playing = fixture.attachment->attach(*fixture.source, *planValue, 1);
		QCOMPARE(playing.code, QStringLiteral("transport_busy"));
		QVERIFY(!playing.attached);
		QCOMPARE(Engine::getSong()->tracks().size(), tracksBefore);
		fixture.transport.world.playing = false;
		auto invalidCrop = *planValue;
		invalidCrop.offsetTicks = 1;
		const auto unreviewed = fixture.attachment->attach(*fixture.source, invalidCrop, 1);
		QCOMPARE(unreviewed.code, QStringLiteral("invalid_placement"));
		QVERIFY(!unreviewed.attached);
		QCOMPARE(Engine::getSong()->tracks().size(), tracksBefore);
	}

	void attachmentWaitsForRecorderHostQuiescence()
	{
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		auto* song = Engine::getSong();
		song->clearProject();
		song->recordingProjectIdentity().resetForNewProject();
		auto driver = std::make_unique<NoDeviceDriver>();
		auto* driverView = driver.get();
		RecordingServiceHost host(std::move(driver));
		const auto selected = host.selectRecordingRoot(directory.path(), song->recordingProjectIdentity());
		QVERIFY(selected.ok);
		const auto takeId = uuid();
		QVERIFY(writeTake(host, selected.project, takeId, 7000));
		auto opened = host.openSealedTake(selected.project, takeId);
		QVERIFY(opened.source);
		TestTransport transport;
		transport.world.available = true;
		transport.world.projectId = selected.project.id;
		transport.world.projectGeneration = 1;
		transport.world.contextRevision = 1;
		transport.world.tempo = 125;
		transport.world.numerator = 4;
		transport.world.denominator = 4;
		transport.world.ticksPerBar = 192;
		transport.world.sampleRate = 48000;
		RecordingAttachment attachment(host, transport);
		NativeGeometrySnapshot geometry;
		geometry.current = transport.world;
		geometry.reviewed = transport.world;
		geometry.reviewedSource = opened.source->facts();
		geometry.placementRevision = 1;
		geometry.sourceFramesPerTick = Engine::framesPerTick(48000);
		const auto approval = RecordingRecorderView::approve(takeId, 1, 1, 13, 384);
		const auto planned = RecordingPlacement::plan(opened.source->facts(), approval, geometry);
		QVERIFY(planned.plan);
		SelectedInputProfile profile;
		profile.deviceId = QStringLiteral("synthetic");
		profile.hostApiName = QStringLiteral("WASAPI");
		profile.wasapiShared = true;
		profile.input = {48000, 1, 0, 4096, 5760000};
		QCOMPARE(host.enumerateInputs(0, 8).code, QStringLiteral("ok"));
		auto ticket = host.localArmTicket(profile, 1);
		QVERIFY(ticket.ok);
		QVERIFY(host.adoptLocalArm(std::move(ticket.ticket)).ok);
		QVERIFY(!host.quiescent());
		const auto tracks = song->tracks().size();
		const auto result = attachment.attach(*opened.source, *planned.plan, 1);
		QCOMPARE(result.code, QStringLiteral("recording_busy"));
		QVERIFY(!result.attached);
		QCOMPARE(song->tracks().size(), tracks);
		QCOMPARE(driverView->opens, 0);
		QVERIFY(host.disarm().ok);
		QVERIFY(host.quiescent());
	}

	void createsOneNewTrackAndClip()
	{
		Fixture fixture;
		QVERIFY(prepare(fixture));
		const auto planValue = plan(fixture);
		QVERIFY(planValue);
		const auto tracksBefore = Engine::getSong()->tracks().size();
		const auto result = fixture.attachment->attach(*fixture.source, *planValue, 1);
		QVERIFY(result.attached);
		QCOMPARE(result.code, QStringLiteral("ok"));
		QVERIFY(!result.trackId.isEmpty() && !result.clipId.isEmpty());
		QCOMPARE(Engine::getSong()->tracks().size(), tracksBefore + 1);
		SampleTrack* owner = nullptr;
		auto* clip = findRecordedClip(Engine::getSong(), fixture.takeId, &owner);
		QVERIFY(clip && owner);
		QCOMPARE(owner->numOfClips(), 1);
		QCOMPARE(clip->startPosition().getTicks(), planValue->positionTicks);
		QCOMPARE(clip->startTimeOffset().getTicks(), planValue->offsetTicks);
		QCOMPARE(clip->length().getTicks(), planValue->lengthTicks);
		QVERIFY(!clip->getAutoResize());
		QCOMPARE(clip->recordedTakeReference()->trackId, result.trackId);
		QCOMPARE(clip->recordedTakeReference()->clipId, result.clipId);
		QCOMPARE(clip->recordedTakeReference()->sourceDigest, fixture.originalSha);
	}

	void duplicateConfirmationReturnsSameIds()
	{
		Fixture fixture;
		QVERIFY(prepare(fixture));
		const auto planValue = plan(fixture);
		QVERIFY(planValue);
		const auto first = fixture.attachment->attach(*fixture.source, *planValue, 1);
		QVERIFY(first.attached);
		const auto tracks = Engine::getSong()->tracks().size();
		const auto repeated = fixture.attachment->attach(*fixture.source, *planValue, 1);
		QVERIFY(repeated.attached);
		QCOMPARE(repeated.trackId, first.trackId);
		QCOMPARE(repeated.clipId, first.clipId);
		QCOMPARE(Engine::getSong()->tracks().size(), tracks);
	}

	void newLocalRevisionAdjustsOnlyOwnedClipAndIsUndoable()
	{
		Fixture fixture;
		QVERIFY(prepare(fixture));
		const auto firstPlan = plan(fixture, 13, 384, 1);
		QVERIFY(firstPlan);
		const auto first = fixture.attachment->attach(*fixture.source, *firstPlan, 1);
		QVERIFY(first.attached);
		auto* clip = findRecordedClip(Engine::getSong(), fixture.takeId);
		QVERIFY(clip);
		const auto secondPlan = plan(fixture, 14, 500, 2);
		QVERIFY(secondPlan);
		const auto second = fixture.attachment->attach(*fixture.source, *secondPlan, 1);
		QVERIFY(second.attached);
		QCOMPARE(second.trackId, first.trackId);
		QCOMPARE(second.clipId, first.clipId);
		QCOMPARE(Engine::getSong()->tracks().size(), std::size_t(1));
		QCOMPARE(clip->startPosition().getTicks(), qint64(500));
		QCOMPARE(clip->startTimeOffset().getTicks(), qint64(-14));
		QVERIFY(Engine::projectJournal()->canUndo());
		Engine::projectJournal()->undo();
		QCOMPARE(clip->startPosition().getTicks(), qint64(384));
		QCOMPARE(clip->startTimeOffset().getTicks(), qint64(-13));
		QCOMPARE(clip->length().getTicks(), firstPlan->lengthTicks);
		QCOMPARE(clip->recordedTakeReference()->placementRevision, quint64(1));
	}

	void staleEditedOrDeletedOwnedClipDoesNotAutoDuplicate()
	{
		Fixture fixture;
		QVERIFY(prepare(fixture));
		const auto firstPlan = plan(fixture);
		QVERIFY(firstPlan);
		const auto first = fixture.attachment->attach(*fixture.source, *firstPlan, 1);
		QVERIFY(first.attached);
		auto* clip = findRecordedClip(Engine::getSong(), fixture.takeId);
		QVERIFY(clip);
		clip->movePosition(900);
		const auto tracks = Engine::getSong()->tracks().size();
		const auto stale = fixture.attachment->attach(*fixture.source, *firstPlan, 1);
		QVERIFY(!stale.attached);
		QCOMPARE(stale.code, QStringLiteral("stale_owned_clip"));
		QCOMPARE(Engine::getSong()->tracks().size(), tracks);
		SampleTrack* owner = nullptr;
		clip = findRecordedClip(Engine::getSong(), fixture.takeId, &owner);
		QVERIFY(clip && owner);
		owner->removeClip(clip);
		delete clip;
		const auto deleted = fixture.attachment->attach(*fixture.source, *firstPlan, 1);
		QVERIFY(!deleted.attached);
		QCOMPARE(deleted.code, QStringLiteral("stale_owned_clip"));
		QCOMPARE(Engine::getSong()->tracks().size(), tracks);
	}

	void prefixNotPlayedOriginalHashUnchanged()
	{
		Fixture fixture;
		QVERIFY(prepare(fixture));
		const auto planValue = plan(fixture);
		QVERIFY(planValue);
		const auto before = fileHash(takePath(fixture));
		QCOMPARE(before, fixture.originalSha);
		const auto attached = fixture.attachment->attach(*fixture.source, *planValue, 1);
		QVERIFY(attached.attached);
		auto* song = Engine::getSong();
		song->getTimeline(Song::PlayMode::Song).setLoopEnabled(false);
		song->playSong();
		song->setPlayPos(planValue->positionTicks, Song::PlayMode::Song);
		bool audible = false;
		for (int period = 0; period < 8; ++period)
		{
			const auto block = Engine::audioEngine()->renderNextPeriod();
			for (const auto& frame : block)
			{
				for (int channel = 0; channel < 2; ++channel)
				{
					const auto value = std::abs(frame[channel]);
					QVERIFY(value <= 0.5f);
					if (value > 0.01f) audible = true;
				}
			}
		}
		QVERIFY(audible);
		song->stop();
		Engine::audioEngine()->clear();
		QCOMPARE(fileHash(takePath(fixture)), before);
	}

	void saveReloadAndSaveAsPreserveTakeLinkAndOff()
	{
		Fixture fixture;
		QVERIFY(prepare(fixture));
		const auto planValue = plan(fixture);
		QVERIFY(planValue);
		QVERIFY(fixture.attachment->attach(*fixture.source, *planValue, 1).attached);
		const auto firstFile = fixture.root + QStringLiteral("/placed.mmp");
		QVERIFY(Engine::getSong()->saveProjectFile(firstFile));
		Engine::getSong()->loadProject(firstFile);
		auto* restored = findRecordedClip(Engine::getSong(), fixture.takeId);
		QVERIFY(restored);
		QCOMPARE(restored->sample().sampleSize(), size_t(7000));
		QCOMPARE(restored->startPosition().getTicks(), planValue->positionTicks);
		QCOMPARE(restored->startTimeOffset().getTicks(), planValue->offsetTicks);
		QCOMPARE(restored->recordedTakeReference()->sourceDigest, fixture.originalSha);
		const auto saveAs = fixture.root + QStringLiteral("/placed-save-as.mmp");
		QVERIFY(Engine::getSong()->saveProjectFile(saveAs));
		Engine::getSong()->loadProject(saveAs);
		restored = findRecordedClip(Engine::getSong(), fixture.takeId);
		QVERIFY(restored);
		QCOMPARE(restored->startTimeOffset().getTicks(), planValue->offsetTicks);
		QCOMPARE(fileHash(takePath(fixture)), fixture.originalSha);
	}

	void tempoRateChangeInvalidatesPlacementReview()
	{
		Fixture fixture;
		QVERIFY(prepare(fixture));
		const auto planValue = plan(fixture);
		QVERIFY(planValue);
		QVERIFY(fixture.attachment->attach(*fixture.source, *planValue, 1).attached);
		auto* clip = findRecordedClip(Engine::getSong(), fixture.takeId);
		QVERIFY(clip);
		const auto before = clip->startPosition().getTicks();
		fixture.transport.world.tempo = 130;
		Engine::getSong()->setTempo(130);
		auto staleReview = *planValue;
		staleReview.placementRevision = 2;
		const auto result = fixture.attachment->attach(*fixture.source, staleReview, 1);
		QVERIFY(!result.attached);
		QCOMPARE(result.code, QStringLiteral("placement_review_stale"));
		QCOMPARE(clip->startPosition().getTicks(), before);
	}

	void reentrantOwnerInvalidationPreservesSavedUnattached()
	{
		Fixture fixture;
		QVERIFY(prepare(fixture));
		const auto planValue = plan(fixture);
		QVERIFY(planValue);
		const auto before = fileHash(takePath(fixture));
		bool invalidated = false;
		auto* song = Engine::getSong();
		const auto connection = connect(song, &TrackContainer::trackAdded, song, [&](Track* track)
		{
			if (track->name().startsWith(QStringLiteral("Vocal take ")))
			{
				invalidated = true;
				delete track;
			}
		}, Qt::DirectConnection);
		const auto result = fixture.attachment->attach(*fixture.source, *planValue, 1);
		QObject::disconnect(connection);
		QVERIFY(invalidated);
		QVERIFY(!result.attached);
		QCOMPARE(result.code, QStringLiteral("saved_unattached"));
		QVERIFY(!findRecordedClip(song, fixture.takeId));
		QCOMPARE(fileHash(takePath(fixture)), before);
	}

	void partialNativeFailureNotReportedAttached()
	{
		Fixture fixture;
		QVERIFY(prepare(fixture));
		const auto planValue = plan(fixture);
		QVERIFY(planValue);
		ConfigManager::inst()->setValue("aibridge", m_settingNames[2], QStringLiteral("4179"));
		const auto tracks = Engine::getSong()->tracks().size();
		const auto before = fileHash(takePath(fixture));
		const auto result = fixture.attachment->attach(*fixture.source, *planValue, 1);
		QVERIFY(!result.attached);
		QCOMPARE(result.code, QStringLiteral("saved_unattached"));
		QCOMPARE(result.cause, QStringLiteral("source_too_large"));
		QCOMPARE(Engine::getSong()->tracks().size(), tracks);
		QCOMPARE(fileHash(takePath(fixture)), before);
		QCOMPARE(ConfigManager::inst()->value("aibridge", m_settingNames[2]), QStringLiteral("4179"));
	}

	void fullDurationTakeAttachesWithinExistingDecoderBudget()
	{
		constexpr std::uint64_t frames = std::uint64_t(48000) * 120;
		Fixture fixture;
		QVERIFY(prepare(fixture, frames));
		const auto planValue = plan(fixture);
		QVERIFY(planValue);
		const auto before = fileHash(takePath(fixture));
		QVERIFY(!before.isEmpty());
		const auto result = fixture.attachment->attach(*fixture.source, *planValue, 1);
		QVERIFY2(result.attached, qPrintable(result.cause));
		auto* clip = findRecordedClip(Engine::getSong(), fixture.takeId);
		QVERIFY(clip);
		QCOMPARE(clip->authorizedSourceInfo()->frames, frames);
		QVERIFY(!clip->getAutoResize());
		QCOMPARE(fileHash(takePath(fixture)), before);
	}

	void unrelatedTracksAndSourcesUnchanged()
	{
		Fixture fixture;
		QVERIFY(prepare(fixture));
		SampleTrack unrelated(Engine::getSong());
		SampleClip unrelatedClip(&unrelated);
		unrelatedClip.setSampleBuffer(std::make_shared<const SampleBuffer>(std::vector<SampleFrame>(8), 48000));
		unrelatedClip.movePosition(123);
		unrelatedClip.changeLength(56);
		const auto unrelatedTracks = Engine::getSong()->tracks().size();
		const auto unrelatedClips = unrelated.numOfClips();
		const auto unrelatedSize = unrelatedClip.sample().sampleSize();
		const auto planValue = plan(fixture);
		QVERIFY(planValue);
		QVERIFY(fixture.attachment->attach(*fixture.source, *planValue, 1).attached);
		QCOMPARE(Engine::getSong()->tracks().size(), unrelatedTracks + 1);
		QCOMPARE(unrelated.numOfClips(), unrelatedClips);
		QCOMPARE(unrelatedClip.startPosition().getTicks(), qint64(123));
		QCOMPARE(unrelatedClip.length().getTicks(), qint64(56));
		QCOMPARE(unrelatedClip.sample().sampleSize(), unrelatedSize);
		QCOMPARE(fileHash(takePath(fixture)), fixture.originalSha);
	}
};

QTEST_GUILESS_MAIN(RecordingAttachmentTest)
#include "RecordingAttachmentTest.moc"
