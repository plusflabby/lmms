#include <QtTest>

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QLocalSocket>

#include "AiBridgeIpcServer.h"
#include "AiBridgeProjectContext.h"
#include "Engine.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Note.h"
#include "Song.h"

class AiBridgeIpcServerTest : public QObject
{
	Q_OBJECT

private slots:
	void initTestCase()
	{
		lmms::Engine::init(true);
	}

	void cleanupTestCase()
	{
		lmms::Engine::destroy();
	}

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

	void servesPhase3HMidiCapabilities()
	{
		lmms::AiBridgeIpcServer server(nullptr, QStringLiteral("lmms-aibridge-midi-capabilities-test"));
		QVERIFY(server.start());

		QLocalSocket socket;
		socket.connectToServer(server.endpointName());
		QVERIFY(socket.waitForConnected(1000));
		socket.write("{\"version\":2,\"request_id\":\"midi-capabilities\",\"action\":\"get_midi_capabilities\",\"parameters\":{}}\n");
		socket.flush();
		QTRY_VERIFY_WITH_TIMEOUT(socket.bytesAvailable() > 0, 1000);
		const auto response = socket.readAll();
		QVERIFY(response.contains("\"request_id\":\"midi-capabilities\""));
		QVERIFY(response.contains("\"success\":true"));
		QVERIFY(response.contains("\"max_clips_per_mutation\":32"));
		QVERIFY(response.contains("\"max_notes_per_page\":512"));
	}

	void servesCopiedMidiNotesAndDescriptiveAnalysis()
	{
		using namespace lmms;
		InstrumentTrack track(Engine::getSong());
		MidiClip clip(&track);
		clip.addNote(Note(TimePos(12), TimePos(24), 64, 90, 0), false);
		clip.addNote(Note(TimePos(24), TimePos(0), 60, 80, 0), false);
		const auto clipId = AiBridgeProjectContext{}.idForClip(&clip);
		AiBridgeIpcServer server(nullptr, QStringLiteral("lmms-aibridge-midi-read-test"));
		QVERIFY(server.start());

		QLocalSocket socket;
		socket.connectToServer(server.endpointName());
		QVERIFY(socket.waitForConnected(1000));
		const auto notesRequest = QByteArray("{\"version\":2,\"request_id\":\"midi-notes\",\"action\":\"get_midi_notes\",\"parameters\":{\"clip_id\":\"") + clipId.toUtf8() + "\",\"offset\":0,\"limit\":1}}\n";
		socket.write(notesRequest);
		socket.flush();
		QTRY_VERIFY_WITH_TIMEOUT(socket.bytesAvailable() > 0, 1000);
		const auto notesResponse = socket.readAll();
		QVERIFY(notesResponse.contains("\"success\":true"));
		QVERIFY(notesResponse.contains("\"total\":2"));
		QVERIFY(notesResponse.contains("\"has_more\":true"));
		QVERIFY(notesResponse.contains("\"midi_state_hash\":"));
		QVERIFY(notesResponse.contains("\"note_ref\":"));

		const auto analysisRequest = QByteArray("{\"version\":2,\"request_id\":\"midi-analysis\",\"action\":\"analyze_midi\",\"parameters\":{\"clip_ids\":[\"") + clipId.toUtf8() + "\"],\"grid_ticks\":24}}\n";
		socket.write(analysisRequest);
		socket.flush();
		QTRY_VERIFY_WITH_TIMEOUT(socket.bytesAvailable() > 0, 1000);
		const auto analysisResponse = socket.readAll();
		QVERIFY(analysisResponse.contains("\"success\":true"));
		QVERIFY(analysisResponse.contains("\"note_count\":2"));
		QVERIFY(analysisResponse.contains("\"grid_deviation\":"));
	}

