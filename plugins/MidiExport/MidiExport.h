#ifndef LMMS_MIDI_EXPORT_H
#define LMMS_MIDI_EXPORT_H

#include "ExportFilter.h"

namespace lmms
{

class MidiExport final : public ExportFilter
{
public:
	MidiExport();
	~MidiExport() override = default;
	gui::PluginView* instantiateView(QWidget*) override { return nullptr; }
	bool tryExport(const TrackContainer::TrackList& tracks,
		const TrackContainer::TrackList& patternTracks,int tempo,int masterPitch,const QString& filename) override;
};

} // namespace lmms

#endif // LMMS_MIDI_EXPORT_H
