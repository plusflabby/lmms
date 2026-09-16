#include "AiBridgeAutomationRegistry.h"

#include <QObject>
#include <QRandomGenerator>

namespace lmms
{

AiBridgeAutomationRegistry &aiBridgeAutomationRegistry()
{
	static AiBridgeAutomationRegistry value(
		QStringLiteral("%1").arg(QRandomGenerator::global()->generate64(), 0, 16), 1);
	return value;
}

AiBridgeAutomationRegistry::AiBridgeAutomationRegistry(QString sessionId, int projectGeneration) :
	m_sessionId(std::move(sessionId)),
	m_projectGeneration(projectGeneration)
{
}

QString AiBridgeAutomationRegistry::automationTrackId(QObject *trackToken)
{
	if (!trackToken) return {};
	for (auto it = m_records.cbegin(); it != m_records.cend(); ++it)
	{
		if (it->track && !it->retired && it->token == trackToken)
		{
			return it->id;
		}
	}
	const auto id = QStringLiteral("automation-track:%1:%2:%3")
		.arg(m_sessionId).arg(m_projectGeneration).arg(m_nextTrackToken++);
	m_records.insert(id, Record{id, QPointer<QObject>(trackToken), {}, true, false});
	return id;
}

QString AiBridgeAutomationRegistry::automationClipId(QObject *clipToken, const QString &trackId)
{
	if (!clipToken || trackId.isEmpty()) return {};
	const auto track = m_records.constFind(trackId);
	if (track == m_records.cend() || !track->track || track->retired || track->token.isNull()) return {};
	for (auto it = m_records.cbegin(); it != m_records.cend(); ++it)
	{
		if (!it->track && !it->retired && it->token == clipToken)
		{
			return it->id;
		}
	}
	const auto id = QStringLiteral("automation-clip:%1:%2:%3")
		.arg(m_sessionId).arg(m_projectGeneration).arg(m_nextClipToken++);
	m_records.insert(id, Record{id, QPointer<QObject>(clipToken), trackId, false, false});
	return id;
}

QObject *AiBridgeAutomationRegistry::resolveTrackToken(const QString &id) const
{
	const auto it = m_records.constFind(id);
	return it != m_records.cend() && it->track && !it->retired ? it->token.data() : nullptr;
}

QObject *AiBridgeAutomationRegistry::resolveClipToken(const QString &id) const
{
	const auto it = m_records.constFind(id);
	return it != m_records.cend() && !it->track && !it->retired ? it->token.data() : nullptr;
}

bool AiBridgeAutomationRegistry::isLive(const QString &id) const
{
	const auto it = m_records.constFind(id);
	return it != m_records.cend() && !it->retired && !it->token.isNull();
}

bool AiBridgeAutomationRegistry::isRetired(const QString &id) const
{
	const auto it = m_records.constFind(id);
	return it != m_records.cend() && (it->retired || it->token.isNull());
}

void AiBridgeAutomationRegistry::retireTrack(const QString &id)
{
	if (auto it = m_records.find(id); it != m_records.end() && it->track)
	{
		it->retired = true;
		for (auto &record : m_records)
		{
			if (!record.track && record.ownerTrackId == id) record.retired = true;
		}
	}
}

void AiBridgeAutomationRegistry::retireClip(const QString &id)
{
	if (auto it = m_records.find(id); it != m_records.end() && !it->track)
	{
		it->retired = true;
	}
}

void AiBridgeAutomationRegistry::retireProject()
{
	for (auto &record : m_records) record.retired = true;
}

void AiBridgeAutomationRegistry::retireSession()
{
	retireProject();
}

} // namespace lmms
