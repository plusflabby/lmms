#include <QtTest>

#include "AiBridgeProtocol.h"

class AiBridgeProtocolTest : public QObject
{
	Q_OBJECT

private slots:
	void acceptsAllAllowlistedActions()
	{
		for (const auto test : {QPair<const char *, const char *>("get_context", "{}"), QPair<const char *, const char *>("set_tempo", "{\"bpm\":82}"), QPair<const char *, const char *>("adjust_current_track_volume", "{\"delta\":-10}"), QPair<const char *, const char *>("add_note_to_current_clip", "{\"key\":60,\"position\":0,\"length\":48,\"velocity\":100}")})
		{
			const auto result = lmms::AiBridgeProtocol::parseRequest(
				QByteArray("{\"version\":2,\"request_id\":\"r1\",\"action\":\"") + test.first + "\",\"parameters\":" + test.second + "}\n");
			QVERIFY2(result.success, qPrintable(result.message));
			QCOMPARE(result.request.action, QString::fromLatin1(test.first));
		}
	}

	void rejectsMalformedAndUnknownFields()
	{
		for (const QByteArray frame : {QByteArray("not json\n"), QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"nope\",\"parameters\":{}}\n"), QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"get_context\",\"parameters\":{},\"extra\":1}\n")})
		{
			QVERIFY(!lmms::AiBridgeProtocol::parseRequest(frame).success);
		}
	}

	void enforcesVersionIdTypesAndFrameLimit()
	{
		QVERIFY(!lmms::AiBridgeProtocol::parseRequest("{\"version\":1,\"request_id\":\"r\",\"action\":\"get_context\",\"parameters\":{}}\n").success);
		QVERIFY(!lmms::AiBridgeProtocol::parseRequest("{\"version\":2,\"request_id\":\"\",\"action\":\"get_context\",\"parameters\":{}}\n").success);
		QVERIFY(!lmms::AiBridgeProtocol::parseRequest(QByteArray(65536, 'x')).success);
		QVERIFY(!lmms::AiBridgeProtocol::parseRequest(QByteArray("{\"version\":2,\"request_id\":\"") + QByteArray(129, 'x') + "\",\"action\":\"get_context\",\"parameters\":{}}\n").success);
	}

	void serializesNewlineTerminatedResponse()
	{
		const auto frame = lmms::AiBridgeProtocol::serializeResponse({2, "r1", true, "ok", "done", QJsonValue(42)});
		QVERIFY(frame.endsWith('\n'));
		QVERIFY(frame.contains("\"request_id\":\"r1\""));
		QVERIFY(frame.contains("\"data\":42"));
	}
};

QTEST_MAIN(AiBridgeProtocolTest)
#include "AiBridgeProtocolTest.moc"
