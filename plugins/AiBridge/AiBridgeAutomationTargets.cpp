#include "AiBridgeAutomationTargets.h"

#include <QRandomGenerator>
#include <QObject>

namespace lmms
{

namespace
{
QString keyFor(const QString &ownerId, const QString &semanticClass, const QString &parameterId)
{
	return ownerId + QChar(0) + semanticClass + QChar(0) + parameterId;
}
}

AiBridgeAutomationTargets &aiBridgeAutomationTargets()
{
	static AiBridgeAutomationTargets value(
		QStringLiteral("%1").arg(QRandomGenerator::global()->generate64(), 0, 16), 1);
	return value;
}

AiBridgeAutomationTargets::AiBridgeAutomationTargets(QString sessionId, int projectGeneration) :
	m_sessionId(std::move(sessionId)),
	m_projectGeneration(projectGeneration)
{
}

QString AiBridgeAutomationTargets::registerTarget(const QString &ownerId,
	const QString &semanticClass, const QString &parameterId,
	const AiBridgeAutomationTargetSnapshot &metadata, QObject *binding)
{
	if (ownerId.isEmpty() || semanticClass.isEmpty() || parameterId.isEmpty()
		|| metadata.capability == QStringLiteral("unsupported"))
	{
		return {};
	}
	const auto key = keyFor(ownerId, semanticClass, parameterId);
	if (const auto existing = m_keyToId.constFind(key); existing != m_keyToId.cend())
	{
		return existing.value();
	}
	const auto id = QStringLiteral("automation-target:%1:%2:%3")
		.arg(m_sessionId).arg(m_projectGeneration).arg(m_nextId++);
	auto copy = metadata;
	copy.automationTargetId = id;
	copy.ownerId = ownerId;
	copy.semanticClass = semanticClass;
	copy.parameterId = parameterId;
	m_keyToId.insert(key, id);
	m_targets.insert(id, copy);
	if (binding) m_bindings.insert(id, binding);
	return id;
}

AiBridgeAutomationTargetSnapshot AiBridgeAutomationTargets::registerUnsupported(
	const QString &semanticClass, const QString &reason)
{
	AiBridgeAutomationTargetSnapshot result;
	result.semanticClass = semanticClass;
	result.capability = QStringLiteral("unsupported");
	result.unit = reason;
	return result;
}

bool AiBridgeAutomationTargets::resolve(const QString &id,
	AiBridgeAutomationTargetSnapshot &target) const
{
	const auto it = m_targets.constFind(id);
	if (it == m_targets.cend()) return false;
	target = it.value();
	return true;
}

QVector<AiBridgeAutomationTargetSnapshot> AiBridgeAutomationTargets::targetsForOwner(
	const QString &ownerId) const
{
	QVector<AiBridgeAutomationTargetSnapshot> result;
	for (const auto &target : m_targets)
	{
		if (target.ownerId == ownerId) result.append(target);
	}
	return result;
}

QVector<AiBridgeAutomationTargetSnapshot> AiBridgeAutomationTargets::all() const
{
	return m_targets.values().toVector();
}

bool AiBridgeAutomationTargets::isSupported(const QString &id) const
{
	const auto it = m_targets.constFind(id);
	return it != m_targets.cend() && it->capability != QStringLiteral("unsupported");
}

QObject *AiBridgeAutomationTargets::bindingFor(const QString &id) const
{
	return m_bindings.value(id).data();
}

void AiBridgeAutomationTargets::retireOwner(const QString &ownerId)
{
	for (auto it = m_targets.begin(); it != m_targets.end();)
	{
		if (it->ownerId == ownerId) it = m_targets.erase(it);
		else ++it;
	}
	for (auto it = m_keyToId.begin(); it != m_keyToId.end();)
	{
		if (it.key().startsWith(ownerId + QChar(0))) it = m_keyToId.erase(it);
		else ++it;
	}
}

void AiBridgeAutomationTargets::retireProject()
{
	m_targets.clear();
	m_keyToId.clear();
	m_bindings.clear();
}

void AiBridgeAutomationTargets::retireSession()
{
	retireProject();
}

} // namespace lmms
