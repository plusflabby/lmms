#include <QtTest>
#include <QDomDocument>
#include <QJsonArray>
#include <QJsonDocument>
#include "SampleClip.h"
#include "SampleTrack.h"
#include "Engine.h"
#include "Song.h"
#include "ConfigManager.h"
class SampleClipAudioCopyTest : public QObject
{
	Q_OBJECT
private slots:
	void initTestCase(){lmms::Engine::init(true);}
	void cleanupTestCase(){lmms::Engine::destroy();}
	void rawNativeClonePreservesRangeAndExplicitDestinationOwnsItsCopy()
	{
		using namespace lmms;SampleTrack source(Engine::getSong()),target(Engine::getSong());SampleClip original(&source);
		original.setSampleBuffer(std::make_shared<const SampleBuffer>(std::vector<SampleFrame>(44100),44100));
		original.setName("pre-trimmed");original.setAutoResize(false);original.movePosition(100);original.changeLength(40);original.setStartTimeOffset(-10);original.setMuted(true);original.setSolo(true);original.setColor(QColor("#123456"));
		original.sample().setFrequency(330);original.sample().setAmplification(0.75f);original.sample().setReversed(true);
		std::unique_ptr<SampleClip> raw(original.clone());QCOMPARE(raw->length().getTicks(),40);QCOMPARE(raw->startTimeOffset().getTicks(),-10);
		std::unique_ptr<SampleClip> copied(original.cloneToTrack(&target));QVERIFY(copied);QCOMPARE(copied->getTrack(),static_cast<Track*>(&target));QCOMPARE(copied->parentModel(),static_cast<Model*>(&target));
		QCOMPARE(source.numOfClips(),2);QCOMPARE(target.numOfClips(),1);QCOMPARE(copied->sample().buffer(),original.sample().buffer());
		QCOMPARE(copied->length().getTicks(),40);QCOMPARE(copied->startTimeOffset().getTicks(),-10);QCOMPARE(copied->startPosition().getTicks(),100);
		QVERIFY(copied->isMuted());QVERIFY(copied->isSolo());QCOMPARE(copied->name(),QStringLiteral("pre-trimmed"));QCOMPARE(copied->color(),original.color());
		QCOMPARE(copied->sample().frequency(),330.0f);QCOMPARE(copied->sample().amplification(),0.75f);QVERIFY(copied->sample().reversed());
		copied.reset();QCOMPARE(target.numOfClips(),0);QCOMPARE(original.sample().sampleSize(),size_t(44100));
	}
	void unavailableNativeLinkAndExpectationsSurviveDestinationCopy()
	{
		using namespace lmms;SampleTrack source(Engine::getSong()),target(Engine::getSong());SampleClip original(&source);
		QDomDocument document;auto node=document.createElement("sampleclip");document.appendChild(node);
		node.setAttribute("src","G:/lmms-plus-phase3i/lmms/build-phase3i/absent-copy-source.wav");node.setAttribute("bounded_source","1");node.setAttribute("source_sha256",QString(64,QChar('0')));node.setAttribute("source_file_id",QString(64,QChar('1')));
		node.setAttribute("len",40);node.setAttribute("off",-10);node.setAttribute("autoresize",0);original.loadSettings(node);
		std::unique_ptr<SampleClip> copied(original.cloneToTrack(&target));QVERIFY(copied);
		QCOMPARE(copied->sampleFile(),original.sampleFile());QCOMPARE(copied->expectedSourceDigest(),original.expectedSourceDigest());QCOMPARE(copied->sourceFileIdentity(),original.sourceFileIdentity());QCOMPARE(copied->sourceLoadError(),original.sourceLoadError());
		QVERIFY(copied->usesBoundedSampleSource());QCOMPARE(copied->sample().sampleSize(),size_t(0));
	}
};
QTEST_MAIN(SampleClipAudioCopyTest)
#include "SampleClipAudioCopyTest.moc"
