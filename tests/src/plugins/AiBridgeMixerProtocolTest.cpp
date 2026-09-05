#include <QtTest>
#include "AiBridgeProtocol.h"

class AiBridgeMixerProtocolTest : public QObject
{
	Q_OBJECT
private slots:
	void acceptsBoundedMixerRequests()
	{
		const auto frame = QByteArray("{\"version\":2,\"request_id\":\"mixer\",\"action\":\"get_mixer_structure\",\"parameters\":{\"offset\":0,\"limit\":256}}\n");
		QVERIFY(lmms::AiBridgeProtocol::parseRequest(frame).success);
		QVERIFY(!lmms::AiBridgeProtocol::parseRequest(QByteArray("{\"version\":2,\"request_id\":\"mixer\",\"action\":\"get_mixer_structure\",\"parameters\":{\"limit\":257}}\n")).success);
	}
};
QTEST_MAIN(AiBridgeMixerProtocolTest)
#include "AiBridgeMixerProtocolTest.moc"
