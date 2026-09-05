#ifndef LMMS_AIBRIDGE_EFFECT_REGISTRY_H
#define LMMS_AIBRIDGE_EFFECT_REGISTRY_H
#include <QHash>
#include <QPointer>
#include <QString>
class QObject;
namespace lmms {
class AiBridgeEffectRegistry {
public:
	AiBridgeEffectRegistry(QString sessionId, int generation);
	QString effectId(QObject* lifetimeToken, const QString& channelId);
	QObject* resolve(const QString& id) const;
	class Effect* resolveEffect(const QString& id) const;
	void retire(const QString& id);
	void retireProject();
private:
	struct Record { QString id; QPointer<QObject> object; bool retired = false; };
	QString m_sessionId; int m_generation; quint64 m_nextId = 1; QHash<QString, Record> m_records;
};
AiBridgeEffectRegistry &aiBridgeEffectRegistry();
}
#endif
