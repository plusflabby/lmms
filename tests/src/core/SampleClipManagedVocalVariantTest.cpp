#include <QtTest>
#include <QCryptographicHash>
#include <QDomDocument>
#include <QTemporaryDir>
#include <QDir>
#include <QUuid>
#include <array>
#include <memory>
#include <numeric>
#include <vector>
#include "ConfigManager.h"
#include "AudioDevice.h"
#include "Engine.h"
#include "ManagedVocalAssetRoot.h"
#include "ManagedVocalAssetStore.h"
#include "ManagedVocalPlayHandle.h"
#include "ProjectJournal.h"
#include "RecordingLibrary.h"
#include "RecordedTakeSource.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"

using namespace lmms;

class ManagedVocalProbeDevice final : public AudioDevice
{
public:
	explicit ManagedVocalProbeDevice(int sampleRate) : AudioDevice(2, Engine::audioEngine())
	{
		setSampleRate(sampleRate);
	}

private:
	void startProcessingImpl() override {}
	void stopProcessingImpl() override {}
};

class SampleClipManagedVocalVariantTest : public QObject
{
	Q_OBJECT

	QString m_oldRoot;

	static QString digest(char value)
	{
		return QString(64, QChar(value));
	}

	static QString uuid()
	{
		return QUuid::createUuid().toString(QUuid::WithoutBraces);
	}

	static ManagedVocalVariantAssociation association(
		const ManagedVocalVariantReference& reference,
		Song& song,
		int rate,
		quint64 frames)
	{
		ManagedVocalVariantAssociation value;
		value.active = reference;
		value.playback.tempoBpm = song.getTempo();
		value.playback.outputRate = rate;
		value.playback.ticksPerQuarterNote = DefaultTicksPerBar / 4;
		value.playback.timeSignatureNumerator = song.getTimeSigModel().getNumerator();
		value.playback.timeSignatureDenominator = song.getTimeSigModel().getDenominator();
		const auto extent = managedVocalVisualExtent(frames, rate, value.playback.tempoBpm);
		value.playback.visualLengthTicks = extent.visualLengthTicks;
		value.playback.nativeVisualCoverageFrames = extent.nativeCoverageFrames;
		value.playback.endpointResidualNumerator = extent.exactResidualNumerator;
		value.playback.endpointResidualDenominator = extent.exactResidualDenominator;
		value.playback.worldHash = digest('f');
		value.originalAutoResize = true;
		value.originalLengthTicks = 8;
		value.originalOffsetTicks = 0;
		return value;
	}

	static ManagedVocalVariantReference createVariant(const QString& sourceReference, int rate)
	{
		auto approval = ManagedVocalAssetRoot::authorizeConfiguredRoot();
		if (!approval.root) return {};
		ManagedVocalAssetStore store(approval.root);
		auto created = store.beginCreate(3, rate, 2);
		if (!created.writer) return {};
		const std::array<float, 6> output{0.1f, -0.1f, 0.2f, -0.2f, 0.3f, -0.3f};
		if (created.writer->appendFrames(output) != QStringLiteral("ok")) return {};

		ManagedVocalManifest manifest;
		manifest.assetRootId = approval.root->rootId();
		manifest.media = {3, rate, 2, digest('0'), 0.0, false};
		manifest.recipeHash = digest('a');
		manifest.rendererId = QStringLiteral("native-float-renderer");
		manifest.rendererVersion = QStringLiteral("1");
		manifest.recipe = QJsonObject{{"recipe_version", 1}, {"operation", "gain_envelope"}};
		manifest.sources.push_back({"clip-a", digest('b'), digest('c'), sourceReference, {0, 8}, 8, rate, 2,
			ManagedVocalSourceBasis::AuthorizedPcm, {}, {}});
		auto published = store.publish(std::move(created.writer), std::move(manifest));
		return published.value.value_or(ManagedVocalVariantReference{});
	}

	static QDomElement originalClipSettings(QDomDocument& document, SampleClip& source, qint64 variantLength)
	{
		auto element = document.createElement(QStringLiteral("sampleclip"));
		document.appendChild(element);
		source.saveSettings(document, element);
		element.setAttribute(QStringLiteral("len"), QString::number(variantLength));
		element.setAttribute(QStringLiteral("autoresize"), QStringLiteral("0"));
		return element;
	}

private slots:
	void initTestCase()
	{
		Engine::init(true);
		m_oldRoot = ConfigManager::inst()->value(QStringLiteral("vocal_assets"),
			QStringLiteral("managed_root"), QStringLiteral("__unset__"));
		Engine::audioEngine()->audioDev()->stopProcessing();
	}

