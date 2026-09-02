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
		for (const auto action : {"get_project_structure", "list_tracks", "list_clips", "get_clip"})
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
