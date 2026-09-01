#ifndef LMMS_AIBRIDGE_PROJECT_CONTEXT_H
#define LMMS_AIBRIDGE_PROJECT_CONTEXT_H

#include <QHash>
#include <QPointer>
#include <QString>
#include <QVector>

namespace lmms
{
class Clip; class Song; class Track;

struct AiBridgeNoteValue { int position=0; int length=0; int key=0; int volume=0; int panning=0; int sourceOrder=0; QString noteType; };
struct AiBridgeTrackValue { QString id, name, type; int index=0; int clipCount=0; int volume=0; bool hasVolume=false; };
struct AiBridgeClipValue { QString id, trackId, name, type, contentHash; int startTicks=0; int lengthTicks=0; int noteCount=0; bool autoResize=false; bool currentInPianoRoll=false; QVector<AiBridgeNoteValue> notes; };
struct AiBridgePage { int total=0; int offset=0; int limit=0; bool hasMore=false; };
struct AiBridgeProjectValue { QString songName; int tempo=0; int lengthBars=0; int lengthTicks=0; int trackCount=0; bool hasLengthTicks=false; QVector<AiBridgeTrackValue> tracks; };
struct AiBridgeContextValue { bool success=false; QString code, message, songName, clipId, trackId, trackName; quint64 projectGeneration=0; int tempo=0; int trackVolume=0; int clipLengthTicks=0; bool hasCurrentMidiClip=false; bool hasOwningInstrumentTrack=false; };

class AiBridgeProjectContext
{
public:
 static constexpr int MaxPageSize=256;
 static constexpr int StructureTrackCap=16;
 AiBridgeProjectContext();
 quint64 projectGeneration() const;
 AiBridgeProjectValue projectStructure() const;
 QVector<AiBridgeTrackValue> tracks(AiBridgePage&) const;
 QVector<AiBridgeClipValue> clips(const QString&, AiBridgePage&) const;
 AiBridgeClipValue clip(const QString&, AiBridgePage&) const;
 AiBridgeContextValue currentContext() const;
 AiBridgeClipValue openableClip(const QString&, QString&, QString&) const;
 Track* resolveTrack(const QString&) const;
 Clip* resolveClip(const QString&) const;
private:
 void ensureLifecycleHook() const;
 void invalidate() const;
 void reconcile() const;
 QString trackId(Track*) const;
 QString clipId(Clip*) const;
 static QString typeName(Track*);
 struct TrackEntry { QPointer<Track> object; quint64 generation=0; };
 struct ClipEntry { QPointer<Clip> object; QString trackId; quint64 generation=0; };
 static QPointer<Song> s_hookedSong;
 static QHash<QString, TrackEntry> s_tracks;
 static QHash<QString, ClipEntry> s_clips;
 static QString s_session;
 static quint64 s_generation;
 static quint64 s_nextId;
 static bool s_hookInstalled;
};
}
#endif