	void cleanupTestCase()
	{
		if (m_oldRoot == QStringLiteral("__unset__"))
		{
			ConfigManager::inst()->deleteValue(QStringLiteral("vocal_assets"), QStringLiteral("managed_root"));
		}
		else
		{
			ConfigManager::inst()->setValue(QStringLiteral("vocal_assets"),
				QStringLiteral("managed_root"), m_oldRoot);
		}
		Engine::destroy();
	}

	void cleanup()
	{
		Engine::projectJournal()->clearJournal();
		Engine::projectJournal()->setJournalling(false);
	}

	void savesReloadsAndClonesManagedVariantWithoutReplacingOriginalBuffer()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed asset root authorization is Windows-only");
#else
		QTemporaryDir rootDirectory;
		QVERIFY(rootDirectory.isValid());
		ConfigManager::inst()->setValue(QStringLiteral("vocal_assets"), QStringLiteral("managed_root"),
			QDir::fromNativeSeparators(rootDirectory.path()));
		auto root = ManagedVocalAssetRoot::authorizeConfiguredRoot();
		QVERIFY(root.root);

		SampleTrack sourceTrack(Engine::getSong());
		SampleClip source(&sourceTrack);
		const auto sampleRate = int(Engine::audioEngine()->outputSampleRate());
		source.setSampleBuffer(std::make_shared<const SampleBuffer>(
			std::vector<SampleFrame>(8, SampleFrame(0.25f)), sampleRate));
		source.changeLength(TimePos(8));
		const auto variant = createVariant(QStringLiteral("embedded:source-a"), sampleRate);
		QVERIFY(!variant.assetId.isEmpty());
		const auto plannedAssociation = association(variant, *Engine::getSong(), sampleRate, variant.frames);
		Engine::projectJournal()->clearJournal();
		Engine::projectJournal()->setJournalling(true);
		QCOMPARE(source.applyManagedVocalVariantAssociation(plannedAssociation), QStringLiteral("asset_loading"));
		QVERIFY(!source.hasManagedVocalVariant());
		QTRY_COMPARE_WITH_TIMEOUT(source.managedVocalVariantStatus(), QStringLiteral("asset_ready"), 5000);
		QCOMPARE(source.applyManagedVocalVariantAssociation(plannedAssociation), QStringLiteral("ok"));
		QVERIFY(source.hasManagedVocalVariant());
		QCOMPARE(source.length().getTicks(), qint64(plannedAssociation.playback.visualLengthTicks));
		QCOMPARE(source.sample().sampleSize(), size_t(8));
		QVERIFY(Engine::projectJournal()->canUndo());
		Engine::projectJournal()->undo();
		QVERIFY(!source.hasManagedVocalVariant());
		QCOMPARE(source.length().getTicks(), qint64(8));
		QVERIFY(source.getAutoResize());
		QVERIFY(Engine::projectJournal()->canRedo());
		Engine::projectJournal()->redo();
		QVERIFY(source.hasManagedVocalVariant());
		QTRY_COMPARE_WITH_TIMEOUT(source.managedVocalVariantStatus(), QStringLiteral("ok"), 5000);
		QCOMPARE(source.revertManagedVocalVariant(), QStringLiteral("ok"));
		QVERIFY(!source.hasManagedVocalVariant());
		QCOMPARE(source.length().getTicks(), qint64(8));
		QVERIFY(source.getAutoResize());

		QDomDocument document;
		auto settings = originalClipSettings(document, source, 1);
		const auto managed = writeManagedVocalVariantAssociation(document, plannedAssociation);
		QVERIFY(!managed.isNull());
		settings.appendChild(managed);

		SampleTrack restoredTrack(Engine::getSong());
		SampleClip restored(&restoredTrack);
		restored.loadSettings(settings);
		QVERIFY(restored.hasManagedVocalVariant());
		QTRY_COMPARE_WITH_TIMEOUT(restored.managedVocalVariantStatus(), QStringLiteral("ok"), 5000);
		QVERIFY(restored.managedVocalPlaybackView());
		QCOMPARE(restored.managedVocalPlaybackView()->frames, variant.frames);
		QCOMPARE(restored.sample().sampleSize(), size_t(8));
		QCOMPARE(restored.sample().data()[0].left(), 0.25f);
		std::vector<SampleFrame> block(Engine::audioEngine()->framesPerPeriod(), SampleFrame(9.0f));
		ManagedVocalPlayHandle handle(&restoredTrack, restored.managedVocalPlaybackView());
		handle.play(block.data());
		QCOMPARE(block[0].left(), 0.1f);
		QCOMPARE(block[1].right(), -0.2f);
		QCOMPARE(block[2].left(), 0.3f);
		QCOMPARE(block[3].left(), 0.0f);
		QVERIFY(handle.isFinished());
		QVERIFY(restoredTrack.play(TimePos(0), Engine::audioEngine()->framesPerPeriod(), 0, -1));

