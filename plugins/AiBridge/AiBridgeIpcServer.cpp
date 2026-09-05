#include "AiBridgeIpcServer.h"

#include <QLocalSocket>

#include <QJsonObject>
#include <QJsonArray>
#include <QMetaObject>
#include <optional>

#include "AiBridgeActions.h"
#include "AiBridgeProjectActions.h"
#include "AiBridgeWindowActions.h"
#include "AiBridgeWindowContext.h"
#include "AiBridgeAssetTypes.h"
#include "AiBridgeMixerActions.h"
#include "AiBridgeMixerContext.h"
#include "AiBridgeMixerRegistry.h"
#include "AiBridgeEffectActions.h"
#include "AiBridgeEffectCatalog.h"
#include "AiBridgeEffectTypes.h"
#include "AiBridgeMeterActions.h"

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
QJsonObject constructionTrackJson(const AiBridgeTrackConstructionValue &v)
{
	return {{"track_id",v.trackId},{"name",v.name},{"type",v.type},{"index",v.index},{"volume",v.volume},{"panning",v.panning}};
}
QJsonObject constructionClipJson(const AiBridgeClipConstructionValue &v)
{
	return {{"clip_id",v.clipId},{"track_id",v.trackId},{"name",v.name},{"type",v.type},{"start_ticks",v.startTicks},{"length_ticks",v.lengthTicks},{"auto_resize",v.autoResize},{"content_hash",v.contentHash}};
}
QJsonObject trackPropertiesJson(const AiBridgeTrackPropertyValue &v)
{
	return {{"track_id",v.trackId},{"volume",v.volume},{"panning",v.panning}};
}
QJsonObject clipPropertiesJson(const AiBridgeClipPropertyValue &v)
{
	return {{"clip_id",v.clipId},{"track_id",v.trackId},{"start_ticks",v.startTicks},{"length_ticks",v.lengthTicks},{"auto_resize",v.autoResize},{"content_hash",v.contentHash}};
}
QJsonObject instrumentJson(const AiBridgeInstrumentValue &v)
{
	return {{"instrument_id",v.instrumentId},{"display_name",v.displayName},{"description",v.description},{"available",v.available}};
}

QJsonObject samplePageJson(const AiBridgeAssetPage<AiBridgeSampleValue> &page)
{
	QJsonArray items;
	for (const auto &item : page.items)
		items.append(QJsonObject{{"asset_id", item.assetId}, {"display_name", item.displayName}, {"instrument_id", item.instrumentId}, {"category", item.category}, {"format", item.format}});
	return QJsonObject{{"items", items}, {"total", page.total}, {"offset", page.offset}, {"limit", page.limit}, {"has_more", page.hasMore}};
}

