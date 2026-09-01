#include <QtTest>

#include <QLocalSocket>

#include "AiBridgeIpcServer.h"

class AiBridgeIpcServerTest : public QObject
{
	Q_OBJECT

private slots:
	void servesContextRequestWithMatchingId()
	{
		lmms::AiBridgeIpcServer server(nullptr, QStringLiteral("lmms-aibridge-test"));
		QVERIFY(server.start());

		QLocalSocket socket;
		socket.connectToServer(server.endpointName());
		QVERIFY(socket.waitForConnected(1000));
		socket.write("{\"version\":2,\"request_id\":\"r1\",\"action\":\"get_context\",\"parameters\":{}}\n");
		socket.flush();
		QTRY_VERIFY_WITH_TIMEOUT(socket.bytesAvailable() > 0, 1000);
		const auto response = socket.readAll();
		QVERIFY(response.contains("\"request_id\":\"r1\""));
		QVERIFY(response.contains("\"success\":true"));
	}
};

QTEST_MAIN(AiBridgeIpcServerTest)
#include "AiBridgeIpcServerTest.moc"
