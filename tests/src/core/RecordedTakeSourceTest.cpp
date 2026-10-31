#include "AudioImportPolicy.h"
#include "ConfigManager.h"
#include "Engine.h"
#include "RecordingLibrary.h"
#include "RecordingServiceHost.h"
#include "RecordingWriter.h"
#include "RecordedTakeSource.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"
#include <QtTest>
#include <QTemporaryDir>
#include <QDomDocument>
#include <QFile>
#include <QCryptographicHash>
#include <QUuid>
#include <QElapsedTimer>
#include "Track.h"

using namespace lmms;
using namespace lmms::recording;

namespace
{
QString uuid()
{
	return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool waitForWriter(RecordingWriter& writer, WriterState state)
{
	QElapsedTimer timer;
	timer.start();
	while (timer.elapsed() < 3000)
	{
		if (writer.poll().state == state) { return true; }
		QTest::qWait(1);
	}
	return writer.poll().state == state;
}

bool writeCompleteTakeSink(std::unique_ptr<RecordingWriterSink> sink, const ProjectIdentity& project, const QString& takeId)
{
	constexpr std::uint32_t rate = 48000;
	constexpr std::uint64_t maximum = std::uint64_t(rate) * 120;
	if (!sink) { return false; }
	RecordingRing ring{rate, 131072, maximum};
	TakeDescriptor descriptor;
	descriptor.projectId = project.id;
	descriptor.takeId = takeId;
	descriptor.label = QStringLiteral("sealed test");
	descriptor.epoch = {1, 1};
	descriptor.projectGeneration = 1;
	descriptor.maxRawFrames = maximum;
	descriptor.sampleRate = rate;
	RecordingWriter writer;
	if (!writer.begin(descriptor, std::move(sink), ring).ok || !waitForWriter(writer, WriterState::Ready)) { return false; }
	const float samples[]{0.25f, -0.25f, 0.5f, 0.0f};
	if (ring.push(samples, 4, 1, 0).acceptedFrames != 4) { return false; }
	InputSnapshot end;
	end.epoch = descriptor.epoch;
	end.receivedFrames = 4;
	end.acceptedFrames = 4;
	end.hardwareLossCountKnown = true;
	writer.requestInputClosed(descriptor.epoch, end);
	return waitForWriter(writer, WriterState::Sealed) && writer.joinAfterExit().ok;
}

bool writeCompleteTake(RecordingLibrary& library, const ProjectIdentity& project, const QString& takeId)
{
	auto created = library.createUniqueTake(project, {takeId, QStringLiteral("sealed test")}, 48000, 5760000);
	return created.code == QStringLiteral("ok") && writeCompleteTakeSink(std::move(created.sink), project, takeId);
}

bool writeCompleteHostTake(RecordingServiceHost& host, const ProjectIdentity& project, const QString& takeId)
{
	Q_UNUSED(project);
	auto created = host.createUniqueTake({takeId, QStringLiteral("sealed test")}, 48000, 5760000);
	return created.code == QStringLiteral("ok") && writeCompleteTakeSink(std::move(created.sink), host.projectIdentity(), takeId);
}

std::unique_ptr<QTemporaryDir> temporaryRoot()
{
	return std::make_unique<QTemporaryDir>(QCoreApplication::applicationDirPath() + QStringLiteral("/recorded-source-XXXXXX"));
}

QByteArray hashFile(const QString& path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) return {};
	return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
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

class RecordedTakeSourceTest : public QObject
{
	Q_OBJECT
	std::vector<std::unique_ptr<QTemporaryDir>> m_roots;
	QStringList m_previousSettings;
	const QStringList m_settingNames{QStringLiteral("audio_import_roots"), QStringLiteral("audio_import_max_file_bytes"),
		QStringLiteral("audio_import_max_decoded_bytes")};

	QTemporaryDir* root()
	{
		auto value = temporaryRoot();
		if (!value->isValid()) { return nullptr; }
		m_roots.emplace_back(std::move(value));
		return m_roots.back().get();
	}

	void configureImportBudgets(const QString& decodedBytes = QStringLiteral("2000000"))
	{
		auto* config = ConfigManager::inst();
		config->setValue("aibridge", m_settingNames[1], QStringLiteral("2000000"));
		config->setValue("aibridge", m_settingNames[2], decodedBytes);
	}

	RecordingLibrary::Approval authorize(QTemporaryDir& directory, ProjectIdentity& project)
	{
		auto approval = RecordingLibrary::authorizeLocalRoot(directory.path());
		if (approval.library)
		{
			project = {uuid(), 1};
			if (approval.library->prepareProjectFolder(project) != QStringLiteral("ok")) { approval.library.reset(); }
		}
		return approval;
	}

	bool selectHostRoot(QTemporaryDir& directory, ProjectIdentity& project)
	{
		auto* song = Engine::getSong();
		song->recordingProjectIdentity().resetForNewProject();
		const auto selected = Engine::recordingServiceHost()->selectRecordingRoot(directory.path(), song->recordingProjectIdentity());
		project = selected.project;
		return selected.ok;
	}

private slots:
	void initTestCase()
	{
		Engine::init(true);
		Engine::audioEngine()->audioDev()->stopProcessing();
		for (const auto& key : m_settingNames) { m_previousSettings.push_back(ConfigManager::inst()->value("aibridge", key, QStringLiteral("task7-absent"))); }
		configureImportBudgets();
	}

	void cleanupTestCase()
	{
		auto* config = ConfigManager::inst();
		for (int i = 0; i < m_settingNames.size(); ++i)
		{
			if (m_previousSettings[i] == QStringLiteral("task7-absent")) { config->deleteValue("aibridge", m_settingNames[i]); }
			else { config->setValue("aibridge", m_settingNames[i], m_previousSettings[i]); }
		}
		AudioImportPolicy::configured().refresh();
		Engine::destroy();
		m_roots.clear();
	}

	void recordingTokenCannotAuthorizeArbitraryFileOrOtherProject()
	{
		auto* directory = root();
		QVERIFY(directory);
		ProjectIdentity project;
		auto approved = authorize(*directory, project);
		QVERIFY(approved.library);
		const auto takeId = uuid();
		QVERIFY(writeCompleteTake(*approved.library, project, takeId));

		auto opened = approved.library->openSealedTake(project, takeId);
		QCOMPARE(opened.code, QStringLiteral("ok"));
		QVERIFY(opened.source);
		QCOMPARE(opened.source->facts().projectId, project.id);
		QCOMPARE(opened.source->facts().takeId, takeId);
		QVERIFY(opened.source->facts().sourceDigest.size() == 32);

		auto otherProject = project;
		otherProject.id = uuid();
		const auto wrongProject = approved.library->openSealedTake(otherProject, takeId);
		QVERIFY(!wrongProject.source);
		QVERIFY(wrongProject.code != QStringLiteral("ok"));
		const auto wrongTake = approved.library->openSealedTake(project, uuid());
		QVERIFY(!wrongTake.source);
		QVERIFY(wrongTake.code != QStringLiteral("ok"));
	}

	void rootsUnchangedByRecordedImport()
	{
		auto* directory = root();
		QVERIFY(directory);
		ProjectIdentity project;
		auto approved = authorize(*directory, project);
		QVERIFY(approved.library);
		const auto takeId = uuid();
		QVERIFY(writeCompleteTake(*approved.library, project, takeId));
		auto opened = approved.library->openSealedTake(project, takeId);
		QCOMPARE(opened.code, QStringLiteral("ok"));
		const QString oldRoots = ConfigManager::inst()->value("aibridge", m_settingNames[0], QStringLiteral(""));
		ConfigManager::inst()->setValue("aibridge", m_settingNames[0], QStringLiteral("[]"));
		const auto prepared = AudioImportPolicy::configured().prepareRecordedTake(std::move(*opened.source));
		QCOMPARE(prepared.code, QStringLiteral("ok"));
		QVERIFY(prepared.audio);
		QCOMPARE(prepared.audio->info().frames, quint64(4));
		QCOMPARE(ConfigManager::inst()->value("aibridge", m_settingNames[0]), QStringLiteral("[]"));
		ConfigManager::inst()->setValue("aibridge", m_settingNames[0], oldRoots);
	}

	void forgedTokenDigestIdentityRejected()
	{
		auto* directory = root();
		QVERIFY(directory);
		ProjectIdentity project;
		QVERIFY(selectHostRoot(*directory, project));
		const auto takeId = uuid();
		QVERIFY(writeCompleteHostTake(*Engine::recordingServiceHost(), project, takeId));
		auto opened = Engine::recordingServiceHost()->openSealedTake(project, takeId);
		QCOMPARE(opened.code, QStringLiteral("ok"));
		QVERIFY(Engine::getSong()->saveProjectFile(directory->filePath(QStringLiteral("trusted-project.mmp"))));
		auto reference = RecordedTakeSource::fromFacts(opened.source->facts());
		QVERIFY(reference.has_value());
		reference->trackId = uuid();
		reference->clipId = uuid();
		reference->placementRevision = 1;
		reference->sourceDigest[0] = reference->sourceDigest[0] == '0' ? '1' : '0';
		QString code;
		QDomDocument document;
		const auto tampered = RecordedTakeSource::writeReference(document, *reference);
		QVERIFY(!tampered.isNull());
		const auto parsed = RecordedTakeSource::readReference(tampered, code);
		QVERIFY(parsed.has_value());
		QVERIFY(!RecordedTakeSource::matches(*parsed, opened.source->facts()));
		auto clipNode = document.createElement(QStringLiteral("sampleclip"));
		document.appendChild(clipNode);
		clipNode.setAttribute(QStringLiteral("src"), QStringLiteral("C:/untrusted/voice.wav"));
		clipNode.appendChild(tampered);
		SampleTrack track(Engine::getSong());
		SampleClip clip(&track);
		clip.loadSettings(clipNode);
		QCOMPARE(clip.sample().sampleSize(), size_t(0));
		QCOMPARE(clip.sourceLoadError(), QStringLiteral("recorded_source_changed"));
		QVERIFY(sampleClipState(clip).firstChildElement(QStringLiteral("recorded_take")).attribute(QStringLiteral("source_sha256"))
			!= QString::fromLatin1(opened.source->facts().sourceDigest.toHex()));
	}

	void untrustedProjectDocumentCannotReopenRecordedTake()
	{
		auto* directory = root();
		QVERIFY(directory);
		ProjectIdentity project;
		QVERIFY(selectHostRoot(*directory, project));
		const auto takeId = uuid();
		QVERIFY(writeCompleteHostTake(*Engine::recordingServiceHost(), project, takeId));
		auto opened = Engine::recordingServiceHost()->openSealedTake(project, takeId);
		QCOMPARE(opened.code, QStringLiteral("ok"));
		auto reference = RecordedTakeSource::fromFacts(opened.source->facts());
		QVERIFY(reference.has_value());
		reference->trackId = uuid();reference->clipId = uuid();reference->placementRevision = 1;
		QDomDocument document;
		auto node = document.createElement(QStringLiteral("sampleclip"));document.appendChild(node);
		node.setAttribute(QStringLiteral("src"), QStringLiteral("C:/untrusted/voice.wav"));
		node.appendChild(RecordedTakeSource::writeReference(document, *reference));
		SampleTrack track(Engine::getSong());SampleClip clip(&track);clip.loadSettings(node);
		QCOMPARE(clip.sourceLoadError(), QStringLiteral("project_identity_unverified"));
		QCOMPARE(clip.sample().sampleSize(), size_t(0));
		QCOMPARE(sampleClipState(clip).attribute(QStringLiteral("src")), QString{});
		QVERIFY(!Engine::recordingServiceHost()->snapshot().input.driverInitialized);
	}

	void missingRevokedRootRetainsUnavailableMetadata()
	{
		auto* directory = root();
		QVERIFY(directory);
		ProjectIdentity project;
		QVERIFY(selectHostRoot(*directory, project));
		const auto takeId = uuid();
		QVERIFY(writeCompleteHostTake(*Engine::recordingServiceHost(), project, takeId));
		auto opened = Engine::recordingServiceHost()->openSealedTake(project, takeId);
		QCOMPARE(opened.code, QStringLiteral("ok"));
		const auto sourcePath = directory->path() + QLatin1Char('/') + project.id + QStringLiteral("/take-") + takeId + QStringLiteral(".wav");
		const auto sourceHash = hashFile(sourcePath);
		QVERIFY(!sourceHash.isEmpty());
		auto reference = RecordedTakeSource::fromFacts(opened.source->facts());
		QVERIFY(reference.has_value());
		reference->trackId = uuid();
		reference->clipId = uuid();
		reference->placementRevision = 1;
		QDomDocument document;
		auto clipNode = document.createElement(QStringLiteral("sampleclip"));
		document.appendChild(clipNode);
		clipNode.setAttribute(QStringLiteral("pos"), 384);
		clipNode.setAttribute(QStringLiteral("len"), 188);
		clipNode.setAttribute(QStringLiteral("off"), -13);
		clipNode.setAttribute(QStringLiteral("autoresize"), 0);
		clipNode.appendChild(RecordedTakeSource::writeReference(document, *reference));
		auto* song = Engine::getSong();
		auto* track = Track::create(Track::Type::Sample, song);
		QVERIFY(track);
		auto* clip = new SampleClip(track);
		clip->loadSettings(clipNode);
		QVERIFY(!clip->sourceLoadError().isEmpty()); // New unsaved projects cannot silently reopen a take.
		const auto projectFile = directory->filePath(QStringLiteral("placed.mmp"));
		QVERIFY(song->saveProjectFile(projectFile));
		song->loadProject(projectFile);
		auto* restored = dynamic_cast<SampleClip*>(song->tracks().back()->getClip(0));
		QVERIFY(restored);
		QCOMPARE(restored->sample().sampleSize(), size_t(4));
		QVERIFY(restored->recordedTakeReference());

		auto* emptyRoot = root();
		QVERIFY(emptyRoot);
		QVERIFY(Engine::recordingServiceHost()->selectRecordingRoot(emptyRoot->path(), song->recordingProjectIdentity()).ok);
		song->loadProject(projectFile);
		restored = dynamic_cast<SampleClip*>(song->tracks().back()->getClip(0));
		QVERIFY(restored);
		QCOMPARE(restored->sample().sampleSize(), size_t(0));
		QVERIFY(!restored->sourceLoadError().isEmpty());
		const auto saved = sampleClipState(*restored);
		QVERIFY(!saved.firstChildElement(QStringLiteral("recorded_take")).isNull());
		QCOMPARE(saved.firstChildElement(QStringLiteral("recorded_take")).attribute(QStringLiteral("take_id")), takeId);
		QCOMPARE(saved.attribute(QStringLiteral("src")), QString{});
		QCOMPARE(hashFile(sourcePath), sourceHash);
	}

	void reloadDoesNotOpenMic()
	{
		const auto before = Engine::recordingServiceHost()->snapshot();
		QVERIFY(!before.input.driverInitialized);
		RawTakeFacts facts{uuid(), uuid(), 48000, 4, {1, 2}, QByteArray(32, 'x')};
		auto reference = RecordedTakeSource::fromFacts(facts);
		QVERIFY(reference.has_value());
		QDomDocument document;
		auto node = document.createElement(QStringLiteral("sampleclip"));
		document.appendChild(node);
		node.setAttribute(QStringLiteral("pos"), 0);
		node.setAttribute(QStringLiteral("len"), 192);
		node.setAttribute(QStringLiteral("off"), 0);
		node.appendChild(RecordedTakeSource::writeReference(document, *reference));
		QDomDocument headDoc;
		auto head = headDoc.createElement(QStringLiteral("head"));
		headDoc.appendChild(head);
		auto identity = headDoc.createElement(QStringLiteral("recording_project"));
		identity.setAttribute(QStringLiteral("schema"), 1);
		identity.setAttribute(QStringLiteral("id"), facts.projectId);
		identity.setAttribute(QStringLiteral("nonce"), uuid());
		head.appendChild(identity);
		Engine::getSong()->recordingProjectIdentity().acceptLoad(head, QString{});
		SampleTrack track(Engine::getSong());
		SampleClip clip(&track);
		clip.loadSettings(node);
		QVERIFY(!clip.sourceLoadError().isEmpty());
		const auto after = Engine::recordingServiceHost()->snapshot();
		QVERIFY(!after.input.driverInitialized);
		QCOMPARE(after.input.state, RecordingControllerState::Closed);
	}

	void decodeBudgetNotSilentlyRaised()
	{
		auto* directory = root();
		QVERIFY(directory);
		ProjectIdentity project;
		auto approved = authorize(*directory, project);
		QVERIFY(approved.library);
		const auto takeId = uuid();
		QVERIFY(writeCompleteTake(*approved.library, project, takeId));
		auto opened = approved.library->openSealedTake(project, takeId);
		QCOMPARE(opened.code, QStringLiteral("ok"));
		const QString oldBudget = ConfigManager::inst()->value("aibridge", m_settingNames[2]);
		ConfigManager::inst()->setValue("aibridge", m_settingNames[2], QStringLiteral("4179"));
		const auto result = AudioImportPolicy::configured().prepareRecordedTake(std::move(*opened.source));
		QCOMPARE(result.code, QStringLiteral("source_too_large"));
		QVERIFY(!result.audio);
		QCOMPARE(ConfigManager::inst()->value("aibridge", m_settingNames[2]), QStringLiteral("4179"));
		ConfigManager::inst()->setValue("aibridge", m_settingNames[2], oldBudget);
	}
};

QTEST_GUILESS_MAIN(RecordedTakeSourceTest)
#include "RecordedTakeSourceTest.moc"