QJsonObject presetPageJson(const AiBridgeAssetPage<AiBridgePresetValue> &page)
{
	QJsonArray items;
	for (const auto &item : page.items)
		items.append(QJsonObject{{"asset_id", item.assetId}, {"display_name", item.displayName}, {"instrument_id", item.instrumentId}, {"category", item.category}, {"format", item.format}});
	return QJsonObject{{"items", items}, {"total", page.total}, {"offset", page.offset}, {"limit", page.limit}, {"has_more", page.hasMore}};
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

QJsonObject mixerPageJson(const AiBridgeMixerPage &page)
{
	return {{"total", page.total}, {"offset", page.offset}, {"limit", page.limit}, {"has_more", page.hasMore}};
}

QJsonObject effectSummaryJson(const AiBridgeMixerEffectSummary &value)
{
	return {{"effect_instance_id", value.effectInstanceId}, {"effect_id", value.effectId}, {"name", value.name}, {"position", value.position}};
}

QJsonObject channelJson(const AiBridgeMixerChannelSnapshot &value)
{
	QJsonArray tracks; for (const auto &id : value.trackIds) tracks.append(id);
	QJsonArray effects; for (const auto &effect : value.effects) effects.append(effectSummaryJson(effect));
	return {{"mixer_channel_id", value.mixerChannelId}, {"name", value.name}, {"master", value.master}, {"volume", value.volume}, {"mute", value.mute}, {"solo", value.solo}, {"track_ids", tracks}, {"effects", effects}};
}

QJsonObject routeJson(const AiBridgeMixerRouteSnapshot &value)
{
	return {{"mixer_route_id", value.mixerRouteId}, {"source_channel_id", value.sourceChannelId}, {"destination_channel_id", value.destinationChannelId}, {"amount", value.amount}};
}

QJsonObject mixerStructureJson(const AiBridgeMixerStructureSnapshot &value)
{
	QJsonArray channels; for (const auto &item : value.channelItems) channels.append(channelJson(item));
	QJsonArray routes; for (const auto &item : value.routeItems) routes.append(routeJson(item));
	return {{"channels", channels}, {"channel_page", mixerPageJson(value.channels)}, {"routes", routes}, {"route_page", mixerPageJson(value.routes)}};
}

QString parameterTypeName(AiBridgeEffectParameterType type)
{
	switch (type) { case AiBridgeEffectParameterType::Float: return "float"; case AiBridgeEffectParameterType::Integer: return "integer"; case AiBridgeEffectParameterType::Boolean: return "boolean"; case AiBridgeEffectParameterType::Enum: return "enum"; }
	return "float";
}

QJsonObject parameterJson(const AiBridgeEffectParameterMetadata &value)
{
	QJsonArray enums; for (const auto &item : value.enumValues) enums.append(item);

	return {{"id", value.id}, {"name", value.name}, {"type", parameterTypeName(value.type)}, {"value", value.value}, {"minimum", value.minimum}, {"maximum", value.maximum}, {"default", value.defaultValue}, {"step", value.step}, {"unit", value.unit}, {"enum_values", enums}, {"writable", value.writable}};
}

QJsonObject effectActionJson(const AiBridgeEffectActionResult &value)
{
	QJsonArray effects; for (const auto &item : value.effects) effects.append(effectSummaryJson(item));
	return {{"effect_instance_id", value.effectInstanceId}, {"effect_id", value.effectId}, {"mixer_channel_id", value.mixerChannelId}, {"position", value.position}, {"enabled", value.enabled}, {"wet_dry", value.wetDry}, {"effects", effects}};
}

QJsonObject parameterPageJson(const AiBridgeEffectParameterActionResult &value)
{
	QJsonArray items; for (const auto &item : value.page.items) items.append(parameterJson(item));
	QJsonObject result = mixerPageJson(value.page.page); result.insert("effect_instance_id", value.effectInstanceId); result.insert("parameters", items); return result;
}

QJsonObject meterPageJson(const AiBridgeMeterActionResult &value)
{
	QJsonArray items; for (const auto &item : value.page.items) items.append(QJsonObject{{"mixer_channel_id", item.mixerChannelId}, {"left_peak", item.leftPeak}, {"right_peak", item.rightPeak}});
	QJsonObject result = mixerPageJson(value.page.page); result.insert("meters", items); return result;
}

bool isMixerAction(const QString &action)
{
	return action == "get_mixer_structure" || action == "create_mixer_channel" || action == "delete_mixer_channel" || action == "set_mixer_channel_properties" || action == "route_track_to_mixer_channel" || action == "create_mixer_route" || action == "delete_mixer_route" || action == "set_mixer_route_properties";
}

bool isEffectAction(const QString &action)
{
	return action == "list_native_effects" || action == "add_effect" || action == "remove_effect" || action == "move_effect" || action == "set_effect_state" || action == "get_effect_parameters" || action == "set_effect_parameters" || action == "get_mixer_meters";
}

AiBridgeEffectParameterType parameterType(const QString &value)
{
	if (value == "integer") return AiBridgeEffectParameterType::Integer;
	if (value == "boolean") return AiBridgeEffectParameterType::Boolean;
	if (value == "enum") return AiBridgeEffectParameterType::Enum;
	return AiBridgeEffectParameterType::Float;
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
		if (request.action == QStringLiteral("get_project_structure") || request.action == QStringLiteral("list_tracks") || request.action == QStringLiteral("list_clips") || request.action == QStringLiteral("get_clip") || request.action == QStringLiteral("open_clip_in_piano_roll") || request.action == QStringLiteral("list_instruments") || request.action == QStringLiteral("create_instrument_track") || request.action == QStringLiteral("rename_track") || request.action == QStringLiteral("create_midi_clip") || request.action == QStringLiteral("set_clip_properties") || request.action == QStringLiteral("set_track_instrument") || request.action == QStringLiteral("set_track_properties") || request.action == QStringLiteral("list_bundled_samples") || request.action == QStringLiteral("load_bundled_sample") || request.action == QStringLiteral("list_bundled_presets") || request.action == QStringLiteral("load_bundled_preset"))
		{
			const auto& p = request.parameters;
			const int offset = p.value(QStringLiteral("offset")).toInt();
			const int limit = p.contains(QStringLiteral("limit")) ? p.value(QStringLiteral("limit")).toInt() : 64;
			AiBridgeProjectActionResult result;
			AiBridgeAssetFilter assetFilter;
			assetFilter.instrumentId = p.value(QStringLiteral("instrument_id")).toString();
			assetFilter.query = p.value(QStringLiteral("query")).toString();
			assetFilter.category = p.value(QStringLiteral("category")).toString();
			assetFilter.format = p.value(QStringLiteral("format")).toString();
			assetFilter.offset = offset;
			assetFilter.limit = limit;
			if (request.action == QStringLiteral("get_project_structure")) result = AiBridgeProjectActions::getProjectStructure();
			else if (request.action == QStringLiteral("list_tracks")) result = AiBridgeProjectActions::listTracks(offset,limit);
			else if (request.action == QStringLiteral("list_clips")) result = AiBridgeProjectActions::listClips(p.value(QStringLiteral("track_id")).toString(),offset,limit);
			else if (request.action == QStringLiteral("get_clip")) result = AiBridgeProjectActions::getClip(p.value(QStringLiteral("clip_id")).toString(),offset,limit);
			else if (request.action == QStringLiteral("open_clip_in_piano_roll")) result = AiBridgeProjectActions::openClipInPianoRoll(p.value(QStringLiteral("clip_id")).toString());
			else if (request.action == QStringLiteral("list_instruments")) result = AiBridgeProjectActions::listInstruments(offset,limit);
			else if (request.action == QStringLiteral("create_instrument_track")) result = AiBridgeProjectActions::createInstrumentTrack(p.contains(QStringLiteral("name")) ? std::optional<QString>(p.value(QStringLiteral("name")).toString()) : std::nullopt);
			else if (request.action == QStringLiteral("rename_track")) result = AiBridgeProjectActions::renameTrack(p.value(QStringLiteral("track_id")).toString(),p.value(QStringLiteral("name")).toString());
			else if (request.action == QStringLiteral("create_midi_clip")) result = AiBridgeProjectActions::createMidiClip(p.value(QStringLiteral("track_id")).toString(),p.value(QStringLiteral("start_ticks")).toVariant().toLongLong(),p.contains(QStringLiteral("length_ticks")) ? std::optional<qint64>(p.value(QStringLiteral("length_ticks")).toVariant().toLongLong()) : std::nullopt,p.value(QStringLiteral("auto_resize")).toBool(false));
			else if (request.action == QStringLiteral("set_clip_properties")) result = AiBridgeProjectActions::setClipProperties(p.value(QStringLiteral("clip_id")).toString(),p.contains(QStringLiteral("start_ticks")) ? std::optional<qint64>(p.value(QStringLiteral("start_ticks")).toVariant().toLongLong()) : std::nullopt,p.contains(QStringLiteral("length_ticks")) ? std::optional<qint64>(p.value(QStringLiteral("length_ticks")).toVariant().toLongLong()) : std::nullopt,p.contains(QStringLiteral("auto_resize")) ? std::optional<bool>(p.value(QStringLiteral("auto_resize")).toBool()) : std::nullopt,p.contains(QStringLiteral("expected_content_hash")) ? std::optional<QString>(p.value(QStringLiteral("expected_content_hash")).toString()) : std::nullopt,p.contains(QStringLiteral("expected_start_ticks")) ? std::optional<qint64>(p.value(QStringLiteral("expected_start_ticks")).toVariant().toLongLong()) : std::nullopt);
			else if (request.action == QStringLiteral("list_bundled_samples")) result = AiBridgeProjectActions::listBundledSamples(assetFilter);
			else if (request.action == QStringLiteral("list_bundled_presets")) result = AiBridgeProjectActions::listBundledPresets(assetFilter);
			else if (request.action == QStringLiteral("load_bundled_sample")) result = AiBridgeProjectActions::loadBundledSample(p.value(QStringLiteral("track_id")).toString(), p.value(QStringLiteral("asset_id")).toString());
			else if (request.action == QStringLiteral("load_bundled_preset")) result = AiBridgeProjectActions::loadBundledPreset(p.value(QStringLiteral("track_id")).toString(), p.value(QStringLiteral("asset_id")).toString());
			else if (request.action == QStringLiteral("set_track_instrument")) result = AiBridgeProjectActions::setTrackInstrument(p.value(QStringLiteral("track_id")).toString(),p.value(QStringLiteral("instrument_id")).toString());
			else result = AiBridgeProjectActions::setTrackProperties(p.value(QStringLiteral("track_id")).toString(),p.contains(QStringLiteral("volume")) ? std::optional<int>(p.value(QStringLiteral("volume")).toInt()) : std::nullopt,p.contains(QStringLiteral("panning")) ? std::optional<int>(p.value(QStringLiteral("panning")).toInt()) : std::nullopt);
			QJsonObject data;
			if (request.action == QStringLiteral("get_project_structure"))
			{
				QJsonArray tracks; for (const auto& item : result.project.tracks) tracks.append(trackJson(item));
				data = {{"project_generation", static_cast<qint64>(AiBridgeProjectContext{}.projectGeneration())}, {"song", QJsonObject{{"name",result.project.songName},{"tempo",result.project.tempo},{"length_bars",result.project.lengthBars},{"length_ticks",result.project.lengthTicks},{"track_count",result.project.trackCount}}}, {"tracks",tracks}};
			}
			else if (request.action == QStringLiteral("list_tracks")) { QJsonArray items; for(const auto& item:result.tracks)items.append(trackJson(item)); data=pageJson(result.page);data.insert("tracks",items); }
			else if (request.action == QStringLiteral("list_clips")) { QJsonArray items; for(const auto& item:result.clips)items.append(clipJson(item)); data=pageJson(result.page);data.insert("clips",items); }
			else if (request.action == QStringLiteral("list_instruments")) { QJsonArray items; for(const auto& item:result.instruments)items.append(instrumentJson(item)); data=pageJson(result.page);data.insert("instruments",items); }
			else if (request.action == QStringLiteral("create_instrument_track") || request.action == QStringLiteral("rename_track") || request.action == QStringLiteral("set_track_instrument")) data=constructionTrackJson(result.track);
			else if (request.action == QStringLiteral("create_midi_clip")) data=constructionClipJson(result.constructionClip);
			else if (request.action == QStringLiteral("set_clip_properties")) data=clipPropertiesJson(result.clipProperties);
			else if (request.action == QStringLiteral("set_track_properties")) data=trackPropertiesJson(result.trackProperties);
			else if (request.action == QStringLiteral("list_bundled_samples")) data=samplePageJson(result.samples);
			else if (request.action == QStringLiteral("list_bundled_presets")) data=presetPageJson(result.presets);
			else if (request.action == QStringLiteral("load_bundled_sample") || request.action == QStringLiteral("load_bundled_preset")) data=QJsonObject{{"asset_id",result.assetLoad.assetId},{"track_id",result.assetLoad.trackId},{"track_name",result.assetLoad.trackName},{"instrument_id",result.assetLoad.instrumentId}};
			else { data=clipJson(result.clip); data.insert("content_hash",result.clip.contentHash); data.insert("auto_resize",result.clip.autoResize); data.insert("total",result.page.total);data.insert("offset",result.page.offset);data.insert("limit",result.page.limit);data.insert("has_more",result.page.hasMore); QJsonArray notes;for(const auto& n:result.clip.notes)notes.append(QJsonObject{{"position",n.position},{"length",n.length},{"key",n.key},{"volume",n.volume},{"panning",n.panning},{"note_type",n.noteType}});data.insert("notes",notes); if(request.action==QStringLiteral("open_clip_in_piano_roll")){const auto& c=result.context;data.insert("context",QJsonObject{{"clip_id",c.clipId},{"track_id",c.trackId},{"track_name",c.trackName},{"project_generation",static_cast<qint64>(c.projectGeneration)}});} }
			response = {2,request.requestId,result.success,result.code,result.message,data};
		}
		else if (isMixerAction(request.action))
		{
			const auto &p = request.parameters;
			AiBridgeProtocolResponse mixerResponse{2, request.requestId, false, "internal_error", "mixer request did not execute"};
			if (request.action == "get_mixer_structure")
			{
				const auto structure = AiBridgeMixerContext(aiBridgeMixerRegistry()).snapshot(p.value("offset").toInt(), p.contains("limit") ? p.value("limit").toInt() : 64);
				mixerResponse = {2, request.requestId, true, "ok", "mixer structure read", mixerStructureJson(structure)};
			}
			else
			{
				AiBridgeMixerActionResult result;
				if (request.action == "create_mixer_channel") result = AiBridgeMixerActions::createChannel(p.contains("name") ? std::optional<QString>(p.value("name").toString()) : std::nullopt);
				else if (request.action == "delete_mixer_channel") result = AiBridgeMixerActions::deleteChannel(p.value("mixer_channel_id").toString());
				else if (request.action == "set_mixer_channel_properties")
				{
					AiBridgeMixerChannelProperties properties;
					if (p.contains("name")) properties.name = p.value("name").toString();
					if (p.contains("volume")) properties.volume = static_cast<float>(p.value("volume").toDouble());
					if (p.contains("mute")) properties.mute = p.value("mute").toBool();
					if (p.contains("solo")) properties.solo = p.value("solo").toBool();
					result = AiBridgeMixerActions::setChannelProperties(p.value("mixer_channel_id").toString(), properties);
				}
				else if (request.action == "route_track_to_mixer_channel") result = AiBridgeMixerActions::routeTrackToChannel(p.value("track_id").toString(), p.value("mixer_channel_id").toString());
				else if (request.action == "create_mixer_route") result = AiBridgeMixerActions::createRoute(p.value("source_channel_id").toString(), p.value("destination_channel_id").toString(), static_cast<float>(p.value("amount").toDouble()));
				else if (request.action == "delete_mixer_route") result = AiBridgeMixerActions::deleteRoute(p.value("mixer_route_id").toString());
				else { AiBridgeMixerRouteProperties properties; properties.amount = static_cast<float>(p.value("amount").toDouble()); result = AiBridgeMixerActions::setRouteProperties(p.value("mixer_route_id").toString(), properties); }
				QJsonObject data = !result.route.mixerRouteId.isEmpty() ? routeJson(result.route) : channelJson(result.channel);
				if (!result.trackId.isEmpty()) data.insert("track_id", result.trackId);
				mixerResponse = {2, request.requestId, result.success, result.code, result.message, data};
			}
			response = mixerResponse;
		}
		else if (isEffectAction(request.action))
		{
			const auto &p = request.parameters;
			if (request.action == "list_native_effects")
			{
				const auto page = AiBridgeEffectCatalog::list(p.value("offset").toInt(), p.contains("limit") ? p.value("limit").toInt() : 64, p.value("filter").toString());
				QJsonArray items; for (const auto &item : page.items) items.append(QJsonObject{{"effect_id", item.id}, {"name", item.name}, {"description", item.description}});
				response = {2, request.requestId, true, "ok", "effect catalog read", QJsonObject{{"effects", items}, {"total", page.page.total}, {"offset", page.page.offset}, {"limit", page.page.limit}, {"has_more", page.page.hasMore}}};
			}
			else if (request.action == "get_mixer_meters")
			{
				std::optional<QVector<QString>> requested;
				if (p.contains("mixer_channel_ids")) { QVector<QString> ids; for (const auto &value : p.value("mixer_channel_ids").toArray()) ids.append(value.toString()); requested = ids; }
				const auto result = AiBridgeMeterActions::get(requested, p.value("offset").toInt(), p.contains("limit") ? p.value("limit").toInt() : 64);
				response = {2, request.requestId, result.success, result.code, result.message, meterPageJson(result)};
			}
			else if (request.action == "get_effect_parameters" || request.action == "set_effect_parameters")
			{
				AiBridgeEffectParameterActionResult result;
				const auto id = p.value("effect_instance_id").toString();
				if (request.action == "get_effect_parameters") result = AiBridgeEffectActions::getParameters(id, p.value("offset").toInt(), p.contains("limit") ? p.value("limit").toInt() : 128);
				else { QVector<AiBridgeEffectParameterChange> changes; for (const auto &value : p.value("changes").toArray()) { const auto object = value.toObject(); changes.append({object.value("id").toString(), parameterType(object.value("type").toString()), static_cast<float>(object.value("value").toDouble())}); } result = AiBridgeEffectActions::setParameters(id, changes); }
				response = {2, request.requestId, result.success, result.code, result.message, parameterPageJson(result)};
			}
			else
			{
				AiBridgeEffectActionResult result;
				if (request.action == "add_effect") result = AiBridgeEffectActions::add(p.value("mixer_channel_id").toString(), p.value("effect_id").toString(), p.value("position").toInt());
				else if (request.action == "remove_effect") result = AiBridgeEffectActions::remove(p.value("effect_instance_id").toString());
				else if (request.action == "move_effect") result = AiBridgeEffectActions::move(p.value("effect_instance_id").toString(), p.value("position").toInt());
				else result = AiBridgeEffectActions::setState(p.value("effect_instance_id").toString(), p.value("enabled").toBool(), static_cast<float>(p.value("wet_dry").toDouble()));
				response = {2, request.requestId, result.success, result.code, result.message, effectActionJson(result)};
			}
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
