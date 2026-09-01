#include "AiBridgeIpcServer.h"

#include <QLocalSocket>

#include <QJsonObject>
#include <QJsonArray>
#include <QMetaObject>

#include "AiBridgeActions.h"
#include "AiBridgeProjectActions.h"
#include "AiBridgeWindowActions.h"
#include "AiBridgeWindowContext.h"

namespace lmms
{

namespace
{
QJsonObject trackJson(const AiBridgeTrackValue& v)
{
	QJsonObject o{{"id",v.id},{"name",v.name},{"type",v.type},{"index",v.index},{"clip_count",v.clipCount}};
	if(v.hasVolume)o.insert("volume",v.volume);
	return o;
}
QJsonObject clipJson(const AiBridgeClipValue& v)
{
	return {{"id",v.id},{"track_id",v.trackId},{"name",v.name},{"type",v.type},{"start_ticks",v.startTicks},{"length_ticks",v.lengthTicks},{"note_count",v.noteCount},{"current_in_piano_roll",v.currentInPianoRoll}};
}
QJsonObject pageJson(const AiBridgePage& p)
{
	return {{"total",p.total},{"offset",p.offset},{"limit",p.limit},{"has_more",p.hasMore}};
}

AiBridgeCopiedNote copiedNote(const QJsonValue &value)
{
	const auto object = value.toObject();
	AiBridgeCopiedNote note;
	note.position_ticks = object.value(QStringLiteral("position_ticks")).toVariant().toLongLong();
	note.length_ticks = object.value(QStringLiteral("length_ticks")).toVariant().toLongLong();
	note.key = object.value(QStringLiteral("key")).toInt();
	note.volume = object.value(QStringLiteral("volume")).toInt();
	note.panning = object.value(QStringLiteral("panning")).toInt();
	note.note_type = object.value(QStringLiteral("note_type")).toString();
	return note;
}

AiBridgeNoteSelection selection(const QJsonObject &object)
{
	AiBridgeNoteSelection value;
	if (object.contains(QStringLiteral("position_start"))) value.position_start = object.value(QStringLiteral("position_start")).toVariant().toLongLong();
	if (object.contains(QStringLiteral("position_end"))) value.position_end = object.value(QStringLiteral("position_end")).toVariant().toLongLong();
	if (object.contains(QStringLiteral("key_min"))) value.key_min = object.value(QStringLiteral("key_min")).toInt();
	if (object.contains(QStringLiteral("key_max"))) value.key_max = object.value(QStringLiteral("key_max")).toInt();
	if (object.contains(QStringLiteral("exact")))
	{
		const auto exactObject = object.value(QStringLiteral("exact")).toObject();
		AiBridgeExactNote exact;
		if (exactObject.contains(QStringLiteral("position"))) exact.position = exactObject.value(QStringLiteral("position")).toVariant().toLongLong();
		if (exactObject.contains(QStringLiteral("length"))) exact.length = exactObject.value(QStringLiteral("length")).toVariant().toLongLong();
		if (exactObject.contains(QStringLiteral("key"))) exact.key = exactObject.value(QStringLiteral("key")).toInt();
		if (exactObject.contains(QStringLiteral("volume"))) exact.volume = exactObject.value(QStringLiteral("volume")).toInt();
		if (exactObject.contains(QStringLiteral("panning"))) exact.panning = exactObject.value(QStringLiteral("panning")).toInt();
		if (exactObject.contains(QStringLiteral("note_type"))) exact.note_type = exactObject.value(QStringLiteral("note_type")).toString();
		value.exact = exact;
	}
	return value;
}

QJsonObject mutationJson(const AiBridgeMutationResult &value)
{
	return {{"clip_id", value.clip_id}, {"track_id", value.track_id},
		{"notes_added", value.notes_added}, {"notes_deleted", value.notes_deleted},
		{"notes_changed", value.notes_changed},
		{"old_content_hash", value.old_content_hash},
		{"new_content_hash", value.new_content_hash},
		{"clip_length_ticks", value.clip_length_ticks},
		{"current_in_piano_roll", value.current_in_piano_roll}};
}
}

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
		const auto snapshot = AiBridgeProjectContext{}.currentContext();
		QJsonObject data{{QStringLiteral("song_name"), snapshot.songName}, {QStringLiteral("tempo"), snapshot.tempo}, {QStringLiteral("has_current_midi_clip"), snapshot.hasCurrentMidiClip}, {QStringLiteral("clip_id"), snapshot.clipId}, {QStringLiteral("clip_length_ticks"), snapshot.clipLengthTicks}, {QStringLiteral("has_owning_instrument_track"), snapshot.hasOwningInstrumentTrack}, {QStringLiteral("track_id"), snapshot.trackId}, {QStringLiteral("track_name"), snapshot.trackName}, {QStringLiteral("track_volume"), snapshot.trackVolume}};
		response = {2, request.requestId, snapshot.success, snapshot.code, snapshot.message, data};
	}
	else if (request.action == QStringLiteral("get_windows"))
	{
		const auto snapshot = AiBridgeWindowContext{}.snapshot();
		QJsonArray windows;
		for (const auto& item : snapshot.windows)
		{
			windows.append(QJsonObject{{QStringLiteral("id"), item.id}, {QStringLiteral("kind"), item.kind}, {QStringLiteral("title"), item.title}, {QStringLiteral("x"), item.geometry.x}, {QStringLiteral("y"), item.geometry.y}, {QStringLiteral("width"), item.geometry.width}, {QStringLiteral("height"), item.geometry.height}, {QStringLiteral("coordinate_space"), item.coordinateSpace}, {QStringLiteral("visible"), item.visible}, {QStringLiteral("active"), item.active}, {QStringLiteral("detached"), item.detached}, {QStringLiteral("maximized"), item.maximized}, {QStringLiteral("minimized"), item.minimized}, {QStringLiteral("overlaps"), QJsonArray::fromStringList(item.overlaps)}});
		}
		response = {2, request.requestId, snapshot.success, snapshot.code, snapshot.message, QJsonObject{{QStringLiteral("success"), snapshot.success}, {QStringLiteral("code"), snapshot.code}, {QStringLiteral("geometry"), QJsonObject{{QStringLiteral("width"), snapshot.geometry.width}, {QStringLiteral("height"), snapshot.geometry.height}}}, {QStringLiteral("windows"), windows}}};
	}
	else
	{
		if (request.action == QStringLiteral("get_project_structure") || request.action == QStringLiteral("list_tracks") || request.action == QStringLiteral("list_clips") || request.action == QStringLiteral("get_clip") || request.action == QStringLiteral("open_clip_in_piano_roll"))
		{
			const auto& p = request.parameters;
			const int offset = p.value(QStringLiteral("offset")).toInt();
			const int limit = p.contains(QStringLiteral("limit")) ? p.value(QStringLiteral("limit")).toInt() : 64;
			AiBridgeProjectActionResult result;
			if (request.action == QStringLiteral("get_project_structure")) result = AiBridgeProjectActions::getProjectStructure();
			else if (request.action == QStringLiteral("list_tracks")) result = AiBridgeProjectActions::listTracks(offset,limit);
			else if (request.action == QStringLiteral("list_clips")) result = AiBridgeProjectActions::listClips(p.value(QStringLiteral("track_id")).toString(),offset,limit);
			else if (request.action == QStringLiteral("get_clip")) result = AiBridgeProjectActions::getClip(p.value(QStringLiteral("clip_id")).toString(),offset,limit);
			else result = AiBridgeProjectActions::openClipInPianoRoll(p.value(QStringLiteral("clip_id")).toString());
			QJsonObject data;
			if (request.action == QStringLiteral("get_project_structure"))
			{
				QJsonArray tracks; for (const auto& item : result.project.tracks) tracks.append(trackJson(item));
				data = {{"project_generation", static_cast<qint64>(AiBridgeProjectContext{}.projectGeneration())}, {"song", QJsonObject{{"name",result.project.songName},{"tempo",result.project.tempo},{"length_bars",result.project.lengthBars},{"length_ticks",result.project.lengthTicks},{"track_count",result.project.trackCount}}}, {"tracks",tracks}};
			}
			else if (request.action == QStringLiteral("list_tracks")) { QJsonArray items; for(const auto& item:result.tracks)items.append(trackJson(item)); data=pageJson(result.page);data.insert("tracks",items); }
			else if (request.action == QStringLiteral("list_clips")) { QJsonArray items; for(const auto& item:result.clips)items.append(clipJson(item)); data=pageJson(result.page);data.insert("clips",items); }
			else { data=clipJson(result.clip); data.insert("content_hash",result.clip.contentHash); data.insert("auto_resize",result.clip.autoResize); data.insert("total",result.page.total);data.insert("offset",result.page.offset);data.insert("limit",result.page.limit);data.insert("has_more",result.page.hasMore); QJsonArray notes;for(const auto& n:result.clip.notes)notes.append(QJsonObject{{"position",n.position},{"length",n.length},{"key",n.key},{"volume",n.volume},{"panning",n.panning},{"note_type",n.noteType}});data.insert("notes",notes); if(request.action==QStringLiteral("open_clip_in_piano_roll")){const auto& c=result.context;data.insert("context",QJsonObject{{"clip_id",c.clipId},{"track_id",c.trackId},{"track_name",c.trackName},{"project_generation",static_cast<qint64>(c.projectGeneration)}});} }
			response = {2,request.requestId,result.success,result.code,result.message,data};
		}
		else
		{
		if (request.action == QStringLiteral("add_notes") || request.action == QStringLiteral("delete_notes") || request.action == QStringLiteral("transpose_notes") || request.action == QStringLiteral("move_notes") || request.action == QStringLiteral("duplicate_region"))
		{
			const auto &p = request.parameters;
			AiBridgeProjectActionResult result;
			const auto id = p.value(QStringLiteral("clip_id")).toString();
			const auto expected = p.value(QStringLiteral("expected_content_hash")).toString();
			if (request.action == QStringLiteral("add_notes"))
			{
				QVector<AiBridgeCopiedNote> notes;
				for (const auto &value : p.value(QStringLiteral("notes")).toArray()) notes.append(copiedNote(value));
				result = AiBridgeProjectActions::addNotes(id, notes, expected);
			}
			else if (request.action == QStringLiteral("delete_notes"))
				result = AiBridgeProjectActions::deleteNotes(id, selection(p.value(QStringLiteral("selection")).toObject()), expected);
			else if (request.action == QStringLiteral("transpose_notes"))
				result = AiBridgeProjectActions::transposeNotes(id, selection(p.value(QStringLiteral("selection")).toObject()), p.value(QStringLiteral("semitones")).toInt(), expected);
			else if (request.action == QStringLiteral("move_notes"))
				result = AiBridgeProjectActions::moveNotes(id, selection(p.value(QStringLiteral("selection")).toObject()), p.value(QStringLiteral("delta_ticks")).toVariant().toLongLong(), expected);
			else
				result = AiBridgeProjectActions::duplicateRegion(id, p.value(QStringLiteral("source_start")).toVariant().toLongLong(), p.value(QStringLiteral("source_end")).toVariant().toLongLong(), p.value(QStringLiteral("destination_start")).toVariant().toLongLong(), expected);
			response = {2, request.requestId, result.success, result.code, result.message, mutationJson(result.mutation)};
		}
		else
		{
		AiBridgeContext context;
		AiBridgeActionResult result;
		if (request.action == QStringLiteral("set_tempo")) result = AiBridgeActions::setSongTempo(context, request.parameters.value(QStringLiteral("bpm")).toInt());
		else if (request.action == QStringLiteral("adjust_current_track_volume")) result = AiBridgeActions::adjustCurrentInstrumentVolume(context, static_cast<float>(request.parameters.value(QStringLiteral("delta")).toDouble()));
		else if (request.action == QStringLiteral("add_note_to_current_clip")) result = AiBridgeActions::insertNoteIntoCurrentMidiClip(context, request.parameters.value(QStringLiteral("key")).toInt(), request.parameters.value(QStringLiteral("position")).toInt(), request.parameters.value(QStringLiteral("length")).toInt(), request.parameters.value(QStringLiteral("velocity")).toInt());
		else
		{
			AiBridgeWindowContext windowContext;
			if (request.action == QStringLiteral("focus_window")) result = AiBridgeWindowActions::focusWindow(windowContext, request.parameters.value(QStringLiteral("id")).toString());
			else if (request.action == QStringLiteral("set_window_geometry")) result = AiBridgeWindowActions::setWindowGeometry(windowContext, request.parameters.value(QStringLiteral("id")).toString(), {request.parameters.value(QStringLiteral("x")).toInt(), request.parameters.value(QStringLiteral("y")).toInt(), request.parameters.value(QStringLiteral("width")).toInt(), request.parameters.value(QStringLiteral("height")).toInt()});
			else result = AiBridgeWindowActions::arrangeWindows(windowContext, request.parameters.value(QStringLiteral("strategy")).toString());
		}
		response = {2, request.requestId, result.success, result.code, result.message};
		}
		}
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
