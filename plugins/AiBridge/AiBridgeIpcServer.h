#ifndef LMMS_AIBRIDGE_IPC_SERVER_H
#define LMMS_AIBRIDGE_IPC_SERVER_H

#include <QByteArray>
#include <QLocalServer>
#include <QLocalSocket>

#include "AiBridgeProtocol.h"

namespace lmms
{

class AiBridgeIpcServer : public QObject
{
public:
	explicit AiBridgeIpcServer(QObject *parent = nullptr, QString endpoint = QStringLiteral("lmms-aibridge-v2"));
	~AiBridgeIpcServer() override;

	bool start();
	void stop();
	bool isListening() const { return m_server.isListening(); }
	QString endpointName() const { return m_endpoint; }

private:
	void acceptConnection();
	void readProbe();
	void clientDisconnected();
	void executeRequest(const AiBridgeProtocolRequest &request);
	void sendResponse(const AiBridgeProtocolResponse &response);

	QLocalServer m_server;
	QLocalSocket *m_client = nullptr;
	QByteArray m_input;
	bool m_busy = false;
	QString m_endpoint;
};

} // namespace lmms

#endif // LMMS_AIBRIDGE_IPC_SERVER_H