	void plansEveryPhase3HMidiMutationWithoutNativeWrites()
	{
		using namespace lmms;
		InstrumentTrack track(Engine::getSong());
		MidiClip clip(&track);
		clip.addNote(Note(TimePos(24), TimePos(12), 61, 80, 0), false);
		clip.addNote(Note(TimePos(24), TimePos(36), 64, 100, 0), false);
		const auto clipId = AiBridgeProjectContext{}.idForClip(&clip);
		AiBridgeIpcServer server(nullptr, QStringLiteral("lmms-aibridge-midi-mutation-plan-test"));
		QVERIFY(server.start());

		QLocalSocket socket;
		socket.connectToServer(server.endpointName());
		QVERIFY(socket.waitForConnected(1000));
		auto request = [&socket](const QByteArray &frame) -> QByteArray {
			socket.write(frame);
			socket.flush();
			QElapsedTimer timer;
			timer.start();
			while (socket.bytesAvailable() == 0 && timer.elapsed() < 1000) QTest::qWait(1);
			return socket.readAll();
		};
		const auto read = request(QByteArray("{\"version\":2,\"request_id\":\"read\",\"action\":\"get_midi_notes\",\"parameters\":{\"clip_id\":\"") + clipId.toUtf8() + "\",\"offset\":0}}\n");
		const auto hash = QJsonDocument::fromJson(read).object().value(QStringLiteral("data")).toObject().value(QStringLiteral("midi_state_hash")).toString();
		QVERIFY(!hash.isEmpty());
		const QByteArray clips = QByteArray("\"clips\":[{\"clip_id\":\"") + clipId.toUtf8() + "\",\"expected_midi_state_hash\":\"" + hash.toUtf8() + "\",\"selection\":{\"mode\":\"predicate\",\"start_tick\":0,\"end_tick\":96}}],\"dry_run\":true";
		QVector<QPair<QByteArray, QByteArray>> actions;
		actions.append(qMakePair(QByteArrayLiteral("quantize_notes"), QByteArrayLiteral(",\"grid_ticks\":24")));
		actions.append(qMakePair(QByteArrayLiteral("humanize_notes"), QByteArrayLiteral(",\"seed\":1,\"timing_range_ticks\":1")));
		actions.append(qMakePair(QByteArrayLiteral("apply_groove"), QByteArrayLiteral(",\"mode\":\"swing\",\"subdivision_ticks\":24,\"swing_percent\":75")));
		actions.append(qMakePair(QByteArrayLiteral("shape_note_velocities"), QByteArrayLiteral(",\"mode\":\"offset\",\"value\":1")));
		actions.append(qMakePair(QByteArrayLiteral("shape_note_lengths"), QByteArrayLiteral(",\"mode\":\"offset\",\"value\":1")));
		actions.append(qMakePair(QByteArrayLiteral("cleanup_notes"), QByteArrayLiteral(",\"remove_exact_duplicates\":true")));
		actions.append(qMakePair(QByteArrayLiteral("constrain_notes_to_scale"), QByteArrayLiteral(",\"allowed_pitch_classes\":[0],\"direction\":\"nearest\"")));
		for (const auto &action : actions)
		{
			const auto response = request(QByteArray("{\"version\":2,\"request_id\":\"mutation\",\"action\":\"") + action.first + "\",\"parameters\":{" + clips + action.second + "}}\n");
			QVERIFY2(response.contains("\"success\":true"), response.constData());
			QVERIFY(response.contains("\"dry_run\":true"));
			QVERIFY(response.contains("\"preview\":"));
		}
		QCOMPARE(clip.notes().at(0)->pos().getTicks(), qint64(12));
	}

