#include <QtTest>
#include "AiBridgeProtocol.h"

class AiBridgeEffectProtocolTest : public QObject
{
	Q_OBJECT
private slots:
	void acceptsEffectAndMeterRequests()
	{
		for (const auto &frame : {
			QByteArray("{\"version\":2,\"request_id\":\"effect\",\"action\":\"list_native_effects\",\"parameters\":{\"limit\":64}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"effect\",\"action\":\"get_effect_parameters\",\"parameters\":{\"effect_instance_id\":\"e\",\"limit\":128}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"effect\",\"action\":\"get_mixer_meters\",\"parameters\":{\"limit\":64}}\n")})
			QVERIFY(lmms::AiBridgeProtocol::parseRequest(frame).success);
	}
};
QTEST_MAIN(AiBridgeEffectProtocolTest)
#include "AiBridgeEffectProtocolTest.moc"
