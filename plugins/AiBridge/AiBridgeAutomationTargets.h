#ifndef LMMS_AIBRIDGE_AUTOMATION_TARGETS_H
#define LMMS_AIBRIDGE_AUTOMATION_TARGETS_H

#include "AiBridgeAutomationTypes.h"

#include <QHash>
#include <QPointer>

class QObject;

namespace lmms
{

class AiBridgeAutomationTargets
{
public:
	AiBridgeAutomationTargets(QString sessionId, int projectGeneration);

	QString registerTarget(const QString &ownerId, const QString &semanticClass,
		const QString &parameterId, const AiBridgeAutomationTargetSnapshot &metadata,
		QObject *binding = nullptr);
	AiBridgeAutomationTargetSnapshot registerUnsupported(const QString &semanticClass,
		const QString &reason);
	bool resolve(const QString &id, AiBridgeAutomationTargetSnapshot &target) const;
	QVector<AiBridgeAutomationTargetSnapshot> targetsForOwner(const QString &ownerId) const;
	QVector<AiBridgeAutomationTargetSnapshot> all() const;
	bool isSupported(const QString &id) const;
	QObject *bindingFor(const QString &id) const;
	void retireOwner(const QString &ownerId);
	void retireProject();
	void retireSession();

private:
	QString m_sessionId;
	int m_projectGeneration;
	quint64 m_nextId = 1;
	QHash<QString, AiBridgeAutomationTargetSnapshot> m_targets;
	QHash<QString, QString> m_keyToId;
	QHash<QString, QPointer<QObject>> m_bindings;
};

AiBridgeAutomationTargets &aiBridgeAutomationTargets();

} // namespace lmms

#endif
