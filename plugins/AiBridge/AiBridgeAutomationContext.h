#ifndef LMMS_AIBRIDGE_AUTOMATION_CONTEXT_H
#define LMMS_AIBRIDGE_AUTOMATION_CONTEXT_H

#include "AiBridgeAutomationRegistry.h"
#include "AiBridgeAutomationTargets.h"

namespace lmms
{

class AiBridgeAutomationContext
{
public:
	static constexpr bool nativeMarkersSupported = false;
	AiBridgeAutomationContext(AiBridgeAutomationRegistry &registry = aiBridgeAutomationRegistry(),
		AiBridgeAutomationTargets &targets = aiBridgeAutomationTargets());

	QVector<AiBridgeAutomationTargetSnapshot> listTargets(int offset, int limit) const;
	QVector<AiBridgeAutomationTargetSnapshot> capabilities(int offset, int limit) const;
	AiBridgeAutomationTrackPage listTracks(int offset, int limit) const;
	AiBridgeAutomationClipPage listClips(int offset, int limit) const;
	bool getClip(const QString &clipId, int offset, int limit,
		AiBridgeAutomationClipSnapshot &snapshot, AiBridgeAutomationPointPage &points) const;

private:
	AiBridgeAutomationRegistry &m_registry;
	AiBridgeAutomationTargets &m_targets;
};

} // namespace lmms

#endif
