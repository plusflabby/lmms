#ifndef LMMS_AIBRIDGE_PROJECT_ACTIONS_H
#define LMMS_AIBRIDGE_PROJECT_ACTIONS_H
#include "AiBridgeProjectContext.h"
#include "AiBridgeMutationTypes.h"
#include "AiBridgeActions.h"
namespace lmms {
struct AiBridgeProjectActionResult { bool success=false; QString code; QString message; AiBridgeProjectValue project; QVector<AiBridgeTrackValue> tracks; QVector<AiBridgeClipValue> clips; AiBridgeClipValue clip; AiBridgeContextValue context; AiBridgePage page; AiBridgeMutationResult mutation; };
class AiBridgeProjectActions {
public:
 static AiBridgeProjectActionResult getProjectStructure();
 static AiBridgeProjectActionResult listTracks(int offset,int limit);
 static AiBridgeProjectActionResult listClips(const QString& trackId,int offset,int limit);
 static AiBridgeProjectActionResult getClip(const QString& clipId,int offset,int limit);
 static AiBridgeProjectActionResult openClipInPianoRoll(const QString& clipId);
 static AiBridgeProjectActionResult addNotes(const QString& clipId, const QVector<AiBridgeCopiedNote>& notes, const QString& expectedContentHash = {});
 static AiBridgeProjectActionResult deleteNotes(const QString& clipId, const AiBridgeNoteSelection& selection, const QString& expectedContentHash = {});
 static AiBridgeProjectActionResult transposeNotes(const QString& clipId, const AiBridgeNoteSelection& selection, int semitones, const QString& expectedContentHash = {});
 static AiBridgeProjectActionResult moveNotes(const QString& clipId, const AiBridgeNoteSelection& selection, qint64 deltaTicks, const QString& expectedContentHash = {});
 static AiBridgeProjectActionResult duplicateRegion(const QString& clipId, qint64 sourceStart, qint64 sourceEnd, qint64 destinationStart, const QString& expectedContentHash = {});
};
}
#endif
