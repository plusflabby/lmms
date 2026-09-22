#include <QtTest>

#include "AiBridgeProtocol.h"

namespace
{
QByteArray request(const char *action, const QByteArray &parameters)
{
	return QByteArray("{\"version\":2,\"request_id\":\"midi\",\"action\":\"")
		+ action + "\",\"parameters\":" + parameters + "}\n";
}

void requireAccepted(const char *action, const QByteArray &parameters)
{
	const auto result = lmms::AiBridgeProtocol::parseRequest(request(action, parameters));
	QVERIFY2(result.success, qPrintable(result.code + QStringLiteral(": ") + result.message));
}

void requireRejected(const char *action, const QByteArray &parameters, const QString &code)
{
	const auto result = lmms::AiBridgeProtocol::parseRequest(request(action, parameters));
	QVERIFY(!result.success);
	QCOMPARE(result.code, code);
}
}

class AiBridgeMidiProtocolTest : public QObject
{
	Q_OBJECT

private slots:
	void acceptsExactPhase3HActionAllowlist()
	{
		requireAccepted("get_midi_capabilities", "{}");
		requireAccepted("get_midi_notes", "{\"clip_id\":\"clip\",\"offset\":0,\"limit\":512}");
		requireAccepted("analyze_midi", "{\"clip_ids\":[\"clip\"],\"grid_ticks\":24}");
		const QByteArray mutation = "{\"clips\":[{\"clip_id\":\"clip\",\"expected_midi_state_hash\":\"hash\",\"selection\":{\"mode\":\"predicate\",\"start_tick\":0,\"end_tick\":96}}],\"dry_run\":true";
		requireAccepted("quantize_notes", mutation + ",\"grid_ticks\":24,\"strength\":1,\"direction\":\"nearest\",\"boundary_policy\":\"clamp\"}");
		requireAccepted("humanize_notes", mutation + ",\"seed\":1,\"timing_range_ticks\":2,\"volume_range\":4,\"length_range_ticks\":3,\"distribution\":\"triangular\",\"boundary_policy\":\"clamp\"}");
		requireAccepted("apply_groove", mutation + ",\"mode\":\"swing\",\"subdivision_ticks\":24,\"swing_percent\":60,\"strength\":0.5,\"boundary_policy\":\"clamp\"}");
		requireAccepted("shape_note_velocities", mutation + ",\"mode\":\"ramp\",\"start_value\":60,\"end_value\":120,\"boundary_policy\":\"clamp\"}");
		requireAccepted("shape_note_lengths", mutation + ",\"mode\":\"gate_to_next\",\"gate_percent\":75,\"fallback_length_ticks\":24,\"boundary_policy\":\"clamp\"}");
		requireAccepted("cleanup_notes", mutation + ",\"remove_exact_duplicates\":true,\"same_pitch_overlap_mode\":\"trim_earlier\",\"minimum_length_ticks\":12,\"short_note_action\":\"extend\"}");
		requireAccepted("constrain_notes_to_scale", mutation + ",\"allowed_pitch_classes\":[0,2,4,5,7,9,11],\"direction\":\"nearest\",\"nearest_tie_break\":\"down\"}");

		for (const auto action : {"get_midi_capability", "get_midi_note", "quantize_midi", "set_midi_notes"})
			requireRejected(action, "{}", QStringLiteral("unknown_action"));
	}

	void rejectsInvalidSelectionHashesAndBounds()
	{
		const QByteArray base = "{\"clips\":[{\"clip_id\":\"clip\",\"expected_midi_state_hash\":\"hash\",\"selection\":";
		requireRejected("quantize_notes", base + "{\"mode\":\"note_refs\",\"note_refs\":[\"ref\"],\"start_tick\":0}}],\"grid_ticks\":24}", QStringLiteral("invalid_selection"));
		requireRejected("quantize_notes", "{\"clips\":[{\"clip_id\":\"clip\",\"selection\":{\"mode\":\"predicate\",\"start_tick\":0,\"end_tick\":24}}],\"grid_ticks\":24}", QStringLiteral("invalid_parameter"));
		requireRejected("quantize_notes", base + "{\"mode\":\"predicate\",\"start_tick\":24,\"end_tick\":24}}],\"grid_ticks\":24}", QStringLiteral("invalid_selection"));
		requireRejected("get_midi_notes", "{\"clip_id\":\"clip\",\"offset\":0,\"limit\":513}", QStringLiteral("parameter_out_of_range"));
		requireRejected("analyze_midi", "{\"clip_ids\":[\"clip\"],\"max_notes\":4097}", QStringLiteral("parameter_out_of_range"));
	}

	void rejectsInvalidPerToolUnions()
	{
		const QByteArray mutation = "{\"clips\":[{\"clip_id\":\"clip\",\"expected_midi_state_hash\":\"hash\",\"selection\":{\"mode\":\"predicate\",\"start_tick\":0,\"end_tick\":96}}]";
		requireRejected("quantize_notes", mutation + ",\"grid_ticks\":0}", QStringLiteral("invalid_grid"));
		requireRejected("humanize_notes", mutation + ",\"seed\":-1}", QStringLiteral("parameter_out_of_range"));
		requireRejected("apply_groove", mutation + ",\"mode\":\"swing\",\"subdivision_ticks\":24,\"timing_offsets\":[1]}", QStringLiteral("invalid_parameter"));
		requireRejected("shape_note_velocities", mutation + ",\"mode\":\"set\",\"value\":\"201\"}", QStringLiteral("invalid_parameter"));
		requireRejected("shape_note_lengths", mutation + ",\"mode\":\"gate_to_next\",\"value\":10}", QStringLiteral("invalid_parameter"));
		requireRejected("cleanup_notes", mutation + ",\"short_note_action\":\"extend\"}", QStringLiteral("invalid_parameter"));
		requireRejected("constrain_notes_to_scale", mutation + ",\"allowed_pitch_classes\":[0,12]}", QStringLiteral("invalid_pitch_class"));
	}

	void preservesMachineReadableMidiErrors()
	{
		for (const auto code : {"midi_state_mismatch", "stale_note_ref", "invalid_selection", "invalid_grid", "invalid_pitch_class", "parameter_out_of_range", "note_limit_exceeded", "destination_out_of_bounds", "no_effect", "plan_stale", "post_verification_failed", "partial_failure"})
		{
			const auto frame = lmms::AiBridgeProtocol::serializeResponse({2, "midi", false, QString::fromLatin1(code), "failed"});
			QVERIFY(frame.contains(QByteArray("\"code\":\"") + code + "\""));
			QVERIFY(frame.endsWith('\n'));
		}
	}
};

QTEST_MAIN(AiBridgeMidiProtocolTest)
#include "AiBridgeMidiProtocolTest.moc"
