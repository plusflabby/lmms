#include <QtTest>
#include <QJsonDocument>
#include <QJsonArray>

#include "AiBridgeProtocol.h"

class AiBridgeProtocolTest : public QObject
{
	Q_OBJECT

private slots:
	void acceptsOnlyTheApprovedStrictAudioWireSurface()
	{
		auto parse=[](const QString& action,const QJsonObject& p){return lmms::AiBridgeProtocol::parseRequest(QJsonDocument(QJsonObject{{"version",2},{"request_id","audio"},{"action",action},{"parameters",p}}).toJson(QJsonDocument::Compact)+'\n');};
		const QString hash(64,'a');
		const QJsonObject edit{{"clip_id","clip"},{"expected_audio_state_hash",hash}};
		QVector<QPair<QString,QJsonObject>> cases{{"get_audio_capabilities",{}},{"get_audio_clip",{{"clip_id","clip"}}},{"create_audio_track",{{"name","Voice"},{"dry_run",true}}},{"import_audio_file",{{"track_id","track"},{"path","G:/approved/Verse.wav"},{"position_ticks",3840},{"dry_run",true}}}};
		auto move=edit;move.insert("position_ticks",10);auto copy=move;copy.insert("destination_track_id","track");
		cases.append({"move_audio_clip",move});cases.append({"duplicate_audio_clip",copy});cases.append({"delete_audio_clip",edit});
		auto op=move;op.insert("operation","move");cases.append({"batch_edit_audio_clips",{{"operations",QJsonArray{op}},{"dry_run",true}}});
		for(const auto& row:cases){auto accepted=parse(row.first,row.second);QVERIFY2(accepted.success,qPrintable(row.first+":"+accepted.code));auto bad=row.second;bad.insert("unknown",true);QVERIFY(!parse(row.first,bad).success);bad=row.second;bad.insert("path","G:/private.wav");if(row.first!="import_audio_file")QVERIFY(!parse(row.first,bad).success);}
		for(const auto& action:{"load_audio_file","set_audio_clip"})QCOMPARE(parse(action,{}).code,QStringLiteral("unknown_action"));
		for(const auto& action:{"trim_audio_clip","split_audio_clip"})QCOMPARE(parse(action,{}).code,QStringLiteral("invalid_parameter"));
		for(const auto& key:{"clip_id","expected_audio_state_hash"}){auto bad=move;bad.remove(key);QVERIFY(!parse("move_audio_clip",bad).success);}
		for(const QJsonValue value:{QJsonValue(-1),QJsonValue(0.5),QJsonValue(2147483648.0),QJsonValue("10")}){auto bad=move;bad.insert("position_ticks",value);QVERIFY(!parse("move_audio_clip",bad).success);}
		auto bad=move;bad.insert("dry_run","true");QVERIFY(!parse("move_audio_clip",bad).success);
		bad=cases[3].second;bad.insert("path",QString(1025,'x'));QVERIFY(!parse("import_audio_file",bad).success);
		for(const auto& operation:{"import","trim","split","batch","shell"}){op.insert("operation",operation);QVERIFY(!parse("batch_edit_audio_clips",{{"operations",QJsonArray{op}}}).success);}
		QJsonArray operations;for(int i=0;i<257;++i)operations.append(QJsonObject{{"operation","delete"},{"clip_id","clip"},{"expected_audio_state_hash",hash}});QVERIFY(!parse("batch_edit_audio_clips",{{"operations",operations}}).success);
	}
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

