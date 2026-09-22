#include <QtTest>

#include "AiBridgeProtocol.h"

namespace
{
QByteArray request(const QByteArray &action, const QByteArray &parameters)
{
	return "{\"version\":2,\"request_id\":\"phase3h\",\"action\":\"" + action + "\",\"parameters\":" + parameters + "}\n";
}

void requireRejected(const QByteArray &action, const QByteArray &parameters, const QString &code)
{
	const auto result = lmms::AiBridgeProtocol::parseRequest(request(action, parameters));
	QVERIFY(!result.success);
	QCOMPARE(result.code, code);
}
}

class AiBridgePhase3HSecurityTest : public QObject
{
	Q_OBJECT

private slots:
	void requiresExplicitBoundedCopiedScope()
	{
		requireRejected("quantize_notes", "{\"grid_ticks\":24}", "invalid_parameter");
		requireRejected("quantize_notes", "{\"clips\":[{\"clip_id\":\"clip\",\"expected_midi_state_hash\":\"hash\",\"selection\":{\"mode\":\"predicate\",\"start_tick\":0,\"end_tick\":24,\"ui_selection\":true}}],\"grid_ticks\":24}", "invalid_selection");
		requireRejected("midi_transform", "{}", "unknown_action");
		requireRejected("quantize_notes", "{\"clips\":[{\"clip_id\":\"clip\",\"expected_midi_state_hash\":\"hash\",\"selection\":{\"mode\":\"note_refs\",\"note_refs\":[\"native_index:7\"]}}],\"grid_ticks\":24,\"property\":\"key\"}", "invalid_parameter");
	}

	void rejectsAllFrozenPreflightBounds()
	{
		QByteArray clips = "{\"clips\":[";
		for (int i = 0; i < 33; ++i) { if (i) clips += ','; clips += "{\"clip_id\":\"clip" + QByteArray::number(i) + "\",\"expected_midi_state_hash\":\"hash\",\"selection\":{\"mode\":\"predicate\"}}"; }
		clips += "],\"grid_ticks\":24}";
		requireRejected("quantize_notes", clips, "parameter_out_of_range");

		QByteArray refs = "{\"clips\":[{\"clip_id\":\"clip\",\"expected_midi_state_hash\":\"hash\",\"selection\":{\"mode\":\"note_refs\",\"note_refs\":[";
		for (int i = 0; i < 2049; ++i) { if (i) refs += ','; refs += "\"r" + QByteArray::number(i) + "\""; }
		refs += "]}}],\"grid_ticks\":24}";
		requireRejected("quantize_notes", refs, "note_limit_exceeded");
		requireRejected("get_midi_notes", "{\"clip_id\":\"clip\",\"limit\":513}", "parameter_out_of_range");
		requireRejected("analyze_midi", "{\"clip_ids\":[\"clip\"],\"max_notes\":4097}", "parameter_out_of_range");
	}
};

QTEST_MAIN(AiBridgePhase3HSecurityTest)
#include "AiBridgePhase3HSecurityTest.moc"
