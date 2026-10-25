#ifndef LMMS_AIBRIDGE_MIDI_EXPORT_WIRE_H
#define LMMS_AIBRIDGE_MIDI_EXPORT_WIRE_H

#include "AiBridgeMidiExportJobs.h"
#include "AiBridgeProtocol.h"
#include <QPointer>

namespace lmms
{

class AiBridgeMidiExportWire
{
public:
	static bool handles(const QString& action);
	static AiBridgeProtocolResponse execute(const AiBridgeProtocolRequest& request,QPointer<AiBridgeMidiExportJobs> jobs);
};

} // namespace lmms

#endif // LMMS_AIBRIDGE_MIDI_EXPORT_WIRE_H
