#ifndef LMMS_AIBRIDGE_AUTOMATION_RECONCILER_H
#define LMMS_AIBRIDGE_AUTOMATION_RECONCILER_H

#include "AiBridgeAutomationRegistry.h"
#include "AiBridgeAutomationTargets.h"
#include "AiBridgeMixerRegistry.h"
#include "AiBridgeEffectRegistry.h"

namespace lmms
{
class AiBridgeProjectContext;

class AiBridgeAutomationReconciler
{
public:
	AiBridgeAutomationReconciler(AiBridgeProjectContext &project,
		AiBridgeAutomationRegistry &registry, AiBridgeAutomationTargets &targets,
		AiBridgeMixerRegistry &mixerRegistry = aiBridgeMixerRegistry(),
		AiBridgeEffectRegistry &effectRegistry = aiBridgeEffectRegistry());
	void reconcile();

private:
	AiBridgeProjectContext &m_project;
	AiBridgeAutomationRegistry &m_registry;
	AiBridgeAutomationTargets &m_targets;
	AiBridgeMixerRegistry &m_mixerRegistry;
	AiBridgeEffectRegistry &m_effectRegistry;
};
} // namespace lmms

#endif
