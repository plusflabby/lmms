#include "AiBridgeIpcServer.h"

#include <QLocalSocket>

namespace lmms
{

AiBridgeIpcServer::AiBridgeIpcServer(QObject *parent) : QObject(parent), m_server(this)
{
	connect(&m_server, &QLocalServer::newConnection, this, &AiBridgeIpcServer::acceptConnection);
}

AiBridgeIpcServer::~AiBridgeIpcServer()
{
	stop();
}

bool AiBridgeIpcServer::start()
{
	if (m_server.isListening()) return true;
	return m_server.listen(endpointName());
}

void AiBridgeIpcServer::stop()
{
	if (m_client != nullptr)
	{
		m_client->disconnect(this);
		m_client->close();
		m_client->deleteLater();
		m_client = nullptr;
	}
	m_input.clear();
	m_server.close();
}

void AiBridgeIpcServer::acceptConnection()
{
	while (m_server.hasPendingConnections())
	{
		auto *socket = m_server.nextPendingConnection();
		if (m_client != nullptr)
		{
			socket->close();
			socket->deleteLater();
			continue;
		}
		m_client = socket;
		connect(m_client, &QLocalSocket::readyRead, this, &AiBridgeIpcServer::readProbe);
		connect(m_client, &QLocalSocket::disconnected, this, &AiBridgeIpcServer::clientDisconnected);
	}
}

void AiBridgeIpcServer::readProbe()
{
	if (m_client == nullptr) return;
	m_input += m_client->readAll();
	if (m_input.size() > 64)
	{
		m_client->close();
		return;
	}
	const auto newline = m_input.indexOf('\n');
	if (newline < 0) return;
	const auto frame = m_input.left(newline + 1);
	m_input.remove(0, newline + 1);
	if (frame == QByteArrayLiteral("ping\n"))
	{
		m_client->write(QByteArrayLiteral("pong\n"));
		m_client->flush();
	}
	else
	{
		m_client->close();
	}
}

void AiBridgeIpcServer::clientDisconnected()
{
	if (m_client == nullptr) return;
	const auto *socket = sender();
	if (socket != m_client) return;
	m_client->deleteLater();
	m_client = nullptr;
	m_input.clear();
}

} // namespace lmms
