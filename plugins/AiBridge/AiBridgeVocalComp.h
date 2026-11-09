#ifndef LMMS_AI_BRIDGE_VOCAL_COMP_H
#define LMMS_AI_BRIDGE_VOCAL_COMP_H

#include "AiBridgeVocalEditTypes.h"
#include "SampleFrame.h"
#include <span>

namespace lmms {

struct VocalCompSourceView
{
	qint64 firstSourceFrame = 0;
	std::span<const SampleFrame> frames;
};

// Pure bounded assembler. The caller supplies only precharged selected ranges;
// output blocks are mapped to the explicit comp decision list without allocating.
class AiBridgeVocalComp
{
public:
	static QString process(
		const VocalCompRecipe& recipe,
		std::span<const VocalCompSourceView> sources,
		qint64 firstOutputFrame,
		std::span<SampleFrame> output);
};

} // namespace lmms

#endif
