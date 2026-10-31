#include "AiBridgeProtocol.h"
#include "AiBridgeRecordingTypes.h"
#include "RecordingTypes.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>
#include <limits>

using namespace lmms;

namespace
{
const QString idA = QStringLiteral("00000000-0000-4000-8000-000000000001");
const QString idB = QStringLiteral("00000000-0000-4000-8000-000000000002");

AiBridgeProtocolParseResult parse(const QString& action, const QJsonObject& parameters)
{
	return AiBridgeProtocol::parseRequest(QJsonDocument(QJsonObject{
		{QStringLiteral("version"), 2},
		{QStringLiteral("request_id"), QStringLiteral("recording-protocol-test")},
		{QStringLiteral("action"), action},
		{QStringLiteral("parameters"), parameters}}).toJson(QJsonDocument::Compact) + '\n');
}
}

class AiBridgeRecordingProtocolTest final : public QObject
{
	Q_OBJECT
private slots:
	void allowsOnlyTheSixApprovedStrictRecordingRequests()
	{
		const QVector<QPair<QString, QJsonObject>> valid{
			{QStringLiteral("get_recording_capabilities"), {}},
			{QStringLiteral("get_recording_status"), {}},
			{QStringLiteral("prepare_recording_take"), {{"start_tick", 0}, {"take_label", "Take A"},
				{"count_in_bars", 0}, {"metronome_enabled", false}, {"expected_project_generation", "1"}}},
			{QStringLiteral("start_recording_take"), {{"prepared_take_id", idA},
				{"expected_project_generation", "1"}, {"start_request_id", idB}}},
			{QStringLiteral("stop_recording_take"), {{"take_id", idA}}},
			{QStringLiteral("list_recorded_takes"), {}}};
		for (const auto& request : valid)
		{
			const auto result = parse(request.first, request.second);
			QVERIFY2(result.success, qPrintable(request.first + QStringLiteral(": ") + result.code));
			QCOMPARE(result.request.action, request.first);
			QCOMPARE(result.request.requestId, QStringLiteral("recording-protocol-test"));
		}
		for (const auto* action : {"arm_recording_input", "select_recording_device", "set_recording_root",
			"attach_recorded_take", "set_recording_placement"})
			QCOMPARE(parse(QString::fromLatin1(action), {}).code, QStringLiteral("unknown_action"));
	}

	void unknownFieldsAndWrongTypesRejected()
	{
		const auto emptyPrepare = QJsonObject{{"start_tick", 0}, {"take_label", "Take A"},
			{"count_in_bars", 0}, {"metronome_enabled", false}, {"expected_project_generation", "1"}};
		auto bad = emptyPrepare;
		bad.insert(QStringLiteral("input_device_id"), QStringLiteral("private-endpoint"));
		QVERIFY(!parse(QStringLiteral("prepare_recording_take"), bad).success);
		bad = emptyPrepare;
		bad.insert(QStringLiteral("recording_root"), QStringLiteral("C:/private/recording"));
		QVERIFY(!parse(QStringLiteral("prepare_recording_take"), bad).success);
		bad = emptyPrepare;
		bad.insert(QStringLiteral("source_in_ticks"), 24);
		QVERIFY(!parse(QStringLiteral("prepare_recording_take"), bad).success);
		QVERIFY(!parse(QStringLiteral("get_recording_status"), {{"detail", true}}).success);
		QVERIFY(!parse(QStringLiteral("get_recording_capabilities"), {{"device_id", "private-endpoint"}}).success);

		bad = emptyPrepare;
		bad.insert(QStringLiteral("start_tick"), 0.5);
		QVERIFY(!parse(QStringLiteral("prepare_recording_take"), bad).success);
		bad = emptyPrepare;
		bad.insert(QStringLiteral("count_in_bars"), 3);
		QVERIFY(!parse(QStringLiteral("prepare_recording_take"), bad).success);
		bad = emptyPrepare;
		bad.insert(QStringLiteral("metronome_enabled"), QStringLiteral("false"));
		QVERIFY(!parse(QStringLiteral("prepare_recording_take"), bad).success);
		bad = emptyPrepare;
		bad.insert(QStringLiteral("expected_project_generation"), 1);
		QVERIFY(!parse(QStringLiteral("prepare_recording_take"), bad).success);

		QVERIFY(!parse(QStringLiteral("start_recording_take"), {{"prepared_take_id", idA},
			{"expected_project_generation", "1"}, {"start_request_id", "bad"}, {"retry", true}}).success);
		QVERIFY(!parse(QStringLiteral("stop_recording_take"), {{"take_id", idA}, {"device_id", "private-endpoint"}}).success);
	}

