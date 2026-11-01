#ifndef LMMS_MIDI_EXPORT_OUTPUT_H
#define LMMS_MIDI_EXPORT_OUTPUT_H

#include "StandardMidiWriter.h"
#include <atomic>
#include <functional>
#include "lmms_export.h"

namespace lmms
{

struct MidiExportFileResult
{
	bool success = false;
	QString code;
	QString destinationPath;
	QString sha256;
	qint64 bytesWritten = 0;
};

LMMS_EXPORT MidiExportFileResult writeMidiExportFile(const QString& authorizedRoot,const QString& filename,
	bool overwrite,const MidiExportPlan& plan,const std::atomic_bool* cancelled=nullptr,
	const std::function<bool()>& acquirePublishPermit={});

} // namespace lmms

#endif // LMMS_MIDI_EXPORT_OUTPUT_H
