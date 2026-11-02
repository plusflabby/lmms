#include <QtTest>

#include "AiBridgeVocalRequest.h"

using namespace lmms;

class AiBridgeVocalEditWireTest : public QObject
{
	Q_OBJECT

private slots:
	void admitsStrictDirectPlanRequest()
	{
		const QJsonObject selection{
			{"clip_id", "clip"},
			{"expected_audio_state_hash", QString(64, 'a')},
			{"source_basis", "source_pcm"},
			{"source_frames", QJsonObject{{"in_frame", "0"}, {"out_frame", "48000"}}}};
		const QJsonObject request{
			{"recipe_version", 1},
			{"expected_project_generation", "1"},
			{"expected_world_hash", QString(64, 'b')},
			{"selections", QJsonArray{selection}},
			{"input_kind", "direct_source"},
			{"input_selection_index", 0},
			{"operations", QJsonArray{QJsonObject{{"operation","gain_envelope"},{"points",QJsonArray{
				QJsonObject{{"render_input_frame","0"},{"gain_db",0.0}},
				QJsonObject{{"render_input_frame","48000"},{"gain_db",0.0}}}}}}}};

		QVERIFY(VocalRequest::handles(QStringLiteral("plan_vocal_edit")));
		QVERIFY(VocalRequest::valid(QStringLiteral("plan_vocal_edit"), request));
	}

	void rejectsPrivateAndNoncanonicalPlanFields()
	{
		QJsonObject request{
			{"recipe_version", 1},
			{"expected_project_generation", "1"},
			{"expected_world_hash", QString(64, 'b')},
			{"selections", QJsonArray{QJsonObject{
				{"clip_id", "clip"},
				{"expected_audio_state_hash", QString(64, 'a')},
				{"source_basis", "source_pcm"},
				{"source_frames", QJsonObject{{"in_frame", "00"}, {"out_frame", "48000"}}}}}},
			{"input_kind", "direct_source"},
			{"input_selection_index", 0},
			{"operations", QJsonArray{QJsonObject{{"operation","gain_envelope"},{"points",QJsonArray{
				QJsonObject{{"render_input_frame","0"},{"gain_db",0.0}},
				QJsonObject{{"render_input_frame","48000"},{"gain_db",0.0}}}}}}}};
		QVERIFY(!VocalRequest::valid(QStringLiteral("plan_vocal_edit"), request));
		request.insert(QStringLiteral("path"), QStringLiteral("G:/private.wav"));
		QVERIFY(!VocalRequest::valid(QStringLiteral("plan_vocal_edit"), request));
	}

	void admitsOnlyTheClosedBOperationContracts()
	{
		const QJsonObject render{{"plan_id","00000000-0000-4000-8000-000000000001"},
			{"plan_hash",QString(64,'a')},{"dry_run",true}};
		QVERIFY(VocalRequest::handles(QStringLiteral("render_vocal_variant")));
		QVERIFY(VocalRequest::valid(QStringLiteral("render_vocal_variant"),render));
		QVERIFY(!VocalRequest::valid(QStringLiteral("render_vocal_variant"),QJsonObject{{"plan_id",render["plan_id"]},
			{"plan_hash",render["plan_hash"]},{"dry_run",true},{"path","G:/private.wav"}}));
		QVERIFY(VocalRequest::valid(QStringLiteral("get_vocal_variant"),
			{{"variant_id","00000000-0000-4000-8000-000000000001"}}));
		const QJsonObject apply{{"variant_id","00000000-0000-4000-8000-000000000001"},
			{"clip_id","clip"},{"mode","replace"},{"expected_project_generation","1"},
			{"expected_world_hash",QString(64,'b')},{"expected_audio_state_hash",QString(64,'a')},
			{"expected_track_state_hash",QString(64,'c')},{"dry_run",false}};
		QVERIFY(VocalRequest::valid(QStringLiteral("apply_vocal_variant"),apply));
		QVERIFY(!VocalRequest::valid(QStringLiteral("apply_vocal_variant"),QJsonObject{{"variant_id",apply["variant_id"]},
			{"clip_id","clip"},{"mode","delete"},{"expected_project_generation","1"},
			{"expected_world_hash",QString(64,'b')},{"expected_audio_state_hash",QString(64,'a')},
			{"expected_track_state_hash",QString(64,'c')},{"dry_run",false}}));
	}
};

QTEST_GUILESS_MAIN(AiBridgeVocalEditWireTest)
#include "AiBridgeVocalEditWireTest.moc"
