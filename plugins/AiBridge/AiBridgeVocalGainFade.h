#ifndef LMMS_AI_BRIDGE_VOCAL_GAIN_FADE_H
#define LMMS_AI_BRIDGE_VOCAL_GAIN_FADE_H

#include "AiBridgeVocalEditTypes.h"
#include "SampleFrame.h"
#include <span>
#include <vector>

namespace lmms {

// Pure block processor for the explicitly declared B2 gain/fade operations.
// Frame coordinates are absolute in the render-input domain. It allocates no
// memory and never clips, normalizes, or changes the block's duration.
class AiBridgeVocalGainFade
{
public:
	static QString process(
		const VocalEditFrameRange& renderInputFrames,
		const std::vector<VocalEditOperation>& operations,
		qint64 firstRenderInputFrame,
		std::span<const SampleFrame> input,
		int channels,
		std::span<float> interleavedOutput);
};

} // namespace lmms

#endif
