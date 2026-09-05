#include "AiBridgeMixerRegistry.h"
#include "Mixer.h"

#include <QObject>
#include <QRandomGenerator>

namespace lmms
{

AiBridgeMixerRegistry &aiBridgeMixerRegistry()
{
	static AiBridgeMixerRegistry value(QStringLiteral("%1").arg(QRandomGenerator::global()->generate64(), 0, 16), 1);
	return value;
}

AiBridgeMixerRegistry::AiBridgeMixerRegistry(QString sessionId, int projectGeneration) :
	m_sessionId(std::move(sessionId)),
	m_projectGeneration(projectGeneration),
	m_masterId(QStringLiteral("mixer-channel:master:%1:%2").arg(m_sessionId).arg(m_projectGeneration))
{
}

QString AiBridgeMixerRegistry::masterId() const
{
	return m_masterId;
}

QString AiBridgeMixerRegistry::channelId(QObject* lifetimeToken, bool master)
{
	if (master)
	{
		return m_masterId;
	}
	if (lifetimeToken == nullptr)
	{
		return {};
	}
	for (auto it = m_records.cbegin(); it != m_records.cend(); ++it)
	{
		if (!it->retired && it->lifetimeToken == lifetimeToken)
		{
			return it->id;
		}
	}
	const auto id = QStringLiteral("mixer-channel:%1:%2:%3")
		.arg(m_sessionId).arg(m_projectGeneration).arg(m_nextToken++);
	m_records.insert(id, Record{id, QPointer<QObject>(lifetimeToken), false});
	return id;
}

QString AiBridgeMixerRegistry::routeId(MixerRoute* route)
{
	if (!route) return {};
	for (auto it = m_routes.cbegin(); it != m_routes.cend(); ++it)
	{
		if (it.value() == route) return it.key();
	}
	const auto id = QStringLiteral("mixer-route:%1:%2:%3").arg(m_sessionId).arg(m_projectGeneration).arg(m_nextRouteToken++);
	m_routes.insert(id, QPointer<MixerRoute>(route));
	QObject::connect(route, &QObject::destroyed, [this, id]() { m_routes.remove(id); });
	return id;
}

MixerRoute* AiBridgeMixerRegistry::liveRoute(const QString& id) const
{
	const auto it = m_routes.constFind(id);
	return it == m_routes.cend() ? nullptr : it.value().data();
}

bool AiBridgeMixerRegistry::isLive(const QString& id) const
{
	if (id == m_masterId)
	{
		return true;
	}
	const auto it = m_records.constFind(id);
	return it != m_records.cend() && !it->retired && !it->lifetimeToken.isNull();
}

bool AiBridgeMixerRegistry::isRetired(const QString& id) const
{
	if (id == m_masterId)
	{
		return false;
	}
	const auto it = m_records.constFind(id);
	return it != m_records.cend() && (it->retired || it->lifetimeToken.isNull());
}

void AiBridgeMixerRegistry::retireChannel(const QString& id)
{
	if (auto it = m_records.find(id); it != m_records.end())
	{
		it->retired = true;
	}
}

void AiBridgeMixerRegistry::retireProject()
{
	for (auto& record : m_records)
	{
		record.retired = true;
	}
}

void AiBridgeMixerRegistry::retireSession()
{
	retireProject();
	m_routes.clear();
}

void AiBridgeMixerRegistry::retireRoute(const QString& id)
{
	m_routes.remove(id);
}

} // namespace lmms
