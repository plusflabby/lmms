#include "RecordingPcm16.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace lmms::recording {
ConvertStats RecordingPcm16::convert(const float* source,std::int16_t* dest,std::size_t frames) noexcept
{
 ConvertStats stats;
 if(!source || !dest || !frames)return stats;
 for(std::size_t i=0;i<frames;++i)if(!std::isfinite(source[i])){stats.invalid=1;return stats;}
 for(std::size_t i=0;i<frames;++i)
 {
  const auto value=source[i];if(value < -1.f || value > 1.f)++stats.clipped;
  const auto scaled=std::round(double(value)*32768.0);
  const auto bounded=std::clamp(scaled,-32768.0,32767.0);
  dest[i]=static_cast<std::int16_t>(bounded);
 }
 stats.valid=true;stats.convertedFrames=frames;return stats;
}
bool RecordingPcm16::encodeLittleEndian(const std::int16_t* src,std::uint8_t* dest,std::size_t samples) noexcept
{
 if(!src || !dest || !samples || samples>std::numeric_limits<std::size_t>::max()/2)return false;
 for(std::size_t i=0;i<samples;++i){const auto word=static_cast<std::uint16_t>(src[i]);dest[2*i]=std::uint8_t(word&0xff);dest[2*i+1]=std::uint8_t(word>>8);}
 return true;
}
}
