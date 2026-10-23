#include "AiBridgeProjectContext.h"
#include "AiBridgeMutationTypes.h"
#include <algorithm>
#include <tuple>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QUuid>
#include <QThread>
#include "Clip.h"
#include "Engine.h"
#include "GuiApplication.h"
#include "MainWindow.h"
#include "InstrumentTrack.h"
#include "MidiClip.h"
#include "Note.h"
#include "PluginFactory.h"
#include "Plugin.h"
#include "PianoRoll.h"
#include "Song.h"
#include "TimePos.h"
#include "Track.h"
namespace lmms {
QPointer<Song> AiBridgeProjectContext::s_hookedSong;
QHash<QString,AiBridgeProjectContext::TrackEntry> AiBridgeProjectContext::s_tracks;
QHash<QString,AiBridgeProjectContext::ClipEntry> AiBridgeProjectContext::s_clips;
QString AiBridgeProjectContext::s_session;
QString AiBridgeProjectContext::s_identitySession;
quint64 AiBridgeProjectContext::s_generation=1;
quint64 AiBridgeProjectContext::s_identityRevision=1;
quint64 AiBridgeProjectContext::s_nextId=1;
bool AiBridgeProjectContext::s_hookInstalled=false;
namespace {
bool qt() { return qApp && QThread::currentThread()==qApp->thread(); }
int cap(int value) { return qBound(1,value,AiBridgeProjectContext::MaxPageSize); }
QString makeId(const QString& prefix,const QString& session,quint64 generation,quint64 number) { return QStringLiteral("%1:%2:%3:%4").arg(prefix,session).arg(generation).arg(number); }
AiBridgeMutationState mutationState(const MidiClip *midi) {
	AiBridgeMutationState state;
	state.clip_length_ticks = midi->length().getTicks();
	state.auto_resize = midi->getAutoResize();
	for (const auto *note : midi->notes()) {
		AiBridgeCopiedNote copied;
		copied.position_ticks = note->pos().getTicks();
		copied.length_ticks = note->length().getTicks();
		copied.key = note->key();
		copied.volume = note->getVolume();
		copied.panning = note->getPanning();
		copied.note_type = note->type() == Note::Type::Step ? QStringLiteral("step") : QStringLiteral("regular");
		state.notes.append(copied);
	}
	return state;
}
}
AiBridgeProjectContext::AiBridgeProjectContext() { if(s_session.isEmpty()) s_session=QString::number(QRandomGenerator::global()->generate64(),16); if(s_identitySession.isEmpty()) s_identitySession=QUuid::createUuid().toString(QUuid::WithoutBraces); ensureLifecycleHook(); }
void AiBridgeProjectContext::ensureLifecycleHook() const {
 if(!qt()) return;
 auto* song=Engine::getSong();
 if(!song || (s_hookInstalled && s_hookedSong==song)) return;
 s_hookedSong=song; s_hookInstalled=true;
	QObject::connect(song,&Song::projectAboutToChange,song,[] { AiBridgeProjectContext context; context.invalidate(); ++s_identityRevision; },Qt::DirectConnection);
	QObject::connect(song,&Song::projectLoaded,song,[] { ++s_identityRevision; },Qt::DirectConnection);
	QObject::connect(song,&Song::modified,song,[] { ++s_identityRevision; },Qt::DirectConnection);
	QObject::connect(song,&Song::projectFileNameChanged,song,[] { ++s_identityRevision; },Qt::DirectConnection);
}
void AiBridgeProjectContext::invalidate() const { if(!qt()) return; for(auto& e:s_tracks)e.generation=0; for(auto& e:s_clips)e.generation=0; ++s_generation; }
quint64 AiBridgeProjectContext::projectGeneration() const { return s_generation; }
QJsonObject AiBridgeProjectContext::projectIdentity() const {
	QJsonObject loaded;
	auto *song = Engine::getSong();
	QString state = QStringLiteral("unknown");
	bool dirty = false;
	if (qt() && song) {
		dirty = song->isModified();
		if (gui::getGUI() && gui::getGUI()->mainWindow() && gui::getGUI()->mainWindow()->getSession() == gui::MainWindow::SessionState::Recover) state = QStringLiteral("recovered");
		else if (!song->projectFileName().isEmpty()) state = dirty ? QStringLiteral("modified") : QStringLiteral("saved");
		else state = QStringLiteral("unsaved");
		if (!song->projectFileName().isEmpty()) {
			const QFileInfo file(song->projectFileName());
			const auto canonical = file.canonicalFilePath();
			if (!canonical.isEmpty()) loaded.insert(QStringLiteral("canonical_path"), QDir::cleanPath(canonical));
			if (!song->projectSourceSha256().isEmpty()) loaded.insert(QStringLiteral("sha256"), QString::fromLatin1(song->projectSourceSha256()));
			if (!song->projectSourceFileId().isEmpty()) loaded.insert(QStringLiteral("windows_file_id"), song->projectSourceFileId());
		}
	}
	return {{QStringLiteral("schema_version"), 1}, {QStringLiteral("availability"), QStringLiteral("unavailable")},
		{QStringLiteral("process_session_id"), s_identitySession},
		{QStringLiteral("project_load_generation"), QString::number(s_generation)},
		{QStringLiteral("identity_revision"), QString::number(s_identityRevision)},
		{QStringLiteral("document_state"), state}, {QStringLiteral("dirty"), dirty},
		{QStringLiteral("loaded_project"), loaded}};
}
QString AiBridgeProjectContext::trackId(Track* track) const {
 for(auto i=s_tracks.cbegin();i!=s_tracks.cend();++i) if(i->object==track&&i->generation==s_generation)return i.key();
 const auto value=makeId(QStringLiteral("track"),s_session,s_generation,s_nextId++); s_tracks.insert(value,{QPointer<Track>(track),s_generation}); return value;
}
QString AiBridgeProjectContext::clipId(Clip* clip) const {
 for(auto i=s_clips.cbegin();i!=s_clips.cend();++i) if(i->object==clip&&i->generation==s_generation)return i.key();
 const auto value=makeId(QStringLiteral("clip"),s_session,s_generation,s_nextId++); s_clips.insert(value,{QPointer<Clip>(clip),{},s_generation}); return value;
}
QString AiBridgeProjectContext::typeName(Track* track) {
 if(!track)return QStringLiteral("unknown");
 switch(track->type()){case Track::Type::Instrument:return QStringLiteral("instrument");case Track::Type::Pattern:return QStringLiteral("pattern");case Track::Type::Sample:return QStringLiteral("sample");case Track::Type::Event:return QStringLiteral("event");case Track::Type::Video:return QStringLiteral("video");case Track::Type::Automation:return QStringLiteral("automation");case Track::Type::HiddenAutomation:return QStringLiteral("hidden_automation");case Track::Type::Count:break;} return QStringLiteral("unknown");
}
void AiBridgeProjectContext::reconcile() const {
 if(!qt())return;
 ensureLifecycleHook();
 auto* song=Engine::getSong();
 if(!song)return;
 for(auto* track:song->tracks()){const auto tid=trackId(track);for(auto* clip:track->getClips()){const auto cid=clipId(clip);s_clips[cid].trackId=tid;}}
}
Track* AiBridgeProjectContext::resolveTrack(const QString& value) const { reconcile(); auto i=s_tracks.constFind(value); return qt()&&i!=s_tracks.cend()&&i->generation==s_generation?i->object.data():nullptr; }
Clip* AiBridgeProjectContext::resolveClip(const QString& value) const { reconcile(); auto i=s_clips.constFind(value); return qt()&&i!=s_clips.cend()&&i->generation==s_generation?i->object.data():nullptr; }
QVector<AiBridgeTrackValue> AiBridgeProjectContext::tracks(AiBridgePage& page) const {
 QVector<AiBridgeTrackValue> result; if(!qt())return result; reconcile(); auto* song=Engine::getSong(); if(!song){page={};return result;}
 page.total=static_cast<int>(song->tracks().size()); page.limit=cap(page.limit); page.offset=qBound(0,page.offset,page.total); page.hasMore=page.offset+page.limit<page.total;
 for(int i=page.offset;i<qMin(page.total,page.offset+page.limit);++i){auto* track=song->tracks().at(i);AiBridgeTrackValue value{trackId(track),track->displayName(),typeName(track),i,track->numOfClips()};if(auto* instrument=dynamic_cast<InstrumentTrack*>(track)){value.hasVolume=true;value.volume=instrument->getVolume();}result.append(value);} return result;
}
AiBridgeTrackValue AiBridgeProjectContext::trackSnapshot(Track* track) const {
 AiBridgeTrackValue value; if(!qt() || !track) return value; const auto id=trackId(track); value.id=id; value.name=track->displayName(); value.type=typeName(track); const auto &tracks=track->trackContainer()->tracks(); const auto position=std::find(tracks.begin(),tracks.end(),track); value.index=position==tracks.end() ? -1 : static_cast<int>(std::distance(tracks.begin(),position)); value.clipCount=track->numOfClips(); if(auto* instrument=dynamic_cast<InstrumentTrack*>(track)){value.hasVolume=true;value.volume=instrument->getVolume();} return value;
}
QString AiBridgeProjectContext::idForClip(Clip* clip) const { return qt() && clip ? clipId(clip) : QString(); }
QString AiBridgeProjectContext::idForTrack(Track* track) const { return qt() && track ? trackId(track) : QString(); }
QVector<AiBridgeInstrumentValue> AiBridgeProjectContext::instruments(AiBridgePage& page) const {
 QVector<AiBridgeInstrumentValue> all; if(!qt() || !getPluginFactory()) { page={}; return all; }
 for(const auto *descriptor : getPluginFactory()->descriptors(Plugin::Type::Instrument)) {
  if(!descriptor || !descriptor->name || !descriptor->displayName || descriptor->subPluginFeatures) continue;
  AiBridgeInstrumentValue value; value.instrumentId=QString::fromUtf8(descriptor->name); value.displayName=QString::fromUtf8(descriptor->displayName); if(descriptor->description) value.description=QString::fromUtf8(descriptor->description); value.available=true; all.append(value);
 }
 std::sort(all.begin(),all.end(),[](const auto&a,const auto&b){return a.instrumentId<b.instrumentId;});
 all.erase(std::unique(all.begin(),all.end(),[](const auto&a,const auto&b){return a.instrumentId==b.instrumentId;}),all.end());
 page.total=all.size(); page.limit=cap(page.limit); page.offset=qBound(0,page.offset,page.total); page.hasMore=page.offset+page.limit<page.total;
 return all.mid(page.offset,page.limit);
}
QVector<AiBridgeClipValue> AiBridgeProjectContext::clips(const QString& trackValue,AiBridgePage& page) const {
 QVector<AiBridgeClipValue> result; if(!qt())return result; auto* track=resolveTrack(trackValue); if(!track){page={};return result;}
 page.total=track->numOfClips();page.limit=cap(page.limit);page.offset=qBound(0,page.offset,page.total);page.hasMore=page.offset+page.limit<page.total;
 for(int i=page.offset;i<qMin(page.total,page.offset+page.limit);++i){auto* clip=track->getClip(i);AiBridgeClipValue value;value.id=clipId(clip);value.trackId=trackValue;value.name=clip->displayName();value.startTicks=clip->startPosition().getTicks();value.lengthTicks=clip->length().getTicks();if(auto* midi=dynamic_cast<MidiClip*>(clip)){value.type=QStringLiteral("midi");value.noteCount=midi->notes().size();value.currentInPianoRoll=gui::getGUI()&&gui::getGUI()->pianoRoll()&&gui::getGUI()->pianoRoll()->aiBridgeCurrentMidiClip()==midi;}else value.type=QStringLiteral("clip");result.append(value);}return result;
}
AiBridgeClipValue AiBridgeProjectContext::clip(const QString& clipValue,AiBridgePage& page) const {
 AiBridgeClipValue result;if(!qt())return result;auto* clip=resolveClip(clipValue);if(!clip){page={};return result;}result.id=clipValue;if(clip->getTrack())result.trackId=trackId(clip->getTrack());result.name=clip->displayName();result.startTicks=clip->startPosition().getTicks();result.lengthTicks=clip->length().getTicks();if(auto* midi=dynamic_cast<MidiClip*>(clip))result.currentInPianoRoll=gui::getGUI()&&gui::getGUI()->pianoRoll()&&gui::getGUI()->pianoRoll()->aiBridgeCurrentMidiClip()==midi;
 if(auto* midi=dynamic_cast<MidiClip*>(clip)){result.type=QStringLiteral("midi");result.autoResize=midi->getAutoResize();result.noteCount=static_cast<int>(midi->notes().size());for(int i=0;i<static_cast<int>(midi->notes().size());++i){auto* note=midi->notes().at(static_cast<std::size_t>(i));AiBridgeNoteValue value{note->pos().getTicks(),note->length().getTicks(),note->key(),note->getVolume(),note->getPanning(),i,note->type()==Note::Type::Step?QStringLiteral("step"):QStringLiteral("regular")};result.notes.append(value);}std::stable_sort(result.notes.begin(),result.notes.end(),[](const auto&a,const auto&b){return std::tie(a.position,a.key,a.length,a.volume,a.panning,a.noteType,a.sourceOrder)<std::tie(b.position,b.key,b.length,b.volume,b.panning,b.noteType,b.sourceOrder);});result.contentHash=AiBridgeMutationTypes::contentHash(mutationState(midi));page.total=result.noteCount;page.limit=cap(page.limit);page.offset=qBound(0,page.offset,page.total);page.hasMore=page.offset+page.limit<page.total;result.notes=result.notes.mid(page.offset,page.limit);}else{result.type=QStringLiteral("clip");page={};}return result;
}
AiBridgeProjectValue AiBridgeProjectContext::projectStructure() const {
 AiBridgeProjectValue result;if(!qt())return result;reconcile();auto* song=Engine::getSong();if(!song)return result;result.songName=song->displayName();result.tempo=song->getTempo();result.lengthBars=song->length();result.lengthTicks=TimePos(song->length(),0).getTicks();result.hasLengthTicks=true;result.trackCount=song->tracks().size();AiBridgePage page{result.trackCount,0,StructureTrackCap,result.trackCount>StructureTrackCap};result.tracks=tracks(page);return result;
}
AiBridgeContextValue AiBridgeProjectContext::currentContext() const {
 AiBridgeContextValue result;if(!qt()){result.code=QStringLiteral("wrong_thread");result.message=QStringLiteral("context requires Qt application thread");return result;}reconcile();auto* song=Engine::getSong();result.success=true;result.code=QStringLiteral("ok");result.message=QStringLiteral("context snapshot captured");result.projectGeneration=s_generation;if(song){result.songName=song->displayName();result.tempo=song->getTempo();}auto* piano=gui::getGUI()?gui::getGUI()->pianoRoll():nullptr;auto* midi=piano?piano->aiBridgeCurrentMidiClip():nullptr;if(midi){result.hasCurrentMidiClip=true;result.clipLengthTicks=midi->length().getTicks();result.clipId=clipId(midi);if(midi->instrumentTrack()){auto* track=midi->instrumentTrack();result.hasOwningInstrumentTrack=true;result.trackId=trackId(track);result.trackName=track->displayName();result.trackVolume=track->getVolume();}}return result;
}
AiBridgeClipValue AiBridgeProjectContext::openableClip(const QString& id,QString& code,QString& message) const {auto* clip=resolveClip(id);if(!clip){code=QStringLiteral("stale_clip");message=QStringLiteral("clip is not live");return {};}if(!dynamic_cast<MidiClip*>(clip)){code=QStringLiteral("clip_not_midi");message=QStringLiteral("clip is not MIDI");return {};}code=QStringLiteral("ok");message=QStringLiteral("resolved");AiBridgePage page{0,0,1,false};return this->clip(id,page);}
}
