#ifndef LMMS_RECORDING_SERVICE_CLOCK_H
#define LMMS_RECORDING_SERVICE_CLOCK_H
#include <QElapsedTimer>
#include <cstdint>
namespace lmms::recording {
inline std::uint64_t recordingServiceMilliseconds(const QElapsedTimer& timer) noexcept
{
 // elapsed() has a different zero for every timer. Both GUI callers must use
 // Qt's shared monotonic reference domain for the input callback watchdog.
 return std::uint64_t(timer.msecsSinceReference()+timer.elapsed());
}
}
#endif