		ManagedVocalPlayHandle seekingHandle(&restoredTrack, restored.managedVocalPlaybackView(), 1, 1);
		seekingHandle.setOffset(2);
		std::fill(block.begin(), block.end(), SampleFrame(7.0f));
		seekingHandle.play(block.data());
		QCOMPARE(block[0].left(), 0.0f);
		QCOMPARE(block[1].right(), 0.0f);
		QCOMPARE(block[2].left(), 0.2f);
		QCOMPARE(block[2].right(), -0.2f);
		QVERIFY(seekingHandle.isFinished());

		QDomDocument savedDocument;
		auto saved = savedDocument.createElement(QStringLiteral("sampleclip"));
		savedDocument.appendChild(saved);
		restored.saveSettings(savedDocument, saved);
		QCOMPARE(saved.attribute(QStringLiteral("data")), settings.attribute(QStringLiteral("data")));
		QCOMPARE(saved.attribute(QStringLiteral("src")), settings.attribute(QStringLiteral("src")));
		QCOMPARE(saved.firstChildElement(QStringLiteral("managed_vocal_variant"))
			.attribute(QStringLiteral("asset_id")), variant.assetId);

		std::unique_ptr<SampleClip> clone(restored.clone());
		QVERIFY(clone->hasManagedVocalVariant());
		QCOMPARE(clone->managedVocalVariantStatus(), QStringLiteral("ok"));
		QVERIFY(clone->managedVocalPlaybackView());
		QCOMPARE(restored.revertManagedVocalVariant(), QStringLiteral("ok"));
		QVERIFY(!restored.hasManagedVocalVariant());
		QCOMPARE(restored.length().getTicks(), qint64(8));
		QVERIFY(restored.getAutoResize());
#endif
	}

	void missingOrCorruptVariantRetainsAssociationAndBlocksOriginalFallback()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed asset root authorization is Windows-only");
#else
		QTemporaryDir rootDirectory;
		QVERIFY(rootDirectory.isValid());
		ConfigManager::inst()->setValue(QStringLiteral("vocal_assets"), QStringLiteral("managed_root"),
			QDir::fromNativeSeparators(rootDirectory.path()));
		const auto sampleRate = int(Engine::audioEngine()->outputSampleRate());
		const auto variant = createVariant(QStringLiteral("embedded:source-b"), sampleRate);
		QVERIFY(!variant.assetId.isEmpty());
		auto corrupt = variant;
		corrupt.mediaSha256 = digest('d');

		SampleTrack sourceTrack(Engine::getSong());
		SampleClip source(&sourceTrack);
		source.setSampleBuffer(std::make_shared<const SampleBuffer>(
			std::vector<SampleFrame>(8, SampleFrame(0.5f)), sampleRate));
		source.setAutoResize(false);
		source.changeLength(TimePos(1));
		QDomDocument document;
		auto settings = originalClipSettings(document, source, 1);
		auto managed = association(corrupt, *Engine::getSong(), sampleRate, corrupt.frames);
		settings.appendChild(writeManagedVocalVariantAssociation(document, managed));

		SampleTrack restoredTrack(Engine::getSong());
		SampleClip restored(&restoredTrack);
		restored.loadSettings(settings);
		QVERIFY(restored.hasManagedVocalVariant());
		QTRY_COMPARE_WITH_TIMEOUT(restored.managedVocalVariantStatus(),
			QStringLiteral("asset_integrity_failed"), 5000);
		QVERIFY(!restored.managedVocalPlaybackView());
		QCOMPARE(restored.sample().sampleSize(), size_t(8));
		QVERIFY(!restoredTrack.play(TimePos(0), Engine::audioEngine()->framesPerPeriod(), 0, -1));

		QDomDocument savedDocument;
		auto saved = savedDocument.createElement(QStringLiteral("sampleclip"));
		savedDocument.appendChild(saved);
		restored.saveSettings(savedDocument, saved);
		QCOMPARE(saved.firstChildElement(QStringLiteral("managed_vocal_variant"))
			.attribute(QStringLiteral("asset_id")), variant.assetId);