	void executesPreflightedMidiMutation()
	{
		using namespace lmms;
		InstrumentTrack track(Engine::getSong());
		MidiClip clip(&track);
		clip.addNote(Note(TimePos(24), TimePos(12), 64, 80, 0), false);
		const auto clipId = AiBridgeProjectContext{}.idForClip(&clip);
		AiBridgeIpcServer server(nullptr, QStringLiteral("lmms-aibridge-midi-mutation-execute-test"));
		QVERIFY(server.start());
		QLocalSocket socket;
		socket.connectToServer(server.endpointName());
		QVERIFY(socket.waitForConnected(1000));
		socket.write(QByteArray("{\"version\":2,\"request_id\":\"read\",\"action\":\"get_midi_notes\",\"parameters\":{\"clip_id\":\"") + clipId.toUtf8() + "\",\"offset\":0}}\n");
		socket.flush();
		QTRY_VERIFY_WITH_TIMEOUT(socket.bytesAvailable() > 0, 1000);
		const auto hash = QJsonDocument::fromJson(socket.readAll()).object().value(QStringLiteral("data")).toObject().value(QStringLiteral("midi_state_hash")).toString();
		const auto mutation = QByteArray("{\"version\":2,\"request_id\":\"apply\",\"action\":\"quantize_notes\",\"parameters\":{\"clips\":[{\"clip_id\":\"") + clipId.toUtf8() + "\",\"expected_midi_state_hash\":\"" + hash.toUtf8() + "\",\"selection\":{\"mode\":\"predicate\",\"start_tick\":0,\"end_tick\":48}}],\"grid_ticks\":24}}\n";
		socket.write(mutation);
		socket.flush();
		QTRY_VERIFY_WITH_TIMEOUT(socket.bytesAvailable() > 0, 1000);
		const auto response = socket.readAll();
		QVERIFY2(response.contains("\"success\":true"), response.constData());
		QCOMPARE(clip.notes().at(0)->pos().getTicks(), qint64(0));
	}

	void boundsDryRunPreviewWithoutNativeWrites()
	{
		using namespace lmms;
		InstrumentTrack track(Engine::getSong());
		MidiClip clip(&track);
		for (int index = 0; index < 129; ++index)
			clip.addNote(Note(TimePos(12), TimePos(index * 24 + 1), 60, 80, 0), false);
		const auto clipId = AiBridgeProjectContext{}.idForClip(&clip);
		AiBridgeIpcServer server(nullptr, QStringLiteral("lmms-aibridge-midi-preview-bound-test"));
		QVERIFY(server.start());
		QLocalSocket socket;
		socket.connectToServer(server.endpointName());
		QVERIFY(socket.waitForConnected(1000));
		auto request = [&socket](const QByteArray &frame) -> QJsonObject {
			socket.write(frame); socket.flush(); QElapsedTimer timer; timer.start();
			while (socket.bytesAvailable() == 0 && timer.elapsed() < 1000) QTest::qWait(1);
			if (socket.bytesAvailable() == 0) return {};
			return QJsonDocument::fromJson(socket.readAll()).object();
		};
		const auto read = request(QByteArray("{\"version\":2,\"request_id\":\"read\",\"action\":\"get_midi_notes\",\"parameters\":{\"clip_id\":\"") + clipId.toUtf8() + "\",\"offset\":0}}\n");
		const auto hash = read.value(QStringLiteral("data")).toObject().value(QStringLiteral("midi_state_hash")).toString();
		const auto response = request(QByteArray("{\"version\":2,\"request_id\":\"dry\",\"action\":\"quantize_notes\",\"parameters\":{\"clips\":[{\"clip_id\":\"") + clipId.toUtf8() + "\",\"expected_midi_state_hash\":\"" + hash.toUtf8() + "\",\"selection\":{\"mode\":\"predicate\",\"start_tick\":0,\"end_tick\":4096}}],\"grid_ticks\":24,\"dry_run\":true}}\n");
		const auto result = response.value(QStringLiteral("data")).toObject().value(QStringLiteral("clips")).toArray().at(0).toObject();
		QCOMPARE(result.value(QStringLiteral("changed_count")).toInt(), 129);
		QCOMPARE(result.value(QStringLiteral("preview")).toArray().size(), 128);
		QVERIFY(result.value(QStringLiteral("preview_has_more")).toBool());
		QCOMPARE(clip.notes().at(0)->pos().getTicks(), qint64(1));
	}
};

QTEST_MAIN(AiBridgeIpcServerTest)
#include "AiBridgeIpcServerTest.moc"
