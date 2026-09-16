#include <QtTest>

#include "AiBridgeProtocol.h"

class AiBridgePhase3GProtocolTest : public QObject
{
	Q_OBJECT

private slots:
	void acceptsArrangementAndAutomationActionNames()
	{
		const QStringList actions = {
			QStringLiteral("get_arrangement"), QStringLiteral("get_timeline_region"), QStringLiteral("set_timeline_clip"),
			QStringLiteral("duplicate_timeline_clip"), QStringLiteral("delete_timeline_clip"), QStringLiteral("batch_edit_timeline_clips"),
			QStringLiteral("list_automation_targets"), QStringLiteral("get_automation_capabilities"), QStringLiteral("list_automation"),
			QStringLiteral("get_automation_clip"), QStringLiteral("create_automation_track"), QStringLiteral("delete_automation_track"),
			QStringLiteral("create_automation_clip"), QStringLiteral("delete_automation_clip"), QStringLiteral("set_automation_clip_properties"),
			QStringLiteral("set_automation_clip_target"), QStringLiteral("edit_automation_points"), QStringLiteral("duplicate_arrangement_region"),
			QStringLiteral("move_arrangement_region"), QStringLiteral("delete_arrangement_region"), QStringLiteral("insert_arrangement_space"),
			QStringLiteral("close_arrangement_gap"), QStringLiteral("replace_arrangement_region")};
		for (const auto &action : actions)
		{
			QJsonObject parameters;
			if (action == QStringLiteral("get_timeline_region"))
			{
				parameters.insert(QStringLiteral("start_tick"), 0);
				parameters.insert(QStringLiteral("end_tick"), 480);
			}
			else if (action == QStringLiteral("set_timeline_clip") || action == QStringLiteral("delete_timeline_clip"))
				parameters.insert(QStringLiteral("clip_id"), QStringLiteral("clip:test"));
			else if (action == QStringLiteral("duplicate_timeline_clip"))
			{
				parameters.insert(QStringLiteral("clip_id"), QStringLiteral("clip:test"));
				parameters.insert(QStringLiteral("destination_tick"), 960);
			}
			else if (action == QStringLiteral("batch_edit_timeline_clips"))
				parameters.insert(QStringLiteral("edits"), QJsonArray{QJsonObject{{"clip_id", "clip:test"}}});
			else if (action == QStringLiteral("get_automation_clip"))
				parameters.insert(QStringLiteral("automation_clip_id"), QStringLiteral("automation-clip:test"));
			else if (action == QStringLiteral("delete_automation_track"))
				parameters.insert(QStringLiteral("automation_track_id"), QStringLiteral("automation-track:test"));
			else if (action == QStringLiteral("delete_automation_clip") || action == QStringLiteral("set_automation_clip_properties"))
			{
				parameters.insert(QStringLiteral("automation_clip_id"), QStringLiteral("automation-clip:test"));
				if (action == QStringLiteral("set_automation_clip_properties")) parameters.insert(QStringLiteral("start_tick"), 0);
			}
			else if (action == QStringLiteral("create_automation_clip"))
		{
				parameters.insert(QStringLiteral("automation_track_id"), QStringLiteral("automation-track:test"));
				parameters.insert(QStringLiteral("start_tick"), 0);
				parameters.insert(QStringLiteral("length_ticks"), 480);
			}
			else if (action == QStringLiteral("set_automation_clip_target"))
		{
				parameters.insert(QStringLiteral("automation_clip_id"), QStringLiteral("automation-clip:test"));
				parameters.insert(QStringLiteral("automation_target_id"), QStringLiteral("automation-target:test"));
			}
			else if (action == QStringLiteral("edit_automation_points"))
		{
				parameters.insert(QStringLiteral("automation_clip_id"), QStringLiteral("automation-clip:test"));
				parameters.insert(QStringLiteral("mutations"), QJsonArray{QJsonObject{{"operation", "add"}, {"tick", 0}, {"value", 0.5}}});
			}
			const auto parsed = lmms::AiBridgeProtocol::parseRequest(
				QJsonDocument(QJsonObject{{"version", 2}, {"request_id", action}, {"action", action}, {"parameters", parameters}}).toJson(QJsonDocument::Compact) + '\n');
			QVERIFY2(parsed.success, qPrintable(action + QStringLiteral(": ") + parsed.code));
		}
	}
};

QTEST_MAIN(AiBridgePhase3GProtocolTest)
#include "AiBridgePhase3GProtocolTest.moc"