	void generationsMustBeCanonicalUnsignedStrings()
	{
		const auto prepare = [](const QJsonValue& generation) {
			return parse(QStringLiteral("prepare_recording_take"), {{"start_tick", 0}, {"take_label", "Take A"},
				{"count_in_bars", 0}, {"metronome_enabled", false}, {"expected_project_generation", generation}});
		};
		QVERIFY(prepare(QStringLiteral("1")).success);
		for (const QJsonValue& generation : {QJsonValue(1), QJsonValue(QStringLiteral("01")),
			QJsonValue(QStringLiteral("+1")), QJsonValue(QStringLiteral("")), QJsonValue(QStringLiteral("18446744073709551616"))})
			QVERIFY(!prepare(generation).success);
	}

	void takeIdsAreCanonicalUuidFields()
	{
		QVERIFY(parse(QStringLiteral("stop_recording_take"), {{"take_id", idA}}).success);
		QVERIFY(!parse(QStringLiteral("stop_recording_take"), {{"take_id", "not-a-uuid"}}).success);
		QVERIFY(parse(QStringLiteral("start_recording_take"), {{"prepared_take_id", idA},
			{"expected_project_generation", "1"}, {"start_request_id", idA}}).success);
	}

	void pagingIsBoundedAndDefaultsToTheApprovedPage()
	{
		QVERIFY(parse(QStringLiteral("list_recorded_takes"), {}).success);
		QVERIFY(parse(QStringLiteral("list_recorded_takes"), {{"offset", 0}, {"limit", 100}}).success);
		QVERIFY(!parse(QStringLiteral("list_recorded_takes"), {{"offset", -1}}).success);
		QVERIFY(!parse(QStringLiteral("list_recorded_takes"), {{"limit", 0}}).success);
		QVERIFY(!parse(QStringLiteral("list_recorded_takes"), {{"limit", 101}}).success);
		QVERIFY(!parse(QStringLiteral("list_recorded_takes"), {{"limit", 2.5}}).success);
		QVERIFY(!parse(QStringLiteral("list_recorded_takes"), {{"offset", 0}, {"limit", 25}, {"path", "private"}}).success);
	}

	void statusCountsUseCanonicalStringsAndUnknownHardwareLossIsNull()
	{
		recording::RecordingStatus status;
		status.projectGeneration = std::numeric_limits<quint64>::max();
		status.receivedFrames = std::numeric_limits<quint64>::max();
		status.acceptedFrames = 48000;
		status.writtenFrames = 48000;
		status.applicationDroppedFrames = 0;
		status.hardwareLossCountKnown = false;
		status.hardwareLostFrames = 0;
		const auto value = AiBridgeRecordingTypes::statusJson(status);
		for (const auto* key : {"project_generation", "received_frames", "accepted_frames", "written_frames",
			"application_dropped_frames", "over_limit_frames", "clipped_samples", "invalid_samples",
			"observations_dropped"})
			QVERIFY2(value.value(QString::fromLatin1(key)).isString(), key);
		QCOMPARE(value.value(QStringLiteral("project_generation")).toString(),
			QStringLiteral("18446744073709551615"));
		QCOMPARE(value.value(QStringLiteral("received_frames")).toString(),
			QStringLiteral("18446744073709551615"));
		QVERIFY(!value.value(QStringLiteral("hardware_loss_count_known")).toBool());
		QVERIFY(value.value(QStringLiteral("hardware_lost_frames")).isNull());
		QCOMPARE(value.value(QStringLiteral("placement_quality")).toString(), QStringLiteral("uncalibrated"));
		QCOMPARE(value.value(QStringLiteral("drift_quality")).toString(), QStringLiteral("unmeasured"));
		QVERIFY(!value.value(QStringLiteral("sample_accuracy_verified")).toBool());

		status.hardwareLossCountKnown = true;
		status.hardwareLostFrames = 9;
		QCOMPARE(AiBridgeRecordingTypes::statusJson(status).value(QStringLiteral("hardware_lost_frames")).toString(),
			QStringLiteral("9"));
	}
};

QTEST_GUILESS_MAIN(AiBridgeRecordingProtocolTest)
#include "AiBridgeRecordingProtocolTest.moc"
