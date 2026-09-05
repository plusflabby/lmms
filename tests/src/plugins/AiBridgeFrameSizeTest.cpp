#include <QtTest>
#include "AiBridgeProtocol.h"

class AiBridgeFrameSizeTest : public QObject
{
	Q_OBJECT
private slots:
	void oversizedCompleteResponsesBecomeBoundedErrors()
	{
		for (const auto action : {"get_project_structure", "list_native_effects", "get_effect_parameters", "get_mixer_meters"})
		{
			const auto frame = lmms::AiBridgeProtocol::serializeResponse({2, action, true, "ok", "done", QJsonObject{{"copied_payload", QString(70000, QLatin1Char('x'))}}});
			QVERIFY(frame.size() <= lmms::AiBridgeProtocol::MaxFrameBytes);
			QVERIFY(frame.contains("\"code\":\"result_too_large\""));
			QVERIFY(!frame.contains("copied_payload"));
		}
	}
};
QTEST_MAIN(AiBridgeFrameSizeTest)
#include "AiBridgeFrameSizeTest.moc"
