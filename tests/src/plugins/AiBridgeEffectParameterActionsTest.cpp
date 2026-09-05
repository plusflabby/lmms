#include <QtTest>
#include "AiBridgeEffectAdapters.h"
#include "AmplifierControls.h"
#include "Engine.h"

class AiBridgeEffectParameterActionsTest : public QObject
{
	Q_OBJECT
private slots:
	void initTestCase() { lmms::Engine::init(true); }
	void cleanupTestCase() { lmms::Engine::destroy(); }
	void rejectsInvalidMemberBeforeAnyWrite()
	{
		lmms::AmplifierControls controls(nullptr);
		controls.volumeModel().setValue(75.0f);
		const auto metadata = lmms::AiBridgeEffectAdapters::parameters("amplifier", &controls);
		const QVector<lmms::AiBridgeEffectParameterChange> changes = {
			{"volume", lmms::AiBridgeEffectParameterType::Float, 25.0f},
			{"pan", lmms::AiBridgeEffectParameterType::Float, 101.0f}};
		QString error;
		QVERIFY(!lmms::AiBridgeEffectAdapters::validateBatch(metadata, changes, &error));
		QCOMPARE(error, QString("out_of_range"));
		QCOMPARE(controls.volumeModel().value(), 75.0f);
	}
};
QTEST_MAIN(AiBridgeEffectParameterActionsTest)
#include "AiBridgeEffectParameterActionsTest.moc"
