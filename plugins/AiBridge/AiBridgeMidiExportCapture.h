#ifndef LMMS_AIBRIDGE_MIDI_EXPORT_CAPTURE_H
#define LMMS_AIBRIDGE_MIDI_EXPORT_CAPTURE_H

#include "AiBridgeProjectContext.h"
#include "MidiExportService.h"

namespace lmms
{

struct MidiExportCaptureResult
{
	QString code;
	MidiExportSnapshot snapshot;
};

MidiExportCaptureResult captureMidiExport(const MidiExportSelection& selection, AiBridgeProjectContext& identities);

} // namespace lmms

#endif // LMMS_AIBRIDGE_MIDI_EXPORT_CAPTURE_H
