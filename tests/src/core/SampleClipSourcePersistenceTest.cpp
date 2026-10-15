#include <QtTest>
#include <QTemporaryDir>
#include <QDomDocument>

#include <memory>
#include <vector>

#include "Engine.h"
#include "SampleBuffer.h"
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Song.h"

class SampleClipSourcePersistenceTest : public QObject
{
	Q_OBJECT
private:
	QDomElement state(const QString &source)
	{
		QDomDocument document;
		auto element = document.createElement("sampleclip");
		document.appendChild(element);
		element.setAttribute("src", source);
		element.setAttribute("pos", 100);
		element.setAttribute("len", 40);
		element.setAttribute("off", -10);
		element.setAttribute("autoresize", 0);
		return element;
	}
	QDomElement save(lmms::SampleClip &clip)
	{
		QDomDocument document;
		auto element = document.createElement("sampleclip");
		document.appendChild(element);
		clip.saveSettings(document, element);
		return element;
	}
	QString fixture() { return QFINDTESTDATA("../../../data/samples/shapes/vowel_u.wav"); }
private slots:
	void initTestCase() { lmms::Engine::init(true); }
	void cleanupTestCase() { lmms::Engine::destroy(); }
	void missingReferenceSurvivesRepeatedNativeSaveReload()
	{
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		const auto source = directory.filePath("missing.wav");
		lmms::SampleTrack track(lmms::Engine::getSong());
		lmms::SampleClip clip(&track);
		clip.loadSettings(state(source));
		QCOMPARE(clip.sampleFile(), source);
		QVERIFY(!clip.hasSampleFileLoaded(source));
		QCOMPARE(clip.sample().sampleSize(), size_t(0));
		for (int i = 0; i < 2; ++i)
		{
			const auto saved = save(clip);
			QCOMPARE(saved.attribute("src"), source);
			clip.loadSettings(saved);
			QCOMPARE(clip.sampleFile(), source);
			QCOMPARE(clip.startPosition().getTicks(), 100);
			QCOMPARE(clip.length().getTicks(), 40);
			QCOMPARE(clip.startTimeOffset().getTicks(), -10);
		}
	}
	void missingReferenceDoesNotKeepAnUnrelatedLoadedBuffer()
	{
		QTemporaryDir directory;
		lmms::SampleTrack track(lmms::Engine::getSong());
		lmms::SampleClip clip(&track);
		clip.setSampleFile(fixture());
		QVERIFY(clip.sample().sampleSize() > 0);
		const auto source = directory.filePath("missing.wav");
		clip.loadSettings(state(source));
		QCOMPARE(clip.sample().sampleSize(), size_t(0));
		QCOMPARE(clip.sampleFile(), source);
		QVERIFY(!clip.hasSampleFileLoaded(source));
	}
	void unavailableReferenceSurvivesClone()
	{
		QTemporaryDir directory;
		const auto source = directory.filePath("missing.wav");
		lmms::SampleTrack track(lmms::Engine::getSong());
		lmms::SampleClip clip(&track);
		clip.loadSettings(state(source));
		auto *copy = clip.clone();
		QCOMPARE(copy->sampleFile(), source);
		QVERIFY(!copy->hasSampleFileLoaded(source));
		delete copy;
	}
	void explicitAvailableSourceReplacementWorks()
	{
		QTemporaryDir directory;
		lmms::SampleTrack track(lmms::Engine::getSong());
		lmms::SampleClip clip(&track);
		clip.loadSettings(state(directory.filePath("missing.wav")));
		const auto source = fixture();
		QVERIFY(!source.isEmpty());
		clip.setSampleFile(source);
		QVERIFY(clip.sample().sampleSize() > 0);
		QVERIFY(!clip.sampleFile().isEmpty());
		QVERIFY(clip.hasSampleFileLoaded(clip.sampleFile()));
		QCOMPARE(save(clip).attribute("src"), clip.sampleFile());
		lmms::SampleClip restored(&track);
		restored.loadSettings(save(clip));
		QCOMPARE(restored.sampleFile(), clip.sampleFile());
		QCOMPARE(restored.sample().sampleSize(), clip.sample().sampleSize());
	}
	void legacyEmbeddedFallbackKeepsDataAndIntendedReference()
	{
		QTemporaryDir directory;
		const auto source = directory.filePath("missing.wav");
		lmms::SampleTrack track(lmms::Engine::getSong());
		lmms::SampleClip clip(&track);
		clip.setSampleFile(fixture());
		const auto frames = clip.sample().sampleSize();
		auto settings = state(source);
		settings.setAttribute("data", clip.sample().toBase64());
		settings.setAttribute("sample_rate", clip.sample().sampleRate());
		clip.loadSettings(settings);
		QCOMPARE(clip.sample().sampleSize(), frames);
		QCOMPARE(clip.sampleFile(), source);
		QVERIFY(!clip.hasSampleFileLoaded(source));
		const auto saved = save(clip);
		QCOMPARE(saved.attribute("src"), source);
		QVERIFY(!saved.attribute("data").isEmpty());
		lmms::SampleClip restored(&track);
		restored.loadSettings(saved);
		QCOMPARE(restored.sampleFile(), source);
		QCOMPARE(restored.sample().sampleSize(), frames);
		settings.removeAttribute("src");
		restored.loadSettings(settings);
		QVERIFY(restored.sampleFile().isEmpty());
		QCOMPARE(restored.sample().sampleSize(), frames);
	}
	void explicitBufferReplacementReturnsToEmbeddedBehavior()
	{
		QTemporaryDir directory;
		lmms::SampleTrack track(lmms::Engine::getSong());
		lmms::SampleClip clip(&track);
		clip.loadSettings(state(directory.filePath("missing.wav")));
		std::vector<lmms::SampleFrame> data(4, lmms::SampleFrame{0.25f, 0.25f});
		clip.setSampleBuffer(std::make_shared<const lmms::SampleBuffer>(std::move(data), 44100));
		QVERIFY(clip.sampleFile().isEmpty());
		const auto saved = save(clip);
		QVERIFY(saved.attribute("src").isEmpty());
		QVERIFY(!saved.attribute("data").isEmpty());
		lmms::SampleClip restored(&track);
		restored.loadSettings(saved);
		QCOMPARE(restored.sample().sampleSize(), size_t(4));
		QVERIFY(restored.sampleFile().isEmpty());
	}
};
QTEST_MAIN(SampleClipSourcePersistenceTest)
#include "SampleClipSourcePersistenceTest.moc"
