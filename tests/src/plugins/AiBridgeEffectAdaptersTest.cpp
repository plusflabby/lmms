#include <QtTest>
#include "AiBridgeEffectAdapters.h"
#include "AmplifierControls.h"
#include "CompressorControls.h"
#include "CrossoverEQControls.h"
#include "DelayControls.h"
#include "ReverbSCControls.h"
#include "Engine.h"
#include <type_traits>

class AiBridgeEffectAdaptersTest : public QObject
{
	Q_OBJECT
private slots:
	void initTestCase() { lmms::Engine::init(true); }
	void cleanupTestCase() { lmms::Engine::destroy(); }
	void readsAndWritesConcreteFloatModel()
	{
		lmms::AmplifierControls controls(nullptr);
		controls.volumeModel().setValue(137.5f);
		const auto values = lmms::AiBridgeEffectAdapters::parameters("amplifier", &controls);
		QCOMPARE(values.size(), 4);
		QCOMPARE(values.at(0).id, QString("volume"));
		QCOMPARE(values.at(0).value, 137.5f);
		QVERIFY(lmms::AiBridgeEffectAdapters::setParameter("amplifier", &controls, "volume", 42.0f));
		QCOMPARE(controls.volumeModel().value(), 42.0f);
	}

	void readsAndWritesEveryApprovedAdapter()
	{
		using namespace lmms;
		CompressorControls compressor(nullptr);
		const auto compressorParameters = AiBridgeEffectAdapters::parameters("compressor", &compressor);
		QCOMPARE(compressorParameters.size(), 28);
		QCOMPARE(compressorParameters.at(0).id, QString("threshold"));
		QVERIFY(AiBridgeEffectAdapters::setParameter("compressor", &compressor, "threshold", -12.0f));
		QCOMPARE(compressor.thresholdModel().value(), -12.0f);

		CrossoverEQControls crossover(nullptr);
		const auto crossoverParameters = AiBridgeEffectAdapters::parameters("crossovereq", &crossover);
		QCOMPARE(crossoverParameters.size(), 11);
		QCOMPARE(crossoverParameters.at(0).id, QString("band_1_2_crossover"));
		QVERIFY(AiBridgeEffectAdapters::setParameter("crossovereq", &crossover, "band_1_gain", -12.3f));
		QCOMPARE(crossover.gain1Model().value(), -12.3f);

		DelayControls delay(nullptr);
		const auto delayParameters = AiBridgeEffectAdapters::parameters("delay", &delay);
		QCOMPARE(delayParameters.size(), 5);
		QCOMPARE(delayParameters.at(0).id, QString("delay_time"));
		QVERIFY(AiBridgeEffectAdapters::setParameter("delay", &delay, "delay_time", 1.25f));
		QCOMPARE(delay.delayTimeModel().value(), 1.25f);

		ReverbSCControls reverb(nullptr);
		const auto reverbParameters = AiBridgeEffectAdapters::parameters("reverbsc", &reverb);
		QCOMPARE(reverbParameters.size(), 4);
		QCOMPARE(reverbParameters.at(0).id, QString("input_gain"));
		QVERIFY(AiBridgeEffectAdapters::setParameter("reverbsc", &reverb, "size", 0.5f));
		QCOMPARE(reverb.sizeModel().value(), 0.5f);
	}

	void rejectsUnknownConcreteParameter()
	{
		lmms::AmplifierControls controls(nullptr);
		QVERIFY(!lmms::AiBridgeEffectAdapters::setParameter("amplifier", &controls, "unknown", 1.0f));
	}

	void acceptsNativeValuesDespiteFloatStepRounding()
	{
		using namespace lmms;
		const QVector<AiBridgeEffectParameterMetadata> metadata = {
			{"threshold", "Threshold", AiBridgeEffectParameterType::Float, -8.0f, -60.0f, 0.0f, -8.0f, 0.001f, "dBFS", {}, true},
			{"delay_time", "Delay samples", AiBridgeEffectParameterType::Float, 0.5f, 0.01f, 5.0f, 0.5f, 0.0001f, "seconds", {}, true}};
		QString error;
		QVERIFY(AiBridgeEffectAdapters::validateBatch(metadata, {{"threshold", AiBridgeEffectParameterType::Float, -8.0f}}, &error));
		QVERIFY(AiBridgeEffectAdapters::validateBatch(metadata, {{"delay_time", AiBridgeEffectParameterType::Float, 0.5f}}, &error));
	}

	void exposesExplicitNativeParameterBinding()
	{
		static_assert(std::is_same_v<decltype(lmms::AiBridgeEffectAdapters::modelForParameter(
			QString(), nullptr, QString())), lmms::AutomatableModel *>);
		QVERIFY(true);
	}

};
QTEST_MAIN(AiBridgeEffectAdaptersTest)
#include "AiBridgeEffectAdaptersTest.moc"
