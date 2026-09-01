#include "AiBridgeIpcServer.h"

#include <QLocalSocket>

#include <QJsonObject>
#include <QMetaObject>

#include "AiBridgeActions.h"

namespace lmms
{

AiBridgeIpcServer::AiBridgeIpcServer(QObject *parent, QString endpoint) : QObject(parent), m_server(this), m_endpoint(std::move(endpoint))
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
	return m_server.listen(m_endpoint);
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
	m_busy = false;
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
	if (m_input.size() > AiBridgeProtocol::MaxFrameBytes)
	{
		m_client->close();
		return;
	}
	for (auto newline = m_input.indexOf('\n'); newline >= 0; newline = m_input.indexOf('\n'))
	{
		const auto frame = m_input.left(newline + 1);
		m_input.remove(0, newline + 1);
		if (frame == QByteArrayLiteral("ping\n"))
		{
			m_client->write(QByteArrayLiteral("pong\n"));
			m_client->flush();
			continue;
		}
		const auto parsed = AiBridgeProtocol::parseRequest(frame);
		if (!parsed.success)
		{
			sendResponse({2, parsed.request.requestId, false, parsed.code, parsed.message});
			continue;
		}
		if (m_busy)
		{
			sendResponse({2, parsed.request.requestId, false, QStringLiteral("busy"), QStringLiteral("Another LMMS request is in flight")});
			continue;
		}
		m_busy = true;
		const auto request = parsed.request;
		QMetaObject::invokeMethod(this, [this, request] { executeRequest(request); }, Qt::QueuedConnection);
	}
}

void AiBridgeIpcServer::executeRequest(const AiBridgeProtocolRequest &request)
{
	AiBridgeProtocolResponse response{2, request.requestId, false, QStringLiteral("internal_error"), QStringLiteral("Request did not execute")};
	if (request.action == QStringLiteral("get_context"))
	{
		const auto snapshot = AiBridgeActions::getContextSnapshot();
		QJsonObject data{{QStringLiteral("song_name"), snapshot.songName}, {QStringLiteral("tempo"), snapshot.tempo}, {QStringLiteral("has_current_midi_clip"), snapshot.hasCurrentMidiClip}, {QStringLiteral("clip_length_ticks"), snapshot.clipLengthTicks}, {QStringLiteral("has_owning_instrument_track"), snapshot.hasOwningInstrumentTrack}, {QStringLiteral("track_name"), snapshot.trackName}, {QStringLiteral("track_volume"), snapshot.trackVolume}};
		response = {2, request.requestId, snapshot.success, snapshot.code, snapshot.message, data};
	}
	else
	{
		AiBridgeContext context;
		AiBridgeActionResult result;
		if (request.action == QStringLiteral("set_tempo")) result = AiBridgeActions::setSongTempo(context, request.parameters.value(QStringLiteral("bpm")).toInt());
		else if (request.action == QStringLiteral("adjust_current_track_volume")) result = AiBridgeActions::adjustCurrentInstrumentVolume(context, static_cast<float>(request.parameters.value(QStringLiteral("delta")).toDouble()));
		else result = AiBridgeActions::insertNoteIntoCurrentMidiClip(context, request.parameters.value(QStringLiteral("key")).toInt(), request.parameters.value(QStringLiteral("position")).toInt(), request.parameters.value(QStringLiteral("length")).toInt(), request.parameters.value(QStringLiteral("velocity")).toInt());
		response = {2, request.requestId, result.success, result.code, result.message};
	}
	m_busy = false;
	sendResponse(response);
}

void AiBridgeIpcServer::sendResponse(const AiBridgeProtocolResponse &response)
{
	if (m_client == nullptr || m_client->state() != QLocalSocket::ConnectedState) return;
	m_client->write(AiBridgeProtocol::serializeResponse(response));
	m_client->flush();
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