#endif
	}

	void applyingManagedVariantPreservesTheRecordedTakeReference()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed asset root authorization is Windows-only");
#else
		QTemporaryDir rootDirectory;
		QVERIFY(rootDirectory.isValid());
		ConfigManager::inst()->setValue(QStringLiteral("vocal_assets"), QStringLiteral("managed_root"),
			QDir::fromNativeSeparators(rootDirectory.path()));
		const auto rate = int(Engine::audioEngine()->outputSampleRate());
		const recording::RawTakeFacts facts{uuid(), uuid(), std::uint32_t(rate), 8, {1, 2}, QByteArray(32, char(0xab))};
		const auto reference = recording::RecordedTakeSource::fromFacts(facts);
		QVERIFY(reference);
		const auto variant = createVariant(QStringLiteral("recorded-take:" ) + reference->takeId, rate);
		QVERIFY(!variant.assetId.isEmpty());
		const auto plannedAssociation = association(variant, *Engine::getSong(), rate, variant.frames);

		QDomDocument document;
		auto settings = document.createElement(QStringLiteral("sampleclip"));
		document.appendChild(settings);
		settings.setAttribute(QStringLiteral("pos"), 0);
		settings.setAttribute(QStringLiteral("len"), 8);
		settings.setAttribute(QStringLiteral("off"), 0);
		settings.setAttribute(QStringLiteral("autoresize"), 1);
		settings.appendChild(recording::RecordedTakeSource::writeReference(document, *reference));

		SampleTrack track(Engine::getSong());
		SampleClip clip(&track);
		clip.loadSettings(settings);
		QVERIFY(clip.isRecordedTake());
		QVERIFY(clip.recordedTakeReference());
		QCOMPARE(clip.recordedTakeReference()->takeId, reference->takeId);
		QCOMPARE(clip.applyManagedVocalVariantAssociation(plannedAssociation), QStringLiteral("asset_loading"));
		QTRY_COMPARE_WITH_TIMEOUT(clip.managedVocalVariantStatus(), QStringLiteral("asset_ready"), 5000);
		QCOMPARE(clip.applyManagedVocalVariantAssociation(plannedAssociation), QStringLiteral("ok"));
		QVERIFY(clip.isRecordedTake());
		QVERIFY(clip.recordedTakeReference());
		QCOMPARE(clip.recordedTakeReference()->takeId, reference->takeId);

		QDomDocument savedDocument;
		auto saved = savedDocument.createElement(QStringLiteral("sampleclip"));
		savedDocument.appendChild(saved);
		clip.saveSettings(savedDocument, saved);
		QCOMPARE(saved.firstChildElement(QStringLiteral("recorded_take"))
			.attribute(QStringLiteral("take_id")), reference->takeId);
		QCOMPARE(saved.firstChildElement(QStringLiteral("managed_vocal_variant"))
			.attribute(QStringLiteral("asset_id")), variant.assetId);
		QCOMPARE(saved.attribute(QStringLiteral("src")), QString{});
#endif
	}

	void quarantinesAtChangedTempoAndRequalifiesOnlyWhenWorldIsRestored()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed asset root authorization is Windows-only");
