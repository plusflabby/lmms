#include <QtTest>

#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QFile>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QtEndian>
#include <cstring>
#include "ConfigManager.h"
#include "AudioImportPolicy.h"
#include "SampleTrack.h"
#include "SampleClip.h"

#include "AiBridgeIpcServer.h"
#include "AiBridgeVocalJobs.h"
#include "AiBridgeProjectContext.h"
#include "Engine.h"
#include "Instrument.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Note.h"
#include "Track.h"
#include "Song.h"

class IpcFailingAudioCopy : public lmms::SampleClip
{
public:
	using SampleClip::SampleClip;
	mutable int attempts=0;
	lmms::SampleClip* cloneToTrack(lmms::SampleTrack*) const override {++attempts;return nullptr;}
};

class AiBridgeIpcServerTest : public QObject
{
	Q_OBJECT

private slots:
	void serializesNativeAudioFailureSlicesAndPostVerificationWithoutReplay()
	{
		using namespace lmms;SampleTrack track(Engine::getSong()),target(Engine::getSong());SampleClip first(&track),last(&track);IpcFailingAudioCopy failure(&track);
		for(auto* clip:{&first,&last,static_cast<SampleClip*>(&failure)}){clip->setSampleBuffer(std::make_shared<const SampleBuffer>(std::vector<SampleFrame>(64),44100));clip->setAutoResize(false);clip->changeLength(40);}
		first.movePosition(100);failure.movePosition(300);last.movePosition(500);AiBridgeProjectContext ids;
		AiBridgeIpcServer server(nullptr,"lmms-audio-wire-failure-test");QVERIFY(server.start());QLocalSocket socket;socket.connectToServer(server.endpointName());QVERIFY(socket.waitForConnected(1000));
		auto call=[&](const QString& action,const QJsonObject& p){socket.write(QJsonDocument(QJsonObject{{"version",2},{"request_id","failure"},{"action",action},{"parameters",p}}).toJson(QJsonDocument::Compact)+'\n');socket.flush();QElapsedTimer timer;timer.start();QByteArray bytes;while(!bytes.endsWith('\n') && timer.elapsed()<3000){QTest::qWait(1);bytes+=socket.readAll();}return QJsonDocument::fromJson(bytes).object();};
		auto op=[&](SampleClip* clip,const QString& kind,int position){auto id=ids.idForClip(clip);auto read=call("get_audio_clip",{{"clip_id",id}}).value("data").toObject();QJsonObject result{{"operation",kind},{"clip_id",id},{"expected_audio_state_hash",read.value("audio_state_hash")}};if(kind!="delete")result.insert("position_ticks",position);if(kind=="duplicate")result.insert("destination_track_id",ids.idForTrack(&target));return result;};
		auto result=call("batch_edit_audio_clips",{{"operations",QJsonArray{op(&first,"move",700),op(&failure,"duplicate",100),op(&last,"delete",0)}}});
		QCOMPARE(result.value("code").toString(),QStringLiteral("partial_failure"));auto data=result.value("data").toObject();QCOMPARE(data.value("applied").toArray(),QJsonArray{0});QCOMPARE(data.value("failed").toArray(),QJsonArray{1});QCOMPARE(data.value("unapplied").toArray(),QJsonArray{2});QCOMPARE(failure.attempts,1);QCOMPARE(first.startPosition().getTicks(),700);QVERIFY(ids.resolveClip(ids.idForClip(&last)));QCOMPARE(target.numOfClips(),0);
		connect(&first,&Clip::positionChanged,&first,[&]{first.setMuted(true);});
		result=call("batch_edit_audio_clips",{{"operations",QJsonArray{op(&first,"move",900),op(&last,"delete",0)}}});QCOMPARE(result.value("code").toString(),QStringLiteral("post_verification_failed"));data=result.value("data").toObject();QCOMPARE(data.value("failed").toArray(),QJsonArray{0});QCOMPARE(data.value("unapplied").toArray(),QJsonArray{1});QCOMPARE(first.startPosition().getTicks(),900);QVERIFY(first.isMuted());QVERIFY(!data.value("expected").toArray().isEmpty());QVERIFY(!data.value("observed").toArray().isEmpty());QCOMPARE(failure.attempts,1);
	}
	void servesAuthorizedAudioPlansAndNativeMutationsWithoutReplay()
	{
		using namespace lmms;
		const auto nativeReady=qEnvironmentVariable("LMMS_PHASE3JA_HOST_READY");
		const bool nativeHost=!nativeReady.isEmpty();
		QTemporaryDir root(QCoreApplication::applicationDirPath()+"/audio-ipc-XXXXXX");QVERIFY(root.isValid());
		const QStringList keys{"audio_import_roots","audio_import_max_file_bytes","audio_import_max_decoded_bytes"};QStringList old;
		for(const auto& key:keys)old<<ConfigManager::inst()->value("aibridge",key,"absent");
		auto restore=qScopeGuard([&]{for(int i=0;i<keys.size();++i){if(old[i]=="absent")ConfigManager::inst()->deleteValue("aibridge",keys[i]);else ConfigManager::inst()->setValue("aibridge",keys[i],old[i]);}AudioImportPolicy::configured().refresh();});
		const int pcmBytes=88200*(nativeHost?120:1);
		QByteArray wav(pcmBytes+44,0);std::memcpy(wav.data(),"RIFF",4);std::memcpy(wav.data()+8,"WAVEfmt ",8);std::memcpy(wav.data()+36,"data",4);
		qToLittleEndian<quint32>(pcmBytes+36,wav.data()+4);qToLittleEndian<quint32>(16,wav.data()+16);qToLittleEndian<quint16>(1,wav.data()+20);qToLittleEndian<quint16>(1,wav.data()+22);qToLittleEndian<quint32>(44100,wav.data()+24);qToLittleEndian<quint32>(88200,wav.data()+28);qToLittleEndian<quint16>(2,wav.data()+32);qToLittleEndian<quint16>(16,wav.data()+34);qToLittleEndian<quint32>(pcmBytes,wav.data()+40);
		const auto path=root.filePath("Verse.wav");{QFile f(path);QVERIFY(f.open(QIODevice::WriteOnly|QIODevice::NewOnly));QCOMPARE(f.write(wav),qint64(wav.size()));}
		ConfigManager::inst()->setValue("aibridge",keys[0],QString::fromUtf8(QJsonDocument(QJsonArray{root.path()}).toJson(QJsonDocument::Compact)));for(int i=1;i<3;++i)ConfigManager::inst()->setValue("aibridge",keys[i],nativeHost?"134217728":"1000000");
		QCOMPARE(AudioImportPolicy::configured().refresh(), QStringLiteral("ok"));
		AiBridgeVocalJobs vocalJobs;
		AiBridgeIpcServer server(nullptr,nativeHost?"lmms-aibridge-v2":"lmms-audio-wire-test",&vocalJobs);QVERIFY(server.start());QLocalSocket socket;socket.connectToServer(server.endpointName());QVERIFY(socket.waitForConnected(1000));
		auto call=[&](const QString& action,QJsonObject p){socket.write(QJsonDocument(QJsonObject{{"version",2},{"request_id","audio"},{"action",action},{"parameters",p}}).toJson(QJsonDocument::Compact)+'\n');socket.flush();QElapsedTimer t;t.start();QByteArray bytes;while(!bytes.endsWith('\n') && t.elapsed()<3000){QTest::qWait(1);bytes+=socket.readAll();}return QJsonDocument::fromJson(bytes).object();};
		auto caps=call("get_audio_capabilities",{});QCOMPARE(caps.value("code").toString(),QStringLiteral("ok"));QVERIFY(caps.value("data").toObject().value("import_roots_configured").toBool());QVERIFY(!QJsonDocument(caps).toJson().contains(root.path().toUtf8()));
		const auto tracks=Engine::getSong()->tracks().size();QCOMPARE(call("create_audio_track",{{"name","Voice"},{"dry_run",true}}).value("code").toString(),QStringLiteral("ok"));QCOMPARE(Engine::getSong()->tracks().size(),tracks);
		auto created=call("create_audio_track",{{"name","Voice"}});const auto trackId=created.value("data").toObject().value("track_id").toString();QVERIFY(!trackId.isEmpty());AiBridgeProjectContext ids;auto* track=dynamic_cast<SampleTrack*>(ids.resolveTrack(trackId));QVERIFY(track);auto remove=qScopeGuard([&]{delete track;});
		QJsonObject imp{{"track_id",trackId},{"path",path},{"position_ticks",3840},{"dry_run",true}};auto preview=call("import_audio_file",imp);QCOMPARE(preview.value("code").toString(),QStringLiteral("ok"));QCOMPARE(track->numOfClips(),0);imp.insert("dry_run",false);auto imported=call("import_audio_file",imp);QCOMPARE(imported.value("code").toString(),QStringLiteral("ok"));QCOMPARE(imported.value("data").toObject().value("preview"),preview.value("data").toObject().value("preview"));
		const auto id=imported.value("data").toObject().value("clip_id").toString();auto* clip=dynamic_cast<SampleClip*>(ids.resolveClip(id));QVERIFY(clip);QVERIFY(!clip->sourceFileIdentity().isEmpty());
		auto read=call("get_audio_clip",{{"clip_id",id}}).value("data").toObject();QCOMPARE(read.value("position_ticks").toInt(),3840);QCOMPARE(read.value("sample_rate").toInt(),44100);QCOMPARE(read.value("source_status").toString(),QStringLiteral("available"));
		const QJsonObject vocalSelection{{"clip_id",id},{"expected_audio_state_hash",read["audio_state_hash"]},{"source_frames",QJsonObject{{"in_frame","0"},{"out_frame","44100"}}}};
		const QJsonObject vocalRequest{{"kind","levels"},{"selections",QJsonArray{vocalSelection}}};
		if(nativeHost)
		{
			// Explicit test-harness mode only. Expose no additional production route.
			socket.disconnectFromServer();QTRY_VERIFY(socket.state()==QLocalSocket::UnconnectedState);QTest::qWait(5);
			QFile ready(nativeReady);QVERIFY(ready.open(QIODevice::WriteOnly|QIODevice::NewOnly));
			QVERIFY(ready.write(QJsonDocument(QJsonObject{{"pid",double(QCoreApplication::applicationPid())},{"request",vocalRequest}}).toJson(QJsonDocument::Compact))>0);ready.close();
			QElapsedTimer timer;timer.start();while(!QFile::exists(nativeReady+".done") && timer.elapsed()<45000)QTest::qWait(5);
			QVERIFY2(QFile::exists(nativeReady+".done"),"Explicit native MCP harness timed out");
			vocalJobs.shutdown();QFile unchanged(path);QVERIFY(unchanged.open(QIODevice::ReadOnly));QCOMPARE(unchanged.readAll(),wav);
			QCOMPARE(clip->startPosition().getTicks(),3840);QCOMPARE(track->numOfClips(),1);return;
		}
		QCOMPARE(call("get_vocal_capabilities",{}).value("code").toString(),QString("ok"));
		const auto vocalStart=call("analyze_vocal_audio",vocalRequest);QCOMPARE(vocalStart["code"].toString(),QString("ok"));
		const auto jobId=vocalStart["data"].toObject()["job_id"].toString();QVERIFY(!jobId.isEmpty());
		QJsonObject vocalStatus;
		QTRY_VERIFY((vocalStatus=call("get_vocal_job",{{"job_id",jobId},{"mode","status"}}))["data"].toObject()["state"].toString()=="succeeded");
		const auto analysisId=vocalStatus["data"].toObject()["analysis_id"].toString();
		const auto vocalPage=call("get_vocal_analysis",{{"analysis_id",analysisId},{"limit",2}});QCOMPARE(vocalPage["code"].toString(),QString("ok"));
		QCOMPARE(vocalPage["data"].toObject()["records"].toArray().size(),2);QVERIFY(!QJsonDocument(vocalPage).toJson().contains(root.path().toUtf8()));
		QVERIFY(!call("cancel_vocal_job",{{"job_id",jobId}})["data"].toObject()["cancel_applied"].toBool());
		QCOMPARE(call("get_vocal_job",{{"job_id",jobId},{"mode","release"}})["code"].toString(),QString("ok"));
		QCOMPARE(call("get_vocal_analysis",{{"analysis_id",analysisId}})["code"].toString(),QString("stale_id"));
		for(int disposal=0;disposal<3;++disposal)
		{
			auto transient=std::make_unique<AiBridgeIpcServer>(nullptr,QString("lmms-vocal-dispose-%1").arg(disposal),&vocalJobs);QVERIFY(transient->start());
			QLocalSocket transientClient;transientClient.connectToServer(transient->endpointName());QVERIFY(transientClient.waitForConnected(1000));
			bool notified=false;
			const auto observer=connect(&vocalJobs,&AiBridgeVocalJobs::jobsChanged,&vocalJobs,[&]{
				if(notified)return;
				notified=true;
				if(disposal==1)transient.reset();else if(disposal==0)transient->stop();else delete transient->findChild<QLocalSocket*>();
			});
			transientClient.write(QJsonDocument(QJsonObject{{"version",2},{"request_id","dispose"},{"action","analyze_vocal_audio"},{"parameters",vocalRequest}}).toJson(QJsonDocument::Compact)+'\n');transientClient.flush();
			QTRY_VERIFY(notified);disconnect(observer);
			QTRY_VERIFY(transientClient.state()==QLocalSocket::UnconnectedState);QVERIFY(transientClient.readAll().isEmpty());
			if(disposal==2)
			{
				QLocalSocket replacement;replacement.connectToServer(transient->endpointName());QVERIFY(replacement.waitForConnected(1000));
				replacement.write("{\"version\":2,\"request_id\":\"replacement\",\"action\":\"get_vocal_capabilities\",\"parameters\":{}}\n");replacement.flush();
				QTRY_VERIFY(replacement.bytesAvailable()>0);
				QCOMPARE(QJsonDocument::fromJson(replacement.readAll()).object()["code"].toString(),QString("ok"));
			}
			const auto retained=vocalJobs.snapshots();QCOMPARE(retained.size(),size_t(1));const auto retainedId=retained[0].jobId;
			QTRY_VERIFY(vocalJobs.job(retainedId).value->state==VocalJobState::Succeeded);
			QCOMPARE(vocalJobs.release(retainedId).code,QString("ok"));
		}
		QCOMPARE(clip->startPosition().getTicks(),3840);QCOMPARE(track->numOfClips(),1);
		QJsonObject move{{"clip_id",id},{"expected_audio_state_hash",read.value("audio_state_hash")},{"position_ticks",5000},{"dry_run",true}};auto movedry=call("move_audio_clip",move);QCOMPARE(movedry.value("code").toString(),QStringLiteral("ok"));QCOMPARE(clip->startPosition().getTicks(),3840);move.insert("dry_run",false);QCOMPARE(call("move_audio_clip",move).value("code").toString(),QStringLiteral("ok"));QCOMPARE(clip->startPosition().getTicks(),5000);QCOMPARE(call("move_audio_clip",move).value("code").toString(),QStringLiteral("clip_state_mismatch"));
		read=call("get_audio_clip",{{"clip_id",id}}).value("data").toObject();QJsonObject copy{{"clip_id",id},{"expected_audio_state_hash",read.value("audio_state_hash")},{"destination_track_id",trackId},{"position_ticks",5000}};QCOMPARE(call("duplicate_audio_clip",copy).value("code").toString(),QStringLiteral("destination_conflict"));copy.insert("position_ticks",6000);auto copied=call("duplicate_audio_clip",copy);QCOMPARE(copied.value("code").toString(),QStringLiteral("ok"));const auto copiedId=copied.value("data").toObject().value("created_clip_ids").toArray()[0].toString();QVERIFY(copiedId!=id);
		QJsonObject op{{"operation","move"},{"clip_id",id},{"expected_audio_state_hash",read.value("audio_state_hash")},{"position_ticks",7000}};auto aligned=call("batch_edit_audio_clips",{{"operations",QJsonArray{op}},{"dry_run",true}});QCOMPARE(aligned.value("code").toString(),QStringLiteral("ok"));QCOMPARE(clip->startPosition().getTicks(),5000);auto done=call("batch_edit_audio_clips",{{"operations",QJsonArray{op}}});QCOMPARE(done.value("code").toString(),QStringLiteral("ok"));QCOMPARE(done.value("data").toObject().value("preview"),aligned.value("data").toObject().value("preview"));QCOMPARE(clip->startPosition().getTicks(),7000);
		const auto copyRead=call("get_audio_clip",{{"clip_id",copiedId}}).value("data").toObject();auto deleted=call("delete_audio_clip",{{"clip_id",copiedId},{"expected_audio_state_hash",copyRead.value("audio_state_hash")}});QCOMPARE(deleted.value("code").toString(),QStringLiteral("ok"));QVERIFY(!ids.resolveClip(copiedId));QCOMPARE(call("get_audio_clip",{{"clip_id",copiedId}}).value("code").toString(),QStringLiteral("stale_id"));
		imp.insert("path","G:/unauthorized-private.wav");auto denied=call("import_audio_file",imp);QCOMPARE(denied.value("code").toString(),QStringLiteral("import_path_denied"));QVERIFY(!QJsonDocument(denied).toJson().contains("unauthorized-private"));QCOMPARE(track->numOfClips(),1);QFile f(path);QVERIFY(f.open(QIODevice::ReadOnly));QCOMPARE(f.readAll(),wav);
	}
	void initTestCase()
	{
		qputenv("LMMS_PLUGIN_DIR", QCoreApplication::applicationDirPath().toUtf8() + QByteArrayLiteral("/../plugins"));
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

	void projectStructureIncludesLoadedProjectIdentity()
	{
		using namespace lmms;
		AiBridgeIpcServer server(nullptr, QStringLiteral("lmms-identity-wire-test"));
		QVERIFY(server.start());
		QLocalSocket socket;
		socket.connectToServer(server.endpointName());
		QVERIFY(socket.waitForConnected(1000));
		socket.write(QByteArrayLiteral("{\"version\":2,\"request_id\":\"identity\",\"action\":\"get_project_structure\",\"parameters\":{}}\n"));
		socket.flush();
		QTRY_VERIFY_WITH_TIMEOUT(socket.bytesAvailable() > 0, 1000);
		const auto response = QJsonDocument::fromJson(socket.readAll()).object();
		QCOMPARE(response.value(QStringLiteral("code")).toString(), QStringLiteral("ok"));
		const auto identity = response.value(QStringLiteral("data")).toObject().value(QStringLiteral("identity")).toObject();
		QVERIFY(!identity.isEmpty());
		QCOMPARE(identity.value(QStringLiteral("schema_version")).toInt(), 1);
		QVERIFY(!identity.value(QStringLiteral("process_session_id")).toString().isEmpty());
		QVERIFY(!identity.value(QStringLiteral("project_load_generation")).toString().isEmpty());
		QVERIFY(!identity.value(QStringLiteral("identity_revision")).toString().isEmpty());
		QVERIFY(!identity.value(QStringLiteral("document_state")).toString().isEmpty());
		QVERIFY(identity.contains(QStringLiteral("loaded_project")));
		const auto initialDocumentHash = identity.value(QStringLiteral("document_state_hash")).toString();
		QVERIFY(QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(initialDocumentHash).hasMatch());
		QVERIFY(identity.value(QStringLiteral("server_pid")).toInt() > 0);
#if defined(Q_OS_WIN) || defined(Q_OS_LINUX)
		QCOMPARE(identity.value(QStringLiteral("availability")).toString(), QStringLiteral("available"));
		QVERIFY(identity.value(QStringLiteral("client_pid")).toInt() > 0);
		QCOMPARE(identity.value(QStringLiteral("client_pid")).toInt(), static_cast<int>(QCoreApplication::applicationPid()));
		bool serverCreationParsed = false;
		const auto serverCreationTime = identity.value(QStringLiteral("server_creation_time")).toString().toULongLong(&serverCreationParsed);
		QVERIFY(serverCreationParsed);
		QVERIFY(serverCreationTime > 0);
		bool clientCreationParsed = false;
		const auto clientCreationTime = identity.value(QStringLiteral("client_creation_time")).toString().toULongLong(&clientCreationParsed);
		QVERIFY(clientCreationParsed);
		QVERIFY(clientCreationTime > 0);
#else
		QCOMPARE(identity.value(QStringLiteral("availability")).toString(), QStringLiteral("unavailable"));
		QVERIFY(!identity.contains(QStringLiteral("client_pid")));
#endif
		QCOMPARE(identity.value(QStringLiteral("document_state")).toString(), QStringLiteral("unsaved"));

		QTemporaryDir projectRoot;
		QVERIFY(projectRoot.isValid());
		const auto projectPath = projectRoot.filePath(QStringLiteral("identity.mmp"));
		QVERIFY(Engine::getSong()->saveProjectFile(projectPath, false));
		QFile savedFile(projectPath);
		QVERIFY(savedFile.open(QIODevice::ReadOnly));
		const auto expectedHash = QCryptographicHash::hash(savedFile.readAll(), QCryptographicHash::Sha256).toHex();
		Engine::getSong()->loadProject(projectPath);
		socket.write(QByteArrayLiteral("{\"version\":2,\"request_id\":\"identity-loaded\",\"action\":\"get_project_structure\",\"parameters\":{}}\n"));
		socket.flush();
		QTRY_VERIFY_WITH_TIMEOUT(socket.bytesAvailable() > 0, 1000);
		const auto loaded = QJsonDocument::fromJson(socket.readAll()).object().value(QStringLiteral("data")).toObject().value(QStringLiteral("identity")).toObject();
		QCOMPARE(loaded.value(QStringLiteral("document_state")).toString(), QStringLiteral("saved"));
		QCOMPARE(loaded.value(QStringLiteral("loaded_project")).toObject().value(QStringLiteral("canonical_path")).toString(), QFileInfo(projectPath).canonicalFilePath());
		QCOMPARE(loaded.value(QStringLiteral("loaded_project")).toObject().value(QStringLiteral("sha256")).toString().toLatin1(), expectedHash);
#ifdef Q_OS_WIN
		QVERIFY(!loaded.value(QStringLiteral("loaded_project")).toObject().value(QStringLiteral("windows_file_id")).toString().isEmpty());
#endif
#ifdef Q_OS_UNIX
		QVERIFY(!loaded.value(QStringLiteral("loaded_project")).toObject().value(QStringLiteral("file_id")).toString().isEmpty());
#endif
		QVERIFY(loaded.value(QStringLiteral("project_load_generation")).toString().toULongLong() > identity.value(QStringLiteral("project_load_generation")).toString().toULongLong());
		const auto loadedDocumentHash = loaded.value(QStringLiteral("document_state_hash")).toString();
		QVERIFY(QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(loadedDocumentHash).hasMatch());

		Engine::getSong()->setModified();
		socket.write(QByteArrayLiteral("{\"version\":2,\"request_id\":\"identity-modified\",\"action\":\"get_project_structure\",\"parameters\":{}}\n"));
		socket.flush();
		QTRY_VERIFY_WITH_TIMEOUT(socket.bytesAvailable() > 0, 1000);
		const auto modified = QJsonDocument::fromJson(socket.readAll()).object().value(QStringLiteral("data")).toObject().value(QStringLiteral("identity")).toObject();
		QCOMPARE(modified.value(QStringLiteral("document_state")).toString(), QStringLiteral("modified"));
		QVERIFY(modified.value(QStringLiteral("dirty")).toBool());
		QVERIFY(modified.value(QStringLiteral("identity_revision")).toString().toULongLong() > loaded.value(QStringLiteral("identity_revision")).toString().toULongLong());
		auto* changedTrack = Track::create(Track::Type::Instrument, Engine::getSong());
		QVERIFY(changedTrack);
		changedTrack->setName(QStringLiteral("Changed while already dirty"));
		socket.write(QByteArrayLiteral("{\"version\":2,\"request_id\":\"identity-dirty-edit\",\"action\":\"get_project_structure\",\"parameters\":{}}\n"));
		socket.flush();
		QTRY_VERIFY_WITH_TIMEOUT(socket.bytesAvailable() > 0, 1000);
		const auto dirtyEdit = QJsonDocument::fromJson(socket.readAll()).object().value(QStringLiteral("data")).toObject();
		const auto dirtyEditIdentity = dirtyEdit.value(QStringLiteral("identity")).toObject();
		QCOMPARE(dirtyEditIdentity.value(QStringLiteral("identity_revision")).toString(), modified.value(QStringLiteral("identity_revision")).toString());
		QVERIFY(dirtyEditIdentity.value(QStringLiteral("document_state_hash")).toString() != modified.value(QStringLiteral("document_state_hash")).toString());
		bool foundTrackHash = false;
		for (const auto &value : dirtyEdit.value(QStringLiteral("tracks")).toArray())
		{
			const auto track = value.toObject();
			if (track.value(QStringLiteral("name")).toString() == QStringLiteral("Changed while already dirty"))
			{
				foundTrackHash = QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(track.value(QStringLiteral("state_hash")).toString()).hasMatch();
			}
		}
		QVERIFY(foundTrackHash);
		QFile unchangedProjectFile(projectPath);
		QVERIFY(unchangedProjectFile.open(QIODevice::ReadOnly));
		QCOMPARE(QCryptographicHash::hash(unchangedProjectFile.readAll(), QCryptographicHash::Sha256).toHex(), expectedHash);

		const auto saveAsPath = projectRoot.filePath(QStringLiteral("identity-save-as.mmp"));
		QCoreApplication::processEvents();
		QVERIFY(Engine::getSong()->isModified());
		QVERIFY(Engine::getSong()->guiSaveProjectAs(saveAsPath));
		socket.write(QByteArrayLiteral("{\"version\":2,\"request_id\":\"identity-save-as\",\"action\":\"get_project_structure\",\"parameters\":{}}\n"));
		socket.flush();
		QTRY_VERIFY_WITH_TIMEOUT(socket.bytesAvailable() > 0, 1000);
		const auto savedAs = QJsonDocument::fromJson(socket.readAll()).object().value(QStringLiteral("data")).toObject().value(QStringLiteral("identity")).toObject();
		QCOMPARE(savedAs.value(QStringLiteral("document_state")).toString(), QStringLiteral("saved"));
		QVERIFY(!Engine::getSong()->isModified());
		QCOMPARE(savedAs.value(QStringLiteral("loaded_project")).toObject().value(QStringLiteral("canonical_path")).toString(), QFileInfo(saveAsPath).canonicalFilePath());
		QVERIFY(savedAs.value(QStringLiteral("identity_revision")).toString().toULongLong() > modified.value(QStringLiteral("identity_revision")).toString().toULongLong());
		socket.write(QByteArrayLiteral("{\"version\":2,\"request_id\":\"identity-save-as-repeat\",\"action\":\"get_project_structure\",\"parameters\":{}}\n"));
		socket.flush();
		QTRY_VERIFY_WITH_TIMEOUT(socket.bytesAvailable() > 0, 1000);
		const auto savedAsRepeat = QJsonDocument::fromJson(socket.readAll()).object().value(QStringLiteral("data")).toObject().value(QStringLiteral("identity")).toObject();
		QCOMPARE(savedAsRepeat.value(QStringLiteral("document_state")).toString(), QStringLiteral("saved"));
		QCOMPARE(savedAsRepeat.value(QStringLiteral("identity_revision")).toString(), savedAs.value(QStringLiteral("identity_revision")).toString());
		QCOMPARE(savedAsRepeat.value(QStringLiteral("document_state_hash")).toString(), savedAs.value(QStringLiteral("document_state_hash")).toString());
		QVERIFY(!Engine::getSong()->isModified());

		const auto secondPath = projectRoot.filePath(QStringLiteral("identity-second.mmp"));
		QVERIFY(Engine::getSong()->saveProjectFile(secondPath, false));
		Engine::getSong()->loadProject(secondPath);
		socket.write(QByteArrayLiteral("{\"version\":2,\"request_id\":\"identity-switched\",\"action\":\"get_project_structure\",\"parameters\":{}}\n"));
		socket.flush();
		QTRY_VERIFY_WITH_TIMEOUT(socket.bytesAvailable() > 0, 1000);
		const auto switched = QJsonDocument::fromJson(socket.readAll()).object().value(QStringLiteral("data")).toObject().value(QStringLiteral("identity")).toObject();
		QCOMPARE(switched.value(QStringLiteral("loaded_project")).toObject().value(QStringLiteral("canonical_path")).toString(), QFileInfo(secondPath).canonicalFilePath());
		QVERIFY(switched.value(QStringLiteral("project_load_generation")).toString().toULongLong() > savedAs.value(QStringLiteral("project_load_generation")).toString().toULongLong());
		Engine::getSong()->loadProject(secondPath);
		socket.write(QByteArrayLiteral("{\"version\":2,\"request_id\":\"identity-reloaded\",\"action\":\"get_project_structure\",\"parameters\":{}}\n"));
		socket.flush();
		QTRY_VERIFY_WITH_TIMEOUT(socket.bytesAvailable() > 0, 1000);
		const auto reloaded = QJsonDocument::fromJson(socket.readAll()).object().value(QStringLiteral("data")).toObject().value(QStringLiteral("identity")).toObject();
		QVERIFY(reloaded.value(QStringLiteral("project_load_generation")).toString().toULongLong() > switched.value(QStringLiteral("project_load_generation")).toString().toULongLong());
		QCOMPARE(reloaded.value(QStringLiteral("process_session_id")).toString(), identity.value(QStringLiteral("process_session_id")).toString());
	}

	void enforcesGuardedProjectIdentityAndTargetPreconditions()
	{
		using namespace lmms;
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX)
		QSKIP("Guarded mutations require verified Windows or Linux IPC peer identity");
#endif
		Engine::getSong()->createNewProject();
		QTemporaryDir root;
		QVERIFY(root.isValid());
		const auto projectPath = root.filePath(QStringLiteral("guarded.mmp"));
		QVERIFY(Engine::getSong()->saveProjectFile(projectPath, false));
		Engine::getSong()->loadProject(projectPath);

		AiBridgeIpcServer server(nullptr, QStringLiteral("lmms-guarded-mutation-test"));
		QVERIFY(server.start());
		QLocalSocket socket;
		socket.connectToServer(server.endpointName());
		QVERIFY(socket.waitForConnected(1000));
		auto call = [&socket](const QString &action, const QJsonObject &parameters) {
			socket.write(QJsonDocument(QJsonObject{{"version", 2}, {"request_id", "guarded-test"},
				{"action", action}, {"parameters", parameters}}).toJson(QJsonDocument::Compact) + '\n');
			socket.flush();
			QElapsedTimer timer;
			timer.start();
			QByteArray bytes;
			while (!bytes.endsWith('\n') && timer.elapsed() < 3000)
			{
				QTest::qWait(1);
				bytes += socket.readAll();
			}
			return QJsonDocument::fromJson(bytes).object();
		};
		auto readStructure = [&]() {
			return call(QStringLiteral("get_project_structure"), {}).value(QStringLiteral("data")).toObject();
		};
		auto structure = readStructure();
		auto identity = structure.value(QStringLiteral("identity")).toObject();
		QCOMPARE(identity.value(QStringLiteral("availability")).toString(), QStringLiteral("available"));
		QFile source(projectPath);
		QVERIFY(source.open(QIODevice::ReadOnly));
		const auto originalBytes = source.readAll();
		source.close();
		const auto sourceDocumentHashBefore = identity.value(QStringLiteral("document_state_hash")).toString();
		QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Append));
		QCOMPARE(source.write(QByteArrayLiteral("external-change")), qint64(15));
		source.close();
		const auto trackCountBefore = Engine::getSong()->tracks().size();
		const QJsonObject createTrack{{"expected_project_identity", identity}, {"operation", "create_instrument_track"},
			{"parameters", QJsonObject{{"name", "Guarded source check"}}}};
		const auto sourceRejected = call(QStringLiteral("guarded_mutate_project"), createTrack);
		QCOMPARE(sourceRejected.value(QStringLiteral("code")).toString(), QStringLiteral("loaded_source_changed"));
		QCOMPARE(Engine::getSong()->tracks().size(), trackCountBefore);
		QCOMPARE(readStructure().value(QStringLiteral("identity")).toObject().value(QStringLiteral("document_state_hash")).toString(), sourceDocumentHashBefore);
		QVERIFY(!QJsonDocument(sourceRejected).toJson().contains(root.path().toUtf8()));
		QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate));
		QCOMPARE(source.write(originalBytes), qint64(originalBytes.size()));
		source.close();
		Engine::getSong()->loadProject(projectPath);

		structure = readStructure();
		identity = structure.value(QStringLiteral("identity")).toObject();
		const auto tracksBeforeExport = Engine::getSong()->tracks().size();
		const auto exportDocumentHashBefore = identity.value(QStringLiteral("document_state_hash")).toString();
		Engine::getSong()->startExport();
		const auto exportRejected = call(QStringLiteral("guarded_mutate_project"),
			{{"expected_project_identity", identity}, {"operation", "create_instrument_track"},
			 {"parameters", QJsonObject{{"name", "Guarded during export"}}}});
		QCOMPARE(exportRejected.value(QStringLiteral("code")).toString(), QStringLiteral("project_busy"));
		QCOMPARE(Engine::getSong()->tracks().size(), tracksBeforeExport);
		Engine::getSong()->stopExport();
		QVERIFY(Engine::getSong()->isStopped());
		QVERIFY(!Engine::getSong()->isExporting());
		QCOMPARE(readStructure().value(QStringLiteral("identity")).toObject().value(QStringLiteral("document_state_hash")).toString(), exportDocumentHashBefore);

		structure = readStructure();
		identity = structure.value(QStringLiteral("identity")).toObject();
		const auto freshCreateTrack = QJsonObject{{"expected_project_identity", identity},
			{"operation", "create_instrument_track"}, {"parameters", QJsonObject{{"name", "Guarded track"}}}};
		const auto created = call(QStringLiteral("guarded_mutate_project"), freshCreateTrack);
		QCOMPARE(created.value(QStringLiteral("code")).toString(), QStringLiteral("ok"));
		const auto createdData = created.value(QStringLiteral("data")).toObject();
		QCOMPARE(createdData.value(QStringLiteral("outcome")).toString(), QStringLiteral("completed"));
		QVERIFY(QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(createdData.value(QStringLiteral("post_target")).toObject().value(QStringLiteral("state_hash")).toString()).hasMatch());
		QCOMPARE(Engine::getSong()->tracks().size(), trackCountBefore + 1);

		const auto staleIdentityHash = readStructure().value(QStringLiteral("identity")).toObject().value(QStringLiteral("document_state_hash")).toString();
		const auto staleIdentity = call(QStringLiteral("guarded_mutate_project"), createTrack);
		QCOMPARE(staleIdentity.value(QStringLiteral("code")).toString(), QStringLiteral("stale_project_identity"));
		QCOMPARE(Engine::getSong()->tracks().size(), trackCountBefore + 1);
		QCOMPARE(readStructure().value(QStringLiteral("identity")).toObject().value(QStringLiteral("document_state_hash")).toString(), staleIdentityHash);

		structure = readStructure();
		identity = structure.value(QStringLiteral("identity")).toObject();
		QString trackId;
		QString trackHash;
		for (const auto &value : structure.value(QStringLiteral("tracks")).toArray())
		{
			const auto track = value.toObject();
			if (track.value(QStringLiteral("name")).toString() == QStringLiteral("Guarded track"))
			{
				trackId = track.value(QStringLiteral("id")).toString();
				trackHash = track.value(QStringLiteral("state_hash")).toString();
			}
		}
		QVERIFY(!trackId.isEmpty());
		QVERIFY(!trackHash.isEmpty());
		const auto clipsBefore = Engine::getSong()->tracks().back()->numOfClips();
		const auto trackDocumentHashBefore = identity.value(QStringLiteral("document_state_hash")).toString();
		const auto liveTrackHashBefore = AiBridgeProjectContext{}.trackStateHash(Engine::getSong()->tracks().back());
		const auto staleInstrument = call(QStringLiteral("guarded_mutate_project"), {{"expected_project_identity", identity},
			{"operation", "set_track_instrument"}, {"parameters", QJsonObject{{"track_id", trackId}, {"instrument_id", "tripleoscillator"}}},
			{"expected_target", QJsonObject{{"kind", "track"}, {"object_id", trackId}, {"expected_state_hash", QString(64, '0')}}}});
		QCOMPARE(staleInstrument.value(QStringLiteral("code")).toString(), QStringLiteral("stale_target"));
		QCOMPARE(Engine::getSong()->tracks().back()->numOfClips(), clipsBefore);
		QCOMPARE(AiBridgeProjectContext{}.trackStateHash(Engine::getSong()->tracks().back()), liveTrackHashBefore);
		QCOMPARE(readStructure().value(QStringLiteral("identity")).toObject().value(QStringLiteral("document_state_hash")).toString(), trackDocumentHashBefore);
		const auto staleTarget = call(QStringLiteral("guarded_mutate_project"), {{"expected_project_identity", identity},
			{"operation", "create_midi_clip"}, {"parameters", QJsonObject{{"track_id", trackId}, {"start_ticks", 0}}},
			{"expected_target", QJsonObject{{"kind", "track"}, {"object_id", trackId}, {"expected_state_hash", QString(64, '0')}}}});
		QCOMPARE(staleTarget.value(QStringLiteral("code")).toString(), QStringLiteral("stale_target"));
		QCOMPARE(Engine::getSong()->tracks().back()->numOfClips(), clipsBefore);
		QCOMPARE(AiBridgeProjectContext{}.trackStateHash(Engine::getSong()->tracks().back()), liveTrackHashBefore);
		QCOMPARE(readStructure().value(QStringLiteral("identity")).toObject().value(QStringLiteral("document_state_hash")).toString(), trackDocumentHashBefore);
		const auto clipCreated = call(QStringLiteral("guarded_mutate_project"), {{"expected_project_identity", identity},
			{"operation", "create_midi_clip"}, {"parameters", QJsonObject{{"track_id", trackId}, {"start_ticks", 0}, {"length_ticks", 384}}},
			{"expected_target", QJsonObject{{"kind", "track"}, {"object_id", trackId}, {"expected_state_hash", trackHash}}}});
		QCOMPARE(clipCreated.value(QStringLiteral("code")).toString(), QStringLiteral("ok"));
		const auto clipId = clipCreated.value(QStringLiteral("data")).toObject().value(QStringLiteral("operation_result")).toObject().value(QStringLiteral("clip_id")).toString();
		QVERIFY(!clipId.isEmpty());
		const auto clipReadBeforeNote = call(QStringLiteral("get_clip"), {{"clip_id", clipId}}).value(QStringLiteral("data")).toObject();
		const auto identityBeforeNote = readStructure().value(QStringLiteral("identity")).toObject();
		const QJsonObject note{{"position_ticks", 0}, {"length_ticks", 96}, {"key", 60},
			{"volume", 100}, {"panning", 0}, {"note_type", "regular"}};
		const auto noteAdded = call(QStringLiteral("guarded_mutate_project"), {{"expected_project_identity", identityBeforeNote},
			{"operation", "add_notes"}, {"parameters", QJsonObject{{"clip_id", clipId}, {"notes", QJsonArray{note}},
				{"expected_content_hash", clipReadBeforeNote.value(QStringLiteral("content_hash"))}}},
			{"expected_target", QJsonObject{{"kind", "clip"}, {"object_id", clipId},
				{"expected_state_hash", clipReadBeforeNote.value(QStringLiteral("state_hash"))}}}});
		QCOMPARE(noteAdded.value(QStringLiteral("code")).toString(), QStringLiteral("ok"));
		QCOMPARE(noteAdded.value(QStringLiteral("data")).toObject().value(QStringLiteral("operation_result")).toObject().value(QStringLiteral("notes_added")).toInt(), 1);
		QCOMPARE(Engine::getSong()->tracks().back()->getClip(0)->getTrack()->numOfClips(), 1);
		const auto freshIdentity = readStructure().value(QStringLiteral("identity")).toObject();
		const auto clipRead = call(QStringLiteral("get_clip"), {{"clip_id", clipId}}).value(QStringLiteral("data")).toObject();
		const auto staleContentHash = QString(64, 'f');
		const QJsonObject clipTarget{{"kind", "clip"}, {"object_id", clipId},
			{"expected_state_hash", clipRead.value(QStringLiteral("state_hash"))}};
		const auto clipHashBeforeFailures = clipRead.value(QStringLiteral("state_hash")).toString();
		const auto contentHashBeforeFailures = clipRead.value(QStringLiteral("content_hash")).toString();
		const auto documentHashBeforeFailures = freshIdentity.value(QStringLiteral("document_state_hash")).toString();
		const auto staleClipState = call(QStringLiteral("guarded_mutate_project"), {{"expected_project_identity", freshIdentity},
			{"operation", "delete_notes"}, {"parameters", QJsonObject{{"clip_id", clipId},
				{"selection", QJsonObject{{"position_start", 0}, {"position_end", 96}}},
				{"expected_content_hash", contentHashBeforeFailures}}},
			{"expected_target", QJsonObject{{"kind", "clip"}, {"object_id", clipId},
				{"expected_state_hash", QString(64, '0')}}}});
		QCOMPARE(staleClipState.value(QStringLiteral("code")).toString(), QStringLiteral("stale_target"));
		QCOMPARE(readStructure().value(QStringLiteral("identity")).toObject().value(QStringLiteral("document_state_hash")).toString(), documentHashBeforeFailures);
		QCOMPARE(call(QStringLiteral("get_clip"), {{"clip_id", clipId}}).value(QStringLiteral("data")).toObject().value(QStringLiteral("state_hash")).toString(), clipHashBeforeFailures);
		const QVector<QPair<QString, QJsonObject>> staleClipMutations{
			{QStringLiteral("add_notes"), QJsonObject{{"clip_id", clipId}, {"notes", QJsonArray{note}}, {"expected_content_hash", staleContentHash}}},
			{QStringLiteral("delete_notes"), QJsonObject{{"clip_id", clipId}, {"selection", QJsonObject{{"position_start", 0}, {"position_end", 96}}}, {"expected_content_hash", staleContentHash}}},
			{QStringLiteral("transpose_notes"), QJsonObject{{"clip_id", clipId}, {"selection", QJsonObject{{"key_min", 0}, {"key_max", 127}}}, {"semitones", 1}, {"expected_content_hash", staleContentHash}}},
			{QStringLiteral("move_notes"), QJsonObject{{"clip_id", clipId}, {"selection", QJsonObject{{"position_start", 0}, {"position_end", 96}}}, {"delta_ticks", 12}, {"expected_content_hash", staleContentHash}}},
			{QStringLiteral("duplicate_region"), QJsonObject{{"clip_id", clipId}, {"source_start", 0}, {"source_end", 96}, {"destination_start", 96}, {"expected_content_hash", staleContentHash}}},
			{QStringLiteral("set_clip_properties"), QJsonObject{{"clip_id", clipId}, {"auto_resize", true}, {"expected_start_ticks", 0}, {"expected_content_hash", staleContentHash}}}};
		for (const auto &mutation : staleClipMutations)
		{
			const auto rejected = call(QStringLiteral("guarded_mutate_project"), {{"expected_project_identity", freshIdentity},
				{"operation", mutation.first}, {"parameters", mutation.second}, {"expected_target", clipTarget}});
			QCOMPARE(rejected.value(QStringLiteral("code")).toString(), QStringLiteral("clip_changed"));
			QVERIFY2(!rejected.value(QStringLiteral("success")).toBool(), qPrintable(mutation.first));
			QCOMPARE(Engine::getSong()->tracks().back()->getClip(0)->length().getTicks(), qint64(384));
			QCOMPARE(dynamic_cast<MidiClip *>(Engine::getSong()->tracks().back()->getClip(0))->notes().size(), 1);
			const auto unchangedClip = call(QStringLiteral("get_clip"), {{"clip_id", clipId}}).value(QStringLiteral("data")).toObject();
			QCOMPARE(unchangedClip.value(QStringLiteral("state_hash")).toString(), clipHashBeforeFailures);
			QCOMPARE(unchangedClip.value(QStringLiteral("content_hash")).toString(), contentHashBeforeFailures);
			QCOMPARE(readStructure().value(QStringLiteral("identity")).toObject().value(QStringLiteral("document_state_hash")).toString(), documentHashBeforeFailures);
		}
		const auto postNoteStructure = readStructure();
		const auto postNoteIdentity = postNoteStructure.value(QStringLiteral("identity")).toObject();
		const auto currentTempo = Engine::getSong()->getTempo();
		const auto wrongExpectedTempo = currentTempo == 999 ? 998 : currentTempo + 1;
		const auto staleTempo = call(QStringLiteral("guarded_mutate_project"), {{"expected_project_identity", postNoteIdentity},
			{"operation", "set_tempo"}, {"parameters", QJsonObject{{"bpm", 100}}},
			{"expected_target", QJsonObject{{"kind", "tempo"}, {"expected_bpm", wrongExpectedTempo}}}});
		QCOMPARE(staleTempo.value(QStringLiteral("code")).toString(), QStringLiteral("stale_target"));
		QCOMPARE(Engine::getSong()->getTempo(), currentTempo);
		QCOMPARE(readStructure().value(QStringLiteral("identity")).toObject().value(QStringLiteral("document_state_hash")).toString(), postNoteIdentity.value(QStringLiteral("document_state_hash")).toString());
	}

	void executesEveryAllowlistedGuardedOperationAndReportsFreshPostState()
	{
		using namespace lmms;
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX)
		QSKIP("Guarded mutations require verified Windows or Linux IPC peer identity");