	void validatesMidiExportRequestsWithoutAcceptingPaths()
	{
		auto parse=[](const QString& action,const QJsonObject& parameters){
			return lmms::AiBridgeProtocol::parseRequest(QJsonDocument(QJsonObject{{"version",2},{"request_id","midi-export"},{"action",action},{"parameters",parameters}}).toJson(QJsonDocument::Compact)+'\n');
		};
		const QString id="93ad2d63-8335-4af8-b9df-ab7728132619";
		QVERIFY(parse("inspect_midi_export",{{"operation_id",id}}).success);
		QVERIFY(parse("export_midi",{{"operation_id",id},{"filename","arrangement.mid"}}).success);
		QVERIFY(parse("get_midi_export_job",{{"mode","status"},{"operation_id",id}}).success);
		QVERIFY(parse("get_midi_export_job",{{"mode","list"}}).success);
		QVERIFY(parse("get_midi_export_job",{{"mode","release"},{"operation_id",id}}).success);
		QVERIFY(parse("cancel_midi_export_job",{{"operation_id",id}}).success);
		QVERIFY(!parse("export_midi",{{"operation_id",id},{"filename","../private.mid"}}).success);
		QVERIFY(!parse("export_midi",{{"operation_id",id},{"filename","song.mid"},{"path","G:/private.mid"}}).success);
		const QJsonObject duplicateSelection{{"mode","tracks"},{"track_ids",QJsonArray{"track","track"}}};
		const QJsonObject reversedRange{{"start_tick",192},{"end_tick",96}};
		QVERIFY(!parse("inspect_midi_export",{{"operation_id",id},{"selection",duplicateSelection}}).success);
		QVERIFY(!parse("inspect_midi_export",{{"operation_id",id},{"range",reversedRange}}).success);
		QVERIFY(!parse("get_midi_export_job",{{"mode","list"},{"operation_id",id}}).success);
	}

