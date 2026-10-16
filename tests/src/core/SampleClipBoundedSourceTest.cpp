#include <QtTest>
#include <QDomDocument>
#include <QTemporaryDir>
#include <QFile>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include "AudioImportPolicy.h"

#include "ConfigManager.h"
#include "Engine.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"

class SampleClipBoundedSourceTest : public QObject
{
	Q_OBJECT
	QString oldFileBudget, oldMemoryBudget, oldRoots;
	const QString absent = QStringLiteral("phase3ia-test-absent-setting");
	void budgets(const QString &file, const QString &memory)
	{
		auto *config=lmms::ConfigManager::inst();
		config->setValue("aibridge","audio_import_max_file_bytes",file);
		config->setValue("aibridge","audio_import_max_decoded_bytes",memory);
	}
	QDomElement state(const QString &source, bool bounded=true)
	{
		QDomDocument doc;auto element=doc.createElement("sampleclip");doc.appendChild(element);
		element.setAttribute("src",source);element.setAttribute("pos",300);element.setAttribute("len",40);
		element.setAttribute("off",-10);element.setAttribute("autoresize",0);
		if(bounded)element.setAttribute("bounded_source","1");
		return element;
	}
	QDomElement save(lmms::SampleClip &clip)
	{
		QDomDocument doc;auto element=doc.createElement("sampleclip");doc.appendChild(element);
		clip.saveSettings(doc,element);return element;
	}
	QString fixture() { return QFileInfo(QFINDTESTDATA("../../../data/samples/shapes/vowel_u.wav")).canonicalFilePath(); }
private slots:
	void initTestCase() { lmms::Engine::init(true); }
	void cleanupTestCase() { lmms::Engine::destroy(); }
	void init()
	{
		auto *config=lmms::ConfigManager::inst();
		oldFileBudget=config->value("aibridge","audio_import_max_file_bytes",absent);
		oldMemoryBudget=config->value("aibridge","audio_import_max_decoded_bytes",absent);
		oldRoots=config->value("aibridge","audio_import_roots",absent);
		config->setValue("aibridge","audio_import_roots",QString::fromUtf8(QJsonDocument(QJsonArray{QFileInfo(fixture()).absolutePath()}).toJson(QJsonDocument::Compact)));
	}
	void cleanup()
	{
		auto *config=lmms::ConfigManager::inst();
		if(oldFileBudget==absent)config->deleteValue("aibridge","audio_import_max_file_bytes");
		else config->setValue("aibridge","audio_import_max_file_bytes",oldFileBudget);
		if(oldMemoryBudget==absent)config->deleteValue("aibridge","audio_import_max_decoded_bytes");
		else config->setValue("aibridge","audio_import_max_decoded_bytes",oldMemoryBudget);
		if(oldRoots==absent) config->deleteValue("aibridge","audio_import_roots");
		else config->setValue("aibridge","audio_import_roots",oldRoots);
		lmms::AudioImportPolicy::configured().refresh();
	}
	void noConfiguredBudgetDoesNotDecodeBoundedSource()
	{
		budgets("","");
		lmms::SampleTrack track(lmms::Engine::getSong());lmms::SampleClip clip(&track);
		const auto source=fixture();clip.loadSettings(state(source));
		QCOMPARE(clip.sample().sampleSize(),size_t(0));
		QCOMPARE(clip.sampleFile(),source);
		QCOMPARE(save(clip).attribute("bounded_source"),QStringLiteral("1"));
		QCOMPARE(clip.sourceLoadError(),QStringLiteral("audio_resource_limits_not_configured"));
	}
	void boundedSourceSurvivesReloadAndBudgetReduction()
	{
		budgets("20000","20000");
		lmms::SampleTrack track(lmms::Engine::getSong());lmms::SampleClip clip(&track);
		clip.loadSettings(state(fixture()));QCOMPARE(clip.sample().sampleSize(),size_t(337));
		const auto saved=save(clip);
		QCOMPARE(saved.attribute("bounded_source"),QStringLiteral("1"));
		lmms::SampleClip restored(&track);restored.loadSettings(saved);
		QCOMPARE(restored.sample().sampleSize(),size_t(337));
		QCOMPARE(restored.startPosition().getTicks(),300);QCOMPARE(restored.startTimeOffset().getTicks(),-10);
		budgets("1","20000");restored.loadSettings(saved);
		QCOMPARE(restored.sample().sampleSize(),size_t(0));
		QCOMPARE(restored.sampleFile(),saved.attribute("src"));
		const auto rejectedSave=save(restored);
		QCOMPARE(rejectedSave.attribute("src"),saved.attribute("src"));
		QCOMPARE(rejectedSave.attribute("bounded_source"),QStringLiteral("1"));
		budgets("20000","20000");restored.loadSettings(rejectedSave);
		QCOMPARE(restored.sample().sampleSize(),size_t(337));
	}
	void projectCannotEnlargeLocalBudgetOrSupplyEmbeddedBypass()
	{
		budgets("1","1");
		lmms::SampleTrack track(lmms::Engine::getSong());lmms::SampleClip clip(&track);
		auto settings=state(fixture());
		settings.setAttribute("audio_import_max_file_bytes","18446744073709551615");
		settings.setAttribute("audio_import_max_decoded_bytes","18446744073709551615");
		settings.setAttribute("data",QString::fromLatin1(QByteArray(64,'A').toBase64()));
		clip.loadSettings(settings);
		QCOMPARE(clip.sample().sampleSize(),size_t(0));
		QCOMPARE(clip.sampleFile(),settings.attribute("src"));
	}
	void malformedBoundedMarkerCannotSelectLegacyDecoder()
	{
		budgets("20000","20000");
		lmms::SampleTrack track(lmms::Engine::getSong());lmms::SampleClip clip(&track);
		auto settings=state(fixture());settings.setAttribute("bounded_source","invalid");
		clip.loadSettings(settings);
		QCOMPARE(clip.sample().sampleSize(),size_t(0));
		QCOMPARE(clip.sampleFile(),settings.attribute("src"));
	}
	void savedFingerprintMatchesDecodedBytesAndRejectsChangedSource()
	{
		budgets("20000", "20000");
		const auto source = fixture();
		QFile original(source);
		QVERIFY(original.open(QIODevice::ReadOnly));
		const auto digest = QCryptographicHash::hash(original.readAll(), QCryptographicHash::Sha256).toHex();
		lmms::SampleTrack track(lmms::Engine::getSong());
		lmms::SampleClip clip(&track);
		clip.loadSettings(state(source));
		auto saved = save(clip);
		QCOMPARE(saved.attribute("source_sha256").toLatin1(), digest);
		saved.setAttribute("source_sha256", QString(64, QChar('0')));
		clip.loadSettings(saved);
		QCOMPARE(clip.sample().sampleSize(), size_t(0));
		QCOMPARE(clip.sourceLoadError(), QStringLiteral("audio_source_changed"));
		QCOMPARE(clip.sampleFile(), saved.attribute("src"));
		QCOMPARE(save(clip).attribute("source_sha256"), saved.attribute("source_sha256"));
		// The fixture was not changed: this models a persisted expectation that differs
		// from current bytes without writing a temporary or modifying an original WAV.
		saved.setAttribute("source_sha256", QString::fromLatin1(digest));
		clip.loadSettings(saved);
		QCOMPARE(clip.sample().sampleSize(), size_t(337));
		QVERIFY(clip.sourceLoadError().isEmpty());
	}
	void malformedFingerprintAndEmptyReferenceDoNotKeepOldAudio()
	{
		budgets("20000", "20000");
		lmms::SampleTrack track(lmms::Engine::getSong());
		lmms::SampleClip clip(&track);
		clip.loadSettings(state(fixture()));
		auto settings = save(clip);
		settings.setAttribute("source_sha256", "invalid");
		clip.loadSettings(settings);
		QCOMPARE(clip.sample().sampleSize(), size_t(0));
		QVERIFY(!clip.sourceLoadError().isEmpty());
		clip.loadSettings(save(clip));
		QCOMPARE(clip.sample().sampleSize(), size_t(0));
		QVERIFY(!clip.sourceLoadError().isEmpty());
		clip.loadSettings(state(fixture()));
		clip.loadSettings(state(""));
		QCOMPARE(clip.sample().sampleSize(), size_t(0));
		QCOMPARE(clip.sourceLoadError(), QStringLiteral("audio_source_missing"));
	}
	void explicitBufferReplacementClearsAdmissionOnReusedSaveNode()
	{
		budgets("20000", "20000");
		lmms::SampleTrack track(lmms::Engine::getSong());
		lmms::SampleClip clip(&track);
		clip.loadSettings(state(fixture()));
		auto saved = save(clip);
		clip.setSampleBuffer(std::make_shared<const lmms::SampleBuffer>(std::vector<lmms::SampleFrame>(4), 44100));
		auto document = saved.ownerDocument();
		clip.saveSettings(document, saved);
		QVERIFY(!saved.hasAttribute("bounded_source"));
		QVERIFY(!saved.hasAttribute("source_sha256"));
		QVERIFY(!saved.attribute("data").isEmpty());
	}
	void unmarkedLegacyProjectStillUsesItsNormalLoader()
	{
		budgets("1","1");
		lmms::SampleTrack track(lmms::Engine::getSong());lmms::SampleClip clip(&track);
		clip.loadSettings(state(fixture(),false));
		QCOMPARE(clip.sample().sampleSize(),size_t(337));
		QVERIFY(!save(clip).hasAttribute("bounded_source"));
	}
	void missingBoundedReferenceAndMarkerSurviveRepeatedSave()
	{
		budgets("20000","20000");QTemporaryDir directory;
		const auto source=directory.filePath("missing.wav");
		lmms::SampleTrack track(lmms::Engine::getSong());lmms::SampleClip clip(&track);
		clip.loadSettings(state(source));
		for(int i=0;i<2;++i)
		{
			const auto saved=save(clip);QCOMPARE(saved.attribute("src"),source);
			QCOMPARE(saved.attribute("bounded_source"),QStringLiteral("1"));
			clip.loadSettings(saved);QCOMPARE(clip.sample().sampleSize(),size_t(0));
		}
	}
};
QTEST_MAIN(SampleClipBoundedSourceTest)
#include "SampleClipBoundedSourceTest.moc"