#endif
		Engine::getSong()->createNewProject();
		QTemporaryDir root;
		QVERIFY(root.isValid());
		const auto projectPath = root.filePath(QStringLiteral("guarded-success.mmp"));
		QVERIFY(Engine::getSong()->saveProjectFile(projectPath, false));
		Engine::getSong()->loadProject(projectPath);
		QFile source(projectPath);
		QVERIFY(source.open(QIODevice::ReadOnly));
		const auto savedProjectBytes = source.readAll();
		source.close();

		AiBridgeIpcServer server(nullptr, QStringLiteral("lmms-guarded-success-matrix-test"));
		QVERIFY(server.start());
		QLocalSocket socket;
		socket.connectToServer(server.endpointName());
		QVERIFY(socket.waitForConnected(1000));
		int requestNumber = 0;
		auto call = [&socket, &requestNumber](const QString &action, const QJsonObject &parameters) {
			const auto requestId = QStringLiteral("guarded-success-%1").arg(++requestNumber);
			socket.write(QJsonDocument(QJsonObject{{"version", 2}, {"request_id", requestId},
				{"action", action}, {"parameters", parameters}}).toJson(QJsonDocument::Compact) + '\n');
			socket.flush();
			QElapsedTimer timer;
			timer.start();
			QByteArray bytes;
			while (!bytes.endsWith('\n') && timer.elapsed() < 3000)
			{
				QTest::qWait(1);
				bytes += socket.readAll();
			}
			return QJsonDocument::fromJson(bytes).object();
		};
		auto readStructure = [&]() {
			return call(QStringLiteral("get_project_structure"), {}).value(QStringLiteral("data")).toObject();
		};
		auto readClip = [&](const QString &clipId) {
			return call(QStringLiteral("get_clip"), {{"clip_id", clipId}}).value(QStringLiteral("data")).toObject();
		};
		auto readNotes = [&](const QString &clipId) {
			return call(QStringLiteral("get_midi_notes"), {{"clip_id", clipId}, {"offset", 0}, {"limit", 64}}).value(QStringLiteral("data")).toObject();
		};
		auto guarded = [&](const QJsonObject &identity, const QString &operation,
			const QJsonObject &parameters, const QJsonObject &target = QJsonObject()) {
			QJsonObject request{{"expected_project_identity", identity}, {"operation", operation}, {"parameters", parameters}};
			if (!target.isEmpty()) request.insert(QStringLiteral("expected_target"), target);
			return call(QStringLiteral("guarded_mutate_project"), request);
		};
		auto pathFreeIdentity = [](QJsonObject identity) {
			auto loadedProject = identity.value(QStringLiteral("loaded_project")).toObject();
			loadedProject.remove(QStringLiteral("canonical_path"));
			identity.insert(QStringLiteral("loaded_project"), loadedProject);
			return identity;
		};
		auto findTrack = [](const QJsonObject &structure, const QString &trackId) {
			for (const auto &value : structure.value(QStringLiteral("tracks")).toArray())
			{
				const auto track = value.toObject();
				if (track.value(QStringLiteral("id")).toString() == trackId) return track;
			}
			return QJsonObject{};
		};
		auto verifyCompleted = [&](const QJsonObject &response, const QJsonObject &beforeIdentity,
			const QString &targetKind, const QString &targetId, const QJsonObject &freshTarget) -> void {
			QVERIFY(response.value(QStringLiteral("success")).toBool());
			QCOMPARE(response.value(QStringLiteral("code")).toString(), QStringLiteral("ok"));
			const auto data = response.value(QStringLiteral("data")).toObject();
			QCOMPARE(data.value(QStringLiteral("outcome")).toString(), QStringLiteral("completed"));
			const auto liveIdentity = readStructure().value(QStringLiteral("identity")).toObject();
			const auto postIdentity = data.value(QStringLiteral("post_identity")).toObject();
			QCOMPARE(postIdentity, pathFreeIdentity(liveIdentity));
			QVERIFY(postIdentity.value(QStringLiteral("document_state_hash")).toString()
				!= beforeIdentity.value(QStringLiteral("document_state_hash")).toString());
			const auto postTarget = data.value(QStringLiteral("post_target")).toObject();
			QCOMPARE(postTarget.value(QStringLiteral("kind")).toString(), targetKind);
			if (targetKind == QStringLiteral("tempo"))
			{
				QCOMPARE(postTarget.value(QStringLiteral("bpm")).toInt(), Engine::getSong()->getTempo());
			}
			else
			{
				QCOMPARE(postTarget.value(QStringLiteral("object_id")).toString(), targetId);
				QCOMPARE(postTarget.value(QStringLiteral("state_hash")).toString(),
					freshTarget.value(QStringLiteral("state_hash")).toString());
			}
		};

		const auto initialProjectBytes = savedProjectBytes;
		auto structure = readStructure();
		auto identity = structure.value(QStringLiteral("identity")).toObject();
		const int initialTempo = Engine::getSong()->getTempo();
		const int updatedTempo = initialTempo == 999 ? 998 : initialTempo + 1;
		const auto tempoSet = guarded(identity, QStringLiteral("set_tempo"), {{"bpm", updatedTempo}},
			{{"kind", "tempo"}, {"expected_bpm", initialTempo}});
		QCOMPARE(tempoSet.value(QStringLiteral("data")).toObject().value(QStringLiteral("operation_result")).toObject().value(QStringLiteral("bpm")).toInt(), updatedTempo);
		verifyCompleted(tempoSet, identity, QStringLiteral("tempo"), {}, {});
		QCOMPARE(Engine::getSong()->getTempo(), updatedTempo);

		identity = readStructure().value(QStringLiteral("identity")).toObject();
		const auto trackCreated = guarded(identity, QStringLiteral("create_instrument_track"),
			{{"name", "Guarded success matrix"}});
		const auto createdTrackResult = trackCreated.value(QStringLiteral("data")).toObject().value(QStringLiteral("operation_result")).toObject();
		const auto trackId = createdTrackResult.value(QStringLiteral("track_id")).toString();
		QVERIFY(!trackId.isEmpty());
		structure = readStructure();
		auto trackState = findTrack(structure, trackId);
		QCOMPARE(trackState.value(QStringLiteral("name")).toString(), QStringLiteral("Guarded success matrix"));
		verifyCompleted(trackCreated, identity, QStringLiteral("track"), trackId, trackState);

		const auto instrumentResponse = call(QStringLiteral("list_instruments"), {{"offset", 0}, {"limit", 128}});
		QString instrumentId;
		for (const auto &value : instrumentResponse.value(QStringLiteral("data")).toObject().value(QStringLiteral("instruments")).toArray())
		{
			const auto candidate = value.toObject().value(QStringLiteral("instrument_id")).toString();
			if (!candidate.isEmpty() && candidate != QStringLiteral("tripleoscillator"))
			{
				instrumentId = candidate;
				break;
			}
		}
		QVERIFY2(!instrumentId.isEmpty(), "The test engine must expose a second assignable instrument");
		identity = structure.value(QStringLiteral("identity")).toObject();
		const auto instrumentSet = guarded(identity, QStringLiteral("set_track_instrument"),
			{{"track_id", trackId}, {"instrument_id", instrumentId}},
			{{"kind", "track"}, {"object_id", trackId}, {"expected_state_hash", trackState.value(QStringLiteral("state_hash"))}});
		QCOMPARE(instrumentSet.value(QStringLiteral("data")).toObject().value(QStringLiteral("operation_result")).toObject().value(QStringLiteral("track_id")).toString(), trackId);
		auto *instrumentTrack = dynamic_cast<InstrumentTrack *>(AiBridgeProjectContext{}.resolveTrack(trackId));
		QVERIFY(instrumentTrack);
		QVERIFY(instrumentTrack->instrument());
		QVERIFY(instrumentTrack->instrument()->descriptor());
		QCOMPARE(QString::fromLatin1(instrumentTrack->instrument()->descriptor()->name), instrumentId);
		structure = readStructure();
		trackState = findTrack(structure, trackId);
		verifyCompleted(instrumentSet, identity, QStringLiteral("track"), trackId, trackState);

		identity = structure.value(QStringLiteral("identity")).toObject();
		const auto clipCreated = guarded(identity, QStringLiteral("create_midi_clip"),
			{{"track_id", trackId}, {"start_ticks", 0}, {"length_ticks", 384}},
			{{"kind", "track"}, {"object_id", trackId}, {"expected_state_hash", trackState.value(QStringLiteral("state_hash"))}});
		const auto createdClipResult = clipCreated.value(QStringLiteral("data")).toObject().value(QStringLiteral("operation_result")).toObject();
		const auto clipId = createdClipResult.value(QStringLiteral("clip_id")).toString();
		QVERIFY(!clipId.isEmpty());
		QCOMPARE(createdClipResult.value(QStringLiteral("track_id")).toString(), trackId);
		auto clipState = readClip(clipId);
		QCOMPARE(clipState.value(QStringLiteral("start_ticks")).toInt(), 0);
		QCOMPARE(clipState.value(QStringLiteral("length_ticks")).toInt(), 384);
		verifyCompleted(clipCreated, identity, QStringLiteral("clip"), clipId, clipState);

		identity = readStructure().value(QStringLiteral("identity")).toObject();
		const auto clipProperties = guarded(identity, QStringLiteral("set_clip_properties"),
			{{"clip_id", clipId}, {"start_ticks", 24}, {"auto_resize", true},
			 {"expected_start_ticks", 0}, {"expected_content_hash", clipState.value(QStringLiteral("content_hash"))}},
			{{"kind", "clip"}, {"object_id", clipId}, {"expected_state_hash", clipState.value(QStringLiteral("state_hash"))}});
		const auto propertyResult = clipProperties.value(QStringLiteral("data")).toObject().value(QStringLiteral("operation_result")).toObject();
		QCOMPARE(propertyResult.value(QStringLiteral("start_ticks")).toInt(), 24);
		QVERIFY(propertyResult.value(QStringLiteral("auto_resize")).toBool());
		clipState = readClip(clipId);
		QCOMPARE(clipState.value(QStringLiteral("start_ticks")).toInt(), 24);
		QVERIFY(clipState.value(QStringLiteral("auto_resize")).toBool());
		QCOMPARE(clipState.value(QStringLiteral("content_hash")).toString(),
			propertyResult.value(QStringLiteral("content_hash")).toString());
		verifyCompleted(clipProperties, identity, QStringLiteral("clip"), clipId, clipState);

		const QJsonArray addedNotes{
			QJsonObject{{"position_ticks", 0}, {"length_ticks", 48}, {"key", 60}, {"volume", 100}, {"panning", 0}, {"note_type", "regular"}},
			QJsonObject{{"position_ticks", 96}, {"length_ticks", 48}, {"key", 64}, {"volume", 90}, {"panning", 0}, {"note_type", "regular"}}};
		identity = readStructure().value(QStringLiteral("identity")).toObject();
		clipState = readClip(clipId);
		const auto notesAdded = guarded(identity, QStringLiteral("add_notes"),
			{{"clip_id", clipId}, {"notes", addedNotes}, {"expected_content_hash", clipState.value(QStringLiteral("content_hash"))}},
			{{"kind", "clip"}, {"object_id", clipId}, {"expected_state_hash", clipState.value(QStringLiteral("state_hash"))}});
		QCOMPARE(notesAdded.value(QStringLiteral("data")).toObject().value(QStringLiteral("operation_result")).toObject().value(QStringLiteral("notes_added")).toInt(), 2);
		clipState = readClip(clipId);
		auto midiState = readNotes(clipId);
		const auto addedNotesRead = midiState.value(QStringLiteral("notes")).toArray();
		QCOMPARE(addedNotesRead.size(), 2);
		QCOMPARE(addedNotesRead.at(0).toObject().value(QStringLiteral("position_ticks")).toInt(), 0);
		QCOMPARE(addedNotesRead.at(0).toObject().value(QStringLiteral("key")).toInt(), 60);
		QCOMPARE(addedNotesRead.at(1).toObject().value(QStringLiteral("position_ticks")).toInt(), 96);
		QCOMPARE(addedNotesRead.at(1).toObject().value(QStringLiteral("key")).toInt(), 64);
		verifyCompleted(notesAdded, identity, QStringLiteral("clip"), clipId, clipState);

		identity = readStructure().value(QStringLiteral("identity")).toObject();
		clipState = readClip(clipId);
		const auto transposed = guarded(identity, QStringLiteral("transpose_notes"),
			{{"clip_id", clipId}, {"selection", QJsonObject{{"key_min", 60}, {"key_max", 60}}},
			 {"semitones", 2}, {"expected_content_hash", clipState.value(QStringLiteral("content_hash"))}},
			{{"kind", "clip"}, {"object_id", clipId}, {"expected_state_hash", clipState.value(QStringLiteral("state_hash"))}});
		QCOMPARE(transposed.value(QStringLiteral("data")).toObject().value(QStringLiteral("operation_result")).toObject().value(QStringLiteral("notes_changed")).toInt(), 1);
		clipState = readClip(clipId);
		midiState = readNotes(clipId);
		QCOMPARE(midiState.value(QStringLiteral("notes")).toArray().at(0).toObject().value(QStringLiteral("key")).toInt(), 62);
		QCOMPARE(midiState.value(QStringLiteral("notes")).toArray().at(1).toObject().value(QStringLiteral("key")).toInt(), 64);
		verifyCompleted(transposed, identity, QStringLiteral("clip"), clipId, clipState);

		identity = readStructure().value(QStringLiteral("identity")).toObject();
		clipState = readClip(clipId);
		const auto moved = guarded(identity, QStringLiteral("move_notes"),
			{{"clip_id", clipId}, {"selection", QJsonObject{{"position_start", 96}, {"position_end", 144}}},
			 {"delta_ticks", 24}, {"expected_content_hash", clipState.value(QStringLiteral("content_hash"))}},
			{{"kind", "clip"}, {"object_id", clipId}, {"expected_state_hash", clipState.value(QStringLiteral("state_hash"))}});
		QCOMPARE(moved.value(QStringLiteral("data")).toObject().value(QStringLiteral("operation_result")).toObject().value(QStringLiteral("notes_changed")).toInt(), 1);
		clipState = readClip(clipId);
		midiState = readNotes(clipId);
		QCOMPARE(midiState.value(QStringLiteral("notes")).toArray().at(1).toObject().value(QStringLiteral("position_ticks")).toInt(), 120);
		verifyCompleted(moved, identity, QStringLiteral("clip"), clipId, clipState);

		identity = readStructure().value(QStringLiteral("identity")).toObject();
		clipState = readClip(clipId);
		const auto deleted = guarded(identity, QStringLiteral("delete_notes"),
			{{"clip_id", clipId}, {"selection", QJsonObject{{"position_start", 120}, {"position_end", 168}}},
			 {"expected_content_hash", clipState.value(QStringLiteral("content_hash"))}},
			{{"kind", "clip"}, {"object_id", clipId}, {"expected_state_hash", clipState.value(QStringLiteral("state_hash"))}});
		QCOMPARE(deleted.value(QStringLiteral("data")).toObject().value(QStringLiteral("operation_result")).toObject().value(QStringLiteral("notes_deleted")).toInt(), 1);
		clipState = readClip(clipId);
		midiState = readNotes(clipId);
		QCOMPARE(midiState.value(QStringLiteral("notes")).toArray().size(), 1);
		QCOMPARE(midiState.value(QStringLiteral("notes")).toArray().first().toObject().value(QStringLiteral("key")).toInt(), 62);
		verifyCompleted(deleted, identity, QStringLiteral("clip"), clipId, clipState);

		identity = readStructure().value(QStringLiteral("identity")).toObject();
		clipState = readClip(clipId);
		const auto duplicated = guarded(identity, QStringLiteral("duplicate_region"),
			{{"clip_id", clipId}, {"source_start", 0}, {"source_end", 48}, {"destination_start", 240},
			 {"expected_content_hash", clipState.value(QStringLiteral("content_hash"))}},
			{{"kind", "clip"}, {"object_id", clipId}, {"expected_state_hash", clipState.value(QStringLiteral("state_hash"))}});
		QCOMPARE(duplicated.value(QStringLiteral("data")).toObject().value(QStringLiteral("operation_result")).toObject().value(QStringLiteral("notes_added")).toInt(), 1);
		clipState = readClip(clipId);
		midiState = readNotes(clipId);
		QCOMPARE(midiState.value(QStringLiteral("notes")).toArray().size(), 2);
		QCOMPARE(midiState.value(QStringLiteral("notes")).toArray().at(1).toObject().value(QStringLiteral("position_ticks")).toInt(), 240);
		QCOMPARE(midiState.value(QStringLiteral("notes")).toArray().at(1).toObject().value(QStringLiteral("key")).toInt(), 62);
		verifyCompleted(duplicated, identity, QStringLiteral("clip"), clipId, clipState);

		QVERIFY(source.open(QIODevice::ReadOnly));
		QCOMPARE(source.readAll(), initialProjectBytes);
	}

	void rejectsChangedProjectIdentityWithoutWrites()
	{
		using namespace lmms;
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX)
		QSKIP("Guarded mutations require verified Windows or Linux IPC peer identity");
