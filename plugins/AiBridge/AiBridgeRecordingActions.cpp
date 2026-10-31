#include "AiBridgeRecordingActions.h"
#include "RecordingSessionManager.h"

namespace lmms
{
AiBridgeRecordingActions::AiBridgeRecordingActions(recording::RecordingSessionManager* manager)
	: m_manager(manager), m_owner(QThread::currentThread())
{
}

AiBridgeProtocolResponse AiBridgeRecordingActions::execute(const AiBridgeProtocolRequest& request) const
{
	if (QThread::currentThread() != m_owner)
		return {2, request.requestId, false, QStringLiteral("wrong_thread"), QStringLiteral("Recording control requires the LMMS control thread")};
	if (!m_manager)
		return {2, request.requestId, false, QStringLiteral("recording_unavailable"), QStringLiteral("Recording service is unavailable")};

	const auto parsed = AiBridgeRecordingTypes::parse(request.action, request.parameters);
	if (!parsed.success)
		return {2, request.requestId, false, parsed.code, parsed.message};

	const auto reply = [&](const recording::RecordingReply& result) {
		return AiBridgeProtocolResponse{2, request.requestId, result.ok, result.code,
			result.ok ? QStringLiteral("Recording request accepted") : QStringLiteral("Recording request was refused"),
			AiBridgeRecordingTypes::statusJson(result.status)};
	};

	switch (parsed.request.operation)
	{
	case AiBridgeRecordingOperation::GetCapabilities:
		return {2, request.requestId, true, QStringLiteral("ok"), QStringLiteral("Recording capabilities read"),
			AiBridgeRecordingTypes::capabilitiesJson(m_manager->capabilities())};
	case AiBridgeRecordingOperation::GetStatus:
		return {2, request.requestId, true, QStringLiteral("ok"), QStringLiteral("Recording status read"),
			AiBridgeRecordingTypes::statusJson(m_manager->status())};
	case AiBridgeRecordingOperation::Prepare:
		return reply(m_manager->prepare(parsed.request.prepare));
	case AiBridgeRecordingOperation::Start:
		return reply(m_manager->start(parsed.request.start));
	case AiBridgeRecordingOperation::Stop:
		return reply(m_manager->stop(parsed.request.takeId));
	case AiBridgeRecordingOperation::List:
	{
		const auto page = m_manager->takes(parsed.request.page);
		if (page.code != QStringLiteral("ok"))
			return {2, request.requestId, false, page.code, QStringLiteral("Take page was refused")};
		return {2, request.requestId, true, QStringLiteral("ok"), QStringLiteral("Recorded takes listed"),
			AiBridgeRecordingTypes::takePageJson(page, parsed.request.page)};
	}
	}
	return {2, request.requestId, false, QStringLiteral("unknown_action"), QStringLiteral("Action is not a recording operation")};
}
}
