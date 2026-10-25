#ifndef LMMS_RECORDING_TAKE_JOURNAL_H
#define LMMS_RECORDING_TAKE_JOURNAL_H
#include "RecordingTakeTypes.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
namespace lmms::recording {
inline constexpr std::size_t TakeJournalRecordMaximum=256;
struct EncodedTakeJournalRecord
{
 std::array<std::uint8_t,TakeJournalRecordMaximum> bytes{};
 std::uint16_t size=0;
 bool valid=false;
};
class RecordingTakeJournalCodec
{
public:
 static QByteArray encodeHeader(const TakeDescriptor&);
 static bool validateHeader(const QByteArray&,TakeDescriptor&);
 static EncodedTakeJournalRecord encodeProgress(const TakeProgress&) noexcept;
 static EncodedTakeJournalRecord encodeSealIntent(const TakeProgress&) noexcept;
 static EncodedTakeJournalRecord encodeTerminal(TakeTerminal,const TakeProgress&) noexcept;
 static bool validateRecord(const std::uint8_t*,std::size_t,TakeJournalCursor&) noexcept;
};
}
#endif
