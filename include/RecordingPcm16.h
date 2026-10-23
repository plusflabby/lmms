#ifndef LMMS_RECORDING_PCM16_H
#define LMMS_RECORDING_PCM16_H
#include <cstddef>
#include <cstdint>
#include "lmms_export.h"
namespace lmms::recording {
struct ConvertStats {bool valid=false;std::size_t convertedFrames=0;std::uint64_t clipped=0,invalid=0;};
class LMMS_EXPORT RecordingPcm16
{
public:
 // Converts [-1,+1] float mono to signed PCM16; outside values saturate and count as clipped.
 // Ties round away from zero. Any NaN/Inf rejects the whole block without touching dest.
 static ConvertStats convert(const float* src,std::int16_t* dest,std::size_t frames) noexcept;
 static bool encodeLittleEndian(const std::int16_t* src,std::uint8_t* dest,std::size_t samples) noexcept;
};
}
#endif