#endif
		Engine::getSong()->createNewProject();
		QTemporaryDir root;
		QVERIFY(root.isValid());
		const auto projectPath = root.filePath(QStringLiteral("guarded-identity-edit.mmp"));
		QVERIFY(Engine::getSong()->saveProjectFile(projectPath, false));
		Engine::getSong()->loadProject(projectPath);

		AiBridgeIpcServer server(nullptr, QStringLiteral("lmms-guarded-identity-edit-test"));
		QVERIFY(server.start());
		QLocalSocket socket;
		socket.connectToServer(server.endpointName());
		QVERIFY(socket.waitForConnected(1000));
		auto call = [&socket](const QString &action, const QJsonObject &parameters) {
			socket.write(QJsonDocument(QJsonObject{{"version", 2}, {"request_id", "identity-edit-test"},
				{"action", action}, {"parameters", parameters}}).toJson(QJsonDocument::Compact) + '\n');
			socket.flush();
			QElapsedTimer timer;
			timer.start();
			QByteArray bytes;
			while (!bytes.endsWith('\n') && timer.elapsed() < 3000)
			{
				QTest::qWait(1);
				bytes += socket.readAll();
			}
			return QJsonDocument::fromJson(bytes).object();
		};
		auto readStructure = [&]() {
			return call(QStringLiteral("get_project_structure"), {}).value(QStringLiteral("data")).toObject();
		};

		auto *changedTrack = Track::create(Track::Type::Instrument, Engine::getSong());
		QVERIFY(changedTrack);
		changedTrack->setName(QStringLiteral("Existing track"));
		Engine::getSong()->setModified();
		const auto identityBeforeSecondEdit = readStructure().value(QStringLiteral("identity")).toObject();
		const auto revisionBeforeSecondEdit = identityBeforeSecondEdit.value(QStringLiteral("identity_revision")).toString();
		const auto hashBeforeSecondEdit = identityBeforeSecondEdit.value(QStringLiteral("document_state_hash")).toString();
		changedTrack->setName(QStringLiteral("Changed after identity was read"));
		const auto identityAfterSecondEdit = readStructure().value(QStringLiteral("identity")).toObject();
		QCOMPARE(identityAfterSecondEdit.value(QStringLiteral("identity_revision")).toString(), revisionBeforeSecondEdit);
		QVERIFY(identityAfterSecondEdit.value(QStringLiteral("document_state_hash")).toString() != hashBeforeSecondEdit);

		const auto tracksBeforeRejectedWrite = Engine::getSong()->tracks().size();
		const auto staleAfterSecondEdit = call(QStringLiteral("guarded_mutate_project"),
			{{"expected_project_identity", identityBeforeSecondEdit}, {"operation", "create_instrument_track"},
			 {"parameters", QJsonObject{{"name", "Must not be created"}}}});
		QCOMPARE(staleAfterSecondEdit.value(QStringLiteral("code")).toString(), QStringLiteral("stale_project_identity"));
		QCOMPARE(Engine::getSong()->tracks().size(), tracksBeforeRejectedWrite);
		QCOMPARE(readStructure().value(QStringLiteral("identity")).toObject().value(QStringLiteral("document_state_hash")).toString(),
			identityAfterSecondEdit.value(QStringLiteral("document_state_hash")).toString());

		auto changedProcessIdentity = identityAfterSecondEdit;
		changedProcessIdentity.insert(QStringLiteral("process_session_id"), QStringLiteral("different-process-session"));
		const auto staleProcess = call(QStringLiteral("guarded_mutate_project"),
			{{"expected_project_identity", changedProcessIdentity}, {"operation", "create_instrument_track"},
			 {"parameters", QJsonObject{{"name", "Must not be created either"}}}});
		QCOMPARE(staleProcess.value(QStringLiteral("code")).toString(), QStringLiteral("stale_project_identity"));
		QCOMPARE(Engine::getSong()->tracks().size(), tracksBeforeRejectedWrite);
		QCOMPARE(readStructure().value(QStringLiteral("identity")).toObject().value(QStringLiteral("document_state_hash")).toString(),
			identityAfterSecondEdit.value(QStringLiteral("document_state_hash")).toString());
	}

	void rejectsReloadedProjectIdentityWithoutWrites()
	{
		using namespace lmms;
#if !defined(Q_OS_WIN) && !defined(Q_OS_LINUX)
		QSKIP("Guarded mutations require verified Windows or Linux IPC peer identity");
#endif
		Engine::getSong()->createNewProject();
		QTemporaryDir root;
		QVERIFY(root.isValid());
		const auto firstPath = root.filePath(QStringLiteral("guarded-first.mmp"));
		const auto secondPath = root.filePath(QStringLiteral("guarded-second.mmp"));
		QVERIFY(Engine::getSong()->saveProjectFile(firstPath, false));
		Engine::getSong()->loadProject(firstPath);

		AiBridgeIpcServer server(nullptr, QStringLiteral("lmms-guarded-reload-identity-test"));
		QVERIFY(server.start());
		QLocalSocket socket;
		socket.connectToServer(server.endpointName());
		QVERIFY(socket.waitForConnected(1000));
		auto call = [&socket](const QString &action, const QJsonObject &parameters) {
			socket.write(QJsonDocument(QJsonObject{{"version", 2}, {"request_id", "reload-identity-test"},
				{"action", action}, {"parameters", parameters}}).toJson(QJsonDocument::Compact) + '\n');
			socket.flush();
			QElapsedTimer timer;
			timer.start();
			QByteArray bytes;
			while (!bytes.endsWith('\n') && timer.elapsed() < 3000)
			{
				QTest::qWait(1);
				bytes += socket.readAll();
			}
			return QJsonDocument::fromJson(bytes).object();
		};
		auto readStructure = [&]() {
			return call(QStringLiteral("get_project_structure"), {}).value(QStringLiteral("data")).toObject();
		};
		const auto identityFromFirstProject = readStructure().value(QStringLiteral("identity")).toObject();

		Engine::getSong()->createNewProject();
		QVERIFY(Engine::getSong()->saveProjectFile(secondPath, false));
		Engine::getSong()->loadProject(secondPath);
		const auto secondProjectStructure = readStructure();
		const auto secondProjectIdentity = secondProjectStructure.value(QStringLiteral("identity")).toObject();
		QVERIFY(secondProjectIdentity.value(QStringLiteral("project_load_generation")).toString().toULongLong()
			> identityFromFirstProject.value(QStringLiteral("project_load_generation")).toString().toULongLong());
		const auto secondProjectHash = secondProjectIdentity.value(QStringLiteral("document_state_hash")).toString();
		const auto secondProjectTrackCount = Engine::getSong()->tracks().size();
		const auto secondProjectTempo = Engine::getSong()->getTempo();
		const auto rejected = call(QStringLiteral("guarded_mutate_project"),
			{{"expected_project_identity", identityFromFirstProject}, {"operation", "create_instrument_track"},
			 {"parameters", QJsonObject{{"name", "Must not enter the reloaded project"}}}});
		QCOMPARE(rejected.value(QStringLiteral("code")).toString(), QStringLiteral("stale_project_identity"));
		QCOMPARE(Engine::getSong()->tracks().size(), secondProjectTrackCount);
		QCOMPARE(Engine::getSong()->getTempo(), secondProjectTempo);
		QCOMPARE(readStructure().value(QStringLiteral("identity")).toObject().value(QStringLiteral("document_state_hash")).toString(), secondProjectHash);
		QVERIFY(!QJsonDocument(rejected).toJson().contains(root.path().toUtf8()));
	}
};

QTEST_MAIN(AiBridgeIpcServerTest)
#include "AiBridgeIpcServerTest.moc"