	void acceptsPhase3CMutationActions()
	{
		const auto requests = {
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"add_notes\",\"parameters\":{\"clip_id\":\"c\",\"notes\":[{\"position_ticks\":0,\"length_ticks\":120,\"key\":60,\"volume\":100,\"panning\":0,\"note_type\":\"regular\"}]}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"delete_notes\",\"parameters\":{\"clip_id\":\"c\",\"selection\":{\"position_start\":0,\"position_end\":120}}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"transpose_notes\",\"parameters\":{\"clip_id\":\"c\",\"selection\":{\"key_min\":0,\"key_max\":127},\"semitones\":1}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"move_notes\",\"parameters\":{\"clip_id\":\"c\",\"selection\":{\"position_start\":0,\"position_end\":120},\"delta_ticks\":1}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"duplicate_region\",\"parameters\":{\"clip_id\":\"c\",\"source_start\":0,\"source_end\":120,\"destination_start\":120}}\n")
		};
		for (const auto &frame : requests)
			QVERIFY2(lmms::AiBridgeProtocol::parseRequest(frame).success, frame.constData());
	}

	void acceptsPhase3DConstructionActions()
	{
		const auto requests = {
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"create_instrument_track\",\"parameters\":{\"name\":\"Strings\"}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"rename_track\",\"parameters\":{\"track_id\":\"t\",\"name\":\"Strings\"}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"create_midi_clip\",\"parameters\":{\"track_id\":\"t\",\"start_ticks\":0}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"set_clip_properties\",\"parameters\":{\"clip_id\":\"c\",\"start_ticks\":120}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"list_instruments\",\"parameters\":{\"limit\":16}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"set_track_instrument\",\"parameters\":{\"track_id\":\"t\",\"instrument_id\":\"tripleoscillator\"}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"set_track_properties\",\"parameters\":{\"track_id\":\"t\",\"volume\":100}}\n")
		};
		for (const auto &frame : requests)
			QVERIFY2(lmms::AiBridgeProtocol::parseRequest(frame).success, frame.constData());
	}

	void acceptsPhase3EAssetActions()
	{
		const auto requests = {
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"list_bundled_samples\",\"parameters\":{\"instrument_id\":\"audiofileprocessor\",\"limit\":64}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"list_bundled_presets\",\"parameters\":{}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"load_bundled_sample\",\"parameters\":{\"track_id\":\"t\",\"asset_id\":\"sample/audiofileprocessor/a.wav\"}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"load_bundled_preset\",\"parameters\":{\"track_id\":\"t\",\"asset_id\":\"preset/zynaddsubfx/a.xiz\"}}\n")
		};
		for (const auto &frame : requests)
			QVERIFY2(lmms::AiBridgeProtocol::parseRequest(frame).success, frame.constData());
	}

	void acceptsPhase3FMixerActions()
	{
		const auto requests = {
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"get_mixer_structure\",\"parameters\":{\"offset\":0,\"limit\":16}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"create_mixer_channel\",\"parameters\":{\"name\":\"Bus\"}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"delete_mixer_channel\",\"parameters\":{\"mixer_channel_id\":\"m\"}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"set_mixer_channel_properties\",\"parameters\":{\"mixer_channel_id\":\"m\",\"volume\":1}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"route_track_to_mixer_channel\",\"parameters\":{\"track_id\":\"t\",\"mixer_channel_id\":\"m\"}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"create_mixer_route\",\"parameters\":{\"source_channel_id\":\"a\",\"destination_channel_id\":\"b\",\"amount\":0.5}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"delete_mixer_route\",\"parameters\":{\"mixer_route_id\":\"route\"}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"set_mixer_route_properties\",\"parameters\":{\"mixer_route_id\":\"route\",\"amount\":0.5}}\n")
		};
		for (const auto &frame : requests)
			QVERIFY2(lmms::AiBridgeProtocol::parseRequest(frame).success, frame.constData());
	}

	void acceptsPhase3FEffectAndMeterActions()
	{
		const auto requests = {
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"list_native_effects\",\"parameters\":{\"offset\":0,\"limit\":5,\"filter\":\"amp\"}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"add_effect\",\"parameters\":{\"mixer_channel_id\":\"m\",\"effect_id\":\"amplifier\",\"position\":0}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"remove_effect\",\"parameters\":{\"effect_instance_id\":\"e\"}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"move_effect\",\"parameters\":{\"effect_instance_id\":\"e\",\"position\":0}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"set_effect_state\",\"parameters\":{\"effect_instance_id\":\"e\",\"enabled\":true,\"wet_dry\":0}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"get_effect_parameters\",\"parameters\":{\"effect_instance_id\":\"e\",\"offset\":0,\"limit\":128}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"set_effect_parameters\",\"parameters\":{\"effect_instance_id\":\"e\",\"changes\":[{\"id\":\"volume\",\"type\":\"float\",\"value\":1}]}}\n"),
			QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"get_mixer_meters\",\"parameters\":{\"mixer_channel_ids\":[\"m\"],\"offset\":0,\"limit\":64}}\n")
		};
		for (const auto &frame : requests)
			QVERIFY2(lmms::AiBridgeProtocol::parseRequest(frame).success, frame.constData());
	}

	void rejectsMalformedAndUnknownFields()
	{
		for (const QByteArray frame : {QByteArray("not json\n"), QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"nope\",\"parameters\":{}}\n"), QByteArray("{\"version\":2,\"request_id\":\"r\",\"action\":\"get_context\",\"parameters\":{},\"extra\":1}\n")})
			QVERIFY(!lmms::AiBridgeProtocol::parseRequest(frame).success);
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

	void replacesOversizedResultsWithBoundedError()
	{
		for (const auto action : {"get_project_structure", "list_tracks", "list_clips", "get_clip", "list_native_effects", "get_effect_parameters", "get_mixer_meters"})
		{
			QJsonObject data{{"action", action}, {"items", QString(70000, QLatin1Char('x'))}};
			const auto frame = lmms::AiBridgeProtocol::serializeResponse({2, action, true, "ok", "done", data});
			QVERIFY(frame.size() <= lmms::AiBridgeProtocol::MaxFrameBytes);
			QVERIFY(frame.endsWith('\n'));
			QVERIFY(frame.contains("\"success\":false"));
			QVERIFY(frame.contains("\"code\":\"result_too_large\""));
			QVERIFY(!frame.contains("70000"));
		}
	}

	void preservesRequestIdOnParameterFailure()
	{
		const auto result = lmms::AiBridgeProtocol::parseRequest(
			"{\"version\":2,\"request_id\":\"bad-selection\",\"action\":\"transpose_notes\",\"parameters\":{\"clip_id\":\"c\",\"selection\":{\"exact\":{\"key\":60}},\"semitones\":12}}\n");
		QVERIFY(!result.success);
		QCOMPARE(result.request.requestId, QStringLiteral("bad-selection"));
	}
};

QTEST_MAIN(AiBridgeProtocolTest)
#include "AiBridgeProtocolTest.moc"
