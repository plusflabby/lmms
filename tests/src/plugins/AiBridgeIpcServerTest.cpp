#include <QtTest>

#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QFile>
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
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Note.h"
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
		QVERIFY(identity.value(QStringLiteral("server_pid")).toInt() > 0);
		QCOMPARE(identity.value(QStringLiteral("availability")).toString(), QStringLiteral("available"));
		QVERIFY(identity.value(QStringLiteral("client_pid")).toInt() > 0);
		QCOMPARE(identity.value(QStringLiteral("client_pid")).toInt(), static_cast<int>(QCoreApplication::applicationPid()));
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
		QVERIFY(!loaded.value(QStringLiteral("loaded_project")).toObject().value(QStringLiteral("windows_file_id")).toString().isEmpty());
		QVERIFY(loaded.value(QStringLiteral("project_load_generation")).toString().toULongLong() > identity.value(QStringLiteral("project_load_generation")).toString().toULongLong());

		Engine::getSong()->setModified();
		socket.write(QByteArrayLiteral("{\"version\":2,\"request_id\":\"identity-modified\",\"action\":\"get_project_structure\",\"parameters\":{}}\n"));
		socket.flush();
		QTRY_VERIFY_WITH_TIMEOUT(socket.bytesAvailable() > 0, 1000);
		const auto modified = QJsonDocument::fromJson(socket.readAll()).object().value(QStringLiteral("data")).toObject().value(QStringLiteral("identity")).toObject();
		QCOMPARE(modified.value(QStringLiteral("document_state")).toString(), QStringLiteral("modified"));
		QVERIFY(modified.value(QStringLiteral("dirty")).toBool());
		QVERIFY(modified.value(QStringLiteral("identity_revision")).toString().toULongLong() > loaded.value(QStringLiteral("identity_revision")).toString().toULongLong());
	}
};

QTEST_MAIN(AiBridgeIpcServerTest)
#include "AiBridgeIpcServerTest.moc"