#else
		QTemporaryDir rootDirectory;
		QVERIFY(rootDirectory.isValid());
		ConfigManager::inst()->setValue(QStringLiteral("vocal_assets"), QStringLiteral("managed_root"),
			QDir::fromNativeSeparators(rootDirectory.path()));
		const auto sampleRate = int(Engine::audioEngine()->outputSampleRate());
		const auto variant = createVariant(QStringLiteral("embedded:source-c"), sampleRate);
		QVERIFY(!variant.assetId.isEmpty());

		SampleTrack track(Engine::getSong());
		SampleClip source(&track);
		source.setSampleBuffer(std::make_shared<const SampleBuffer>(
			std::vector<SampleFrame>(8, SampleFrame(0.125f)), sampleRate));
		source.setAutoResize(false);
		source.changeLength(TimePos(1));
		QDomDocument document;
		auto settings = originalClipSettings(document, source, 1);
		settings.appendChild(writeManagedVocalVariantAssociation(document,
			association(variant, *Engine::getSong(), sampleRate, variant.frames)));

		SampleClip restored(&track);
		restored.loadSettings(settings);
		QTRY_COMPARE_WITH_TIMEOUT(restored.managedVocalVariantStatus(), QStringLiteral("ok"), 5000);
		QVERIFY(restored.managedVocalPlaybackView());
		const auto originalTempo = Engine::getSong()->getTempo();
		Engine::getSong()->setTempo(originalTempo + 1);
		QCOMPARE(restored.managedVocalVariantStatus(), QStringLiteral("unsupported_playback_world"));
		QVERIFY(!restored.managedVocalPlaybackView());
		QCOMPARE(restored.length().getTicks(), qint64(1));

		Engine::getSong()->setTempo(originalTempo);
		QCOMPARE(restored.managedVocalVariantStatus(), QStringLiteral("ok"));
		QVERIFY(restored.managedVocalPlaybackView());

		Engine::getSong()->getTimeSigModel().setNumerator(6);
		Engine::getSong()->getTimeSigModel().setDenominator(8);
		QCOMPARE(restored.managedVocalVariantStatus(), QStringLiteral("unsupported_playback_world"));
		QVERIFY(!restored.managedVocalPlaybackView());
		Engine::getSong()->getTimeSigModel().setNumerator(4);
		Engine::getSong()->getTimeSigModel().setDenominator(4);
		QCOMPARE(restored.managedVocalVariantStatus(), QStringLiteral("ok"));
		QVERIFY(restored.managedVocalPlaybackView());

		const auto alternateRate = sampleRate == 44100 ? 48000 : 44100;
		Engine::audioEngine()->setAudioDevice(new ManagedVocalProbeDevice(alternateRate), false);
		QCOMPARE(restored.managedVocalVariantStatus(), QStringLiteral("unsupported_playback_world"));
		QVERIFY(!restored.managedVocalPlaybackView());
		Engine::audioEngine()->setAudioDevice(new ManagedVocalProbeDevice(sampleRate), false);
		QCOMPARE(restored.managedVocalVariantStatus(), QStringLiteral("ok"));
		QVERIFY(restored.managedVocalPlaybackView());
#endif
	}

	void preservesUnknownAssociationMetadataAndNeverFallsBack()
	{
		SampleTrack sourceTrack(Engine::getSong());
		SampleClip source(&sourceTrack);
		const auto sampleRate = int(Engine::audioEngine()->outputSampleRate());
		source.setSampleBuffer(std::make_shared<const SampleBuffer>(
			std::vector<SampleFrame>(8, SampleFrame(0.25f)), sampleRate));
		source.setAutoResize(false);
		source.changeLength(TimePos(1));
		QDomDocument document;
		auto settings = originalClipSettings(document, source, 1);
		auto invalid = document.createElement(QStringLiteral("managed_vocal_variant"));
		invalid.setAttribute(QStringLiteral("schema"), QStringLiteral("99"));
		invalid.setAttribute(QStringLiteral("future_attribute"), QStringLiteral("retain-me"));
		settings.appendChild(invalid);

		SampleTrack restoredTrack(Engine::getSong());
		SampleClip restored(&restoredTrack);
		restored.loadSettings(settings);
		QVERIFY(restored.hasManagedVocalVariant());
		QCOMPARE(restored.managedVocalVariantStatus(), QStringLiteral("invalid_managed_variant"));
		QVERIFY(!restored.managedVocalPlaybackView());
		QVERIFY(!restoredTrack.play(TimePos(0), Engine::audioEngine()->framesPerPeriod(), 0, -1));

		QDomDocument savedDocument;
		auto saved = savedDocument.createElement(QStringLiteral("sampleclip"));
		savedDocument.appendChild(saved);
		restored.saveSettings(savedDocument, saved);
		const auto retained = saved.firstChildElement(QStringLiteral("managed_vocal_variant"));
		QCOMPARE(retained.attribute(QStringLiteral("schema")), QStringLiteral("99"));
		QCOMPARE(retained.attribute(QStringLiteral("future_attribute")), QStringLiteral("retain-me"));
		QCOMPARE(saved.attribute(QStringLiteral("data")), settings.attribute(QStringLiteral("data")));
	}

	void refusesSilentLookupFromAnotherManagedAssetRoot()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed asset root authorization is Windows-only");
