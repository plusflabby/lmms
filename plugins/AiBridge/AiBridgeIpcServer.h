#ifndef LMMS_AIBRIDGE_IPC_SERVER_H
#define LMMS_AIBRIDGE_IPC_SERVER_H

#include <QByteArray>
#include <QLocalServer>
#include <QLocalSocket>

namespace lmms
{

class AiBridgeIpcServer : public QObject
{
public:
	explicit AiBridgeIpcServer(QObject *parent = nullptr);
	~AiBridgeIpcServer() override;

	bool start();
	void stop();
	bool isListening() const { return m_server.isListening(); }
	QString endpointName() const { return QStringLiteral("lmms-aibridge-v2"); }

private:
	void acceptConnection();
	void readProbe();
	void clientDisconnected();

	QLocalServer m_server;
	QLocalSocket *m_client = nullptr;
	QByteArray m_input;
};

} // namespace lmms

#endif // LMMS_AIBRIDGE_IPC_SERVER_H
