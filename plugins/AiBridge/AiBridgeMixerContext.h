#ifndef LMMS_AIBRIDGE_MIXER_CONTEXT_H
#define LMMS_AIBRIDGE_MIXER_CONTEXT_H

#include "AiBridgeMixerRegistry.h"
#include "AiBridgeMixerTypes.h"

namespace lmms
{

class AiBridgeMixerContext
{
public:
	explicit AiBridgeMixerContext(AiBridgeMixerRegistry& registry);
	AiBridgeMixerStructureSnapshot snapshot(int offset, int limit);
	class MixerChannel* liveChannel(const QString& id);

private:
	AiBridgeMixerRegistry& m_registry;
};

} // namespace lmms

#endif