#else
		QTemporaryDir sourceRoot;
		QTemporaryDir otherRoot;
		QVERIFY(sourceRoot.isValid() && otherRoot.isValid());
		ConfigManager::inst()->setValue(QStringLiteral("vocal_assets"), QStringLiteral("managed_root"),
			QDir::fromNativeSeparators(sourceRoot.path()));
		const auto sampleRate = int(Engine::audioEngine()->outputSampleRate());
		const auto variant = createVariant(QStringLiteral("embedded:source-d"), sampleRate);
		QVERIFY(!variant.assetId.isEmpty());
		ConfigManager::inst()->setValue(QStringLiteral("vocal_assets"), QStringLiteral("managed_root"),
			QDir::fromNativeSeparators(otherRoot.path()));

		SampleTrack track(Engine::getSong());
		SampleClip source(&track);
		source.setSampleBuffer(std::make_shared<const SampleBuffer>(
			std::vector<SampleFrame>(8, SampleFrame(0.5f)), sampleRate));
		source.setAutoResize(false);
		source.changeLength(TimePos(1));
		QDomDocument document;
		auto settings = originalClipSettings(document, source, 1);
		settings.appendChild(writeManagedVocalVariantAssociation(document,
			association(variant, *Engine::getSong(), sampleRate, variant.frames)));

		SampleTrack restoredTrack(Engine::getSong());
		SampleClip restored(&restoredTrack);
		restored.loadSettings(settings);
		QVERIFY(restored.hasManagedVocalVariant());
		QCOMPARE(restored.managedVocalVariantStatus(), QStringLiteral("asset_root_mismatch"));
		QVERIFY(!restored.managedVocalPlaybackView());
		QCOMPARE(restored.sample().sampleSize(), size_t(8));
		QVERIFY(!restoredTrack.play(TimePos(0), Engine::audioEngine()->framesPerPeriod(), 0, -1));
#endif
	}

	void allowsVerifiedVariantPlaybackWhenTheOriginalReferenceIsMissing()
	{
#ifndef Q_OS_WIN
		QSKIP("Managed asset root authorization is Windows-only");
#else
		QTemporaryDir rootDirectory;
		QTemporaryDir sourceDirectory;
		QVERIFY(rootDirectory.isValid() && sourceDirectory.isValid());
		ConfigManager::inst()->setValue(QStringLiteral("vocal_assets"), QStringLiteral("managed_root"),
			QDir::fromNativeSeparators(rootDirectory.path()));
		const auto sampleRate = int(Engine::audioEngine()->outputSampleRate());
		const auto missingSource = sourceDirectory.filePath(QStringLiteral("missing-vocal.wav"));
		const auto variant = createVariant(missingSource, sampleRate);
		QVERIFY(!variant.assetId.isEmpty());

		QDomDocument document;
		auto settings = document.createElement(QStringLiteral("sampleclip"));
		document.appendChild(settings);
		settings.setAttribute(QStringLiteral("src"), missingSource);
		settings.setAttribute(QStringLiteral("pos"), 0);
		settings.setAttribute(QStringLiteral("len"), 1);
		settings.setAttribute(QStringLiteral("off"), 0);
		settings.setAttribute(QStringLiteral("autoresize"), 0);
		settings.appendChild(writeManagedVocalVariantAssociation(document,
			association(variant, *Engine::getSong(), sampleRate, variant.frames)));

		SampleTrack track(Engine::getSong());
		SampleClip restored(&track);
		restored.loadSettings(settings);
		QCOMPARE(restored.sourceLoadError(), QStringLiteral("audio_source_missing"));
		QCOMPARE(restored.sampleFile(), missingSource);
		QCOMPARE(restored.sample().sampleSize(), size_t(0));
		QTRY_COMPARE_WITH_TIMEOUT(restored.managedVocalVariantStatus(), QStringLiteral("ok"), 5000);
		QVERIFY(restored.managedVocalPlaybackView());
		QVERIFY(track.play(TimePos(0), Engine::audioEngine()->framesPerPeriod(), 0, -1));

		QDomDocument savedDocument;
		auto saved = savedDocument.createElement(QStringLiteral("sampleclip"));
		savedDocument.appendChild(saved);
		restored.saveSettings(savedDocument, saved);
		QCOMPARE(saved.attribute(QStringLiteral("src")), missingSource);
		QCOMPARE(saved.firstChildElement(QStringLiteral("managed_vocal_variant"))
			.attribute(QStringLiteral("asset_id")), variant.assetId);
#endif
	}
};

QTEST_MAIN(SampleClipManagedVocalVariantTest)
#include "SampleClipManagedVocalVariantTest.moc"
