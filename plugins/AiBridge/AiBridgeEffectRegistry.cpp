#include "AiBridgeEffectRegistry.h"
#include "Effect.h"
#include <QRandomGenerator>
namespace lmms {
AiBridgeEffectRegistry &aiBridgeEffectRegistry() { static AiBridgeEffectRegistry value(QStringLiteral("%1").arg(QRandomGenerator::global()->generate64(), 0, 16), 1); return value; }
AiBridgeEffectRegistry::AiBridgeEffectRegistry(QString sessionId, int generation) : m_sessionId(std::move(sessionId)), m_generation(generation) {}
QString AiBridgeEffectRegistry::effectId(QObject* object, const QString& channelId)
{
	if (!object || channelId.isEmpty()) return {};
	for (auto it=m_records.cbegin(); it!=m_records.cend(); ++it) if (!it->retired && it->object == object) return it->id;
	const auto id=QStringLiteral("mixer-effect:%1:%2:%3").arg(m_sessionId).arg(m_generation).arg(m_nextId++);
	m_records.insert(id, {id, QPointer<QObject>(object), false}); return id;
}
QObject* AiBridgeEffectRegistry::resolve(const QString& id) const { const auto it=m_records.constFind(id); return it==m_records.cend() || it->retired ? nullptr : it->object.data(); }
Effect* AiBridgeEffectRegistry::resolveEffect(const QString& id) const { return dynamic_cast<Effect*>(resolve(id)); }
void AiBridgeEffectRegistry::retire(const QString& id) { if (auto it=m_records.find(id); it!=m_records.end()) it->retired=true; }
void AiBridgeEffectRegistry::retireProject() { for (auto& record:m_records) record.retired=true; }
}
