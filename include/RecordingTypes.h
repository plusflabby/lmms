#ifndef LMMS_RECORDING_TYPES_H
#define LMMS_RECORDING_TYPES_H
#include <QString>
#include "lmms_export.h"
namespace lmms::recording {
enum class Phase { Disarmed, Armed, Prepared, Priming, Preroll, Recording, Quiescing, Finalizing };
enum class TakeOutcome { None, Complete, SavedUnattached, Incomplete, NoTakeCreated };
struct LocalArmContext
{
 QString processId,projectId,deviceId,libraryRootId;
 quint64 projectGeneration=0;
 bool operator==(const LocalArmContext&) const=default;
};
struct PreparedTake
{
 QString preparedId;
 LocalArmContext context;
 qint64 startTick=0;
 int countInBars=0;
 bool metronome=false;
 QString label,transportHash;
 unsigned sampleRate=0;
};
struct Status
{
 Phase phase=Phase::Disarmed;
 bool armed=false,incomplete=false;
 QString preparedId,takeId,startRequestId;
 quint64 projectGeneration=0;
 TakeOutcome outcome=TakeOutcome::None;
};
struct Result {bool ok=false;QString code;Status status;};
}
#endif
