#include "RecordingRecovery.h"
#include "RecordingTakeJournal.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QThread>
#include <QUuid>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
namespace lmms::recording {
namespace {
constexpr std::uint32_t ScanBudget=256,MaximumPage=100,RecentLimit=64;
bool id(const QString& value){const QUuid uuid(value);return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces)==value;}
std::uint16_t le16(const char* p){return std::uint16_t(std::uint8_t(p[0]))|std::uint16_t(std::uint8_t(p[1]))<<8;}
std::uint32_t le32(const char* p){std::uint32_t value=0;for(unsigned i=0;i<4;++i)value|=std::uint32_t(std::uint8_t(p[i]))<<(8*i);return value;}
bool count(const QJsonValue& value,std::uint64_t& out)
{
 if(!value.isString())return false;
 const auto text=value.toString();bool ok=false;const auto parsed=text.toULongLong(&ok);
 if(!ok || QString::number(parsed)!=text)return false;
 out=parsed;return true;
}
QByteArray digest(QIODevice& source)
{
 if(!source.seek(0))return {};
 QCryptographicHash hash(QCryptographicHash::Sha256);std::array<char,65536> buffer{};qint64 total=0;
 for(;;){const auto n=source.read(buffer.data(),qint64(buffer.size()));if(n<0)return {};if(!n)break;hash.addData(buffer.data(),int(n));total+=n;}
 return total==source.size() && source.seek(0)?hash.result():QByteArray{};
}
QJsonObject json(QIODevice& file,qint64 maximum)
{
 if(file.size()<0 || file.size()>maximum || !file.seek(0))return {};
 const auto bytes=file.read(maximum+1);if(bytes.size()!=file.size())return {};
 QJsonParseError error{};const auto doc=QJsonDocument::fromJson(bytes,&error);return error.error==QJsonParseError::NoError && doc.isObject()?doc.object():QJsonObject{};
}
QByteArray metadataBytes(QJsonObject object)
{
 object["metadata_sha256"]=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(object).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex());
 return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
QJsonObject checkedMetadata(QIODevice& source,qint64 bound)
{
 auto object=json(source,bound);const auto expected=object.take("metadata_sha256").toString();
 return expected==QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(object).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex())?object:QJsonObject{};
}
struct Inspected {TakeManifest manifest;RecordingLibrary::RecoveryFiles files;bool dismissed=false;};
TakeManifest quarantine(TakeManifest value,const QString& code)
{value.state="quarantined";value.code=code;value.captureIntegrity="failed";value.recoverableFrames=0;return value;}
bool sameSource(const TakeManifest& left,const TakeManifest& right)
{
 return left.descriptor.projectId==right.descriptor.projectId && left.descriptor.takeId==right.descriptor.takeId
  && left.source.sourceIdentity==right.source.sourceIdentity && left.journalIdentity==right.journalIdentity
  && left.source.sourceDigest==right.source.sourceDigest && left.journalDigest==right.journalDigest;
}
bool dismissalMatches(const QJsonObject& marker,const TakeManifest& m)
{
 return marker["schema"]==1 && marker["project_id"]==m.descriptor.projectId && marker["take_id"]==m.descriptor.takeId
  && marker["audio_sha256"].toString()==QString::fromLatin1(m.source.sourceDigest.toHex())
  && marker["journal_sha256"].toString()==QString::fromLatin1(m.journalDigest.toHex());
}
void readReceipt(TakeManifest& m,QIODevice* file)
{
 if(!file || !file->size())return; // Earlier/crashed takes need not have a post-close receipt.
 if(file->size()>32768 || !file->seek(0)){m.diagnosticCode="receipt_unavailable";return;}
 const auto bytes=file->read(32769);if(bytes.size()!=file->size() || !bytes.endsWith('\n')){m.diagnosticCode="receipt_torn";return;}
 const auto lines=bytes.split('\n');if(lines.size()>9){m.diagnosticCode="receipt_limit";return;}
 QJsonObject latest;unsigned sequence=0;
 for(int i=0;i+1<lines.size();++i)
 {
  if(lines[i].isEmpty() || lines[i].size()+1>4096){m.diagnosticCode="receipt_invalid";return;}
  QJsonParseError error{};const auto parsed=QJsonDocument::fromJson(lines[i],&error);
  if(error.error!=QJsonParseError::NoError || !parsed.isObject()){m.diagnosticCode="receipt_invalid";return;}
  auto record=parsed.object();const auto checksum=record.take("record_sha256").toString();
  if(checksum!=QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(record).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex())
   || record["schema"]!=1 || record["sequence"].toInt()!=int(++sequence) || record["project_id"]!=m.descriptor.projectId || record["take_id"]!=m.descriptor.takeId)
  {m.diagnosticCode="receipt_invalid";return;}
  latest=std::move(record);
 }
 const auto counters=latest["counts"].toObject(),observations=latest["observations"].toObject();
 const auto fault=counters["input_fault"].toInt(-1),quality=counters["timestamp_quality"].toInt(-1);
 if(fault<0 || fault>int(InputFault::RingContractMismatch) || quality<0 || quality>int(InputTimestampQuality::Discontinuous))
 {m.diagnosticCode="receipt_invalid";return;}
 auto updated=m;updated.sourceFaultKnown=true;
 if(updated.originalFault==InputFault::None)updated.originalFault=InputFault(fault);
 else if(updated.originalFault!=InputFault(fault))updated.diagnosticCode="receipt_fault_conflict";
 updated.timestampQuality=std::max(updated.timestampQuality,InputTimestampQuality(quality));
 if(updated.originalFault!=InputFault::None)updated.captureIntegrity="incomplete";
 if(!observations.isEmpty())
 {
  std::uint64_t generation=0,revision=0,intervals=0,dropped=0;
  const auto aggregate=observations["quality"].toInt(-1);
  if(!count(observations["project_generation"],generation) || !count(observations["session_revision"],revision)
   || generation!=m.descriptor.epoch.projectGeneration || revision!=m.descriptor.epoch.sessionRevision
   || !count(observations["adc_interval_discontinuities"],intervals) || !count(counters["observations_dropped"],dropped)
   || aggregate<0 || aggregate>int(InputTimestampQuality::Discontinuous))
  {m.diagnosticCode="receipt_observations_invalid";return;}
  updated.timestampQuality=std::max(updated.timestampQuality,InputTimestampQuality(aggregate));updated.timestampDiscontinuities=intervals;
  updated.observationsDropped=dropped;updated.observationLossKnown=true;
 }
 m=std::move(updated);
}
Inspected inspect(RecordingLibrary::RecoveryFiles files,const QString& project,const QString& take)
{
 Inspected inspected;auto& m=inspected.manifest;inspected.files=std::move(files);auto& f=inspected.files;
 m.descriptor.projectId=project;m.descriptor.takeId=take;m.state="incomplete";m.code=f.code;
 m.source.projectId=project;m.source.takeId=take;m.source.sourceIdentity=f.audioIdentity;m.journalIdentity=f.journalIdentity;
 if(f.audio){m.fileBytes=std::uint64_t(f.audio->size());m.source.rawFrames=m.fileBytes>=44?(m.fileBytes-44)/2:0;m.source.sourceDigest=digest(*f.audio);}
 if(f.journal)m.journalDigest=digest(*f.journal);
 if(f.code!="ok")
 {if(f.code!="take_busy")m=quarantine(m,f.code);return inspected;}
 if(!m.fileBytes && !f.journal->size()){m.code="no_verified_checkpoint";return inspected;}
 auto bytes=f.journal->read(65537);
 if(bytes.size()!=f.journal->size() || bytes.size()<4){m=quarantine(m,"torn_manifest_header");return inspected;}
 if(std::uint8_t(bytes[1])!=1){m=quarantine(m,"unsupported_schema");return inspected;}
 const auto headerSize=le16(bytes.constData()+2);
 TakeDescriptor d;
 if(headerSize>TakeJournalRecordMaximum || headerSize>bytes.size() || !RecordingTakeJournalCodec::validateHeader(bytes.left(headerSize),d)
  || d.projectId!=project || d.takeId!=take){m=quarantine(m,"invalid_manifest");return inspected;}
 m.descriptor=d;m.source.sampleRate=d.sampleRate;
 if(m.fileBytes<44){m=quarantine(m,"torn_audio_header");return inspected;}
 m.source.rawFrames=(m.fileBytes-44)/2;
 if(m.fileBytes>44+d.maxRawFrames*2){m=quarantine(m,"audio_limit");return inspected;}
 const auto audioHeader=f.audio->read(44);const auto canonical=RecordingWavHeader::encodePcm16(d.sampleRate,0);
 if(audioHeader.size()!=44 || !canonical.valid){m=quarantine(m,"invalid_audio_header");return inspected;}
 for(int i=0;i<44;++i)if(!((i>=4 && i<8)||i>=40) && std::uint8_t(audioHeader[i])!=canonical.bytes[std::size_t(i)])
 {m=quarantine(m,"invalid_audio_header");return inspected;}
 TakeJournalCursor cursor;std::size_t at=headerSize;bool sealIntent=false,complete=false;std::uint64_t intentFrames=0;
 while(at<std::size_t(bytes.size()))
 {
  const auto remaining=std::size_t(bytes.size())-at;
  if(complete){m=quarantine(m,"records_after_terminal");return inspected;}
  if(remaining<4){m.tornJournalTail=true;break;}
  const auto* record=reinterpret_cast<const std::uint8_t*>(bytes.constData()+at);const auto size=le16(reinterpret_cast<const char*>(record+2));
  if((record[0]!=2 && record[0]!=3) || record[1]!=1 || size<8 || size>TakeJournalRecordMaximum){m=quarantine(m,"invalid_manifest_record");return inspected;}
  if(size>remaining){m.tornJournalTail=true;break;}
  const auto previous=cursor.lastProgress;
  if(!RecordingTakeJournalCodec::validateRecord(record,size,cursor)){m=quarantine(m,"invalid_manifest_record");return inspected;}
  const auto& p=cursor.lastProgress;
  if(p.acceptedFrames>d.maxRawFrames || p.writtenFrames>m.source.rawFrames || p.receivedFrames>d.maxRawFrames+4096
   || p.applicationDroppedFrames>p.receivedFrames || p.invalidSamples>p.receivedFrames || p.clippedSamples>p.writtenFrames
   || p.intentionallyUnrecordedFrames>p.receivedFrames-p.acceptedFrames)
  {m=quarantine(m,"manifest_counter_mismatch");return inspected;}
  if(record[0]==3 && record[4]==std::uint8_t(TakeTerminal::SealIntent))
  {if(!previous.sequence || previous.writtenFrames!=p.writtenFrames){m=quarantine(m,"invalid_seal_intent");return inspected;}sealIntent=true;intentFrames=p.writtenFrames;}
  if(record[0]==3 && record[4]==std::uint8_t(TakeTerminal::Complete))
  {if(!f.sealed || !sealIntent || p.writtenFrames!=intentFrames){m=quarantine(m,"invalid_complete");return inspected;}complete=true;}
  at+=size;
 }
 m.progress=cursor.lastProgress;m.originalFault=m.progress.fault;m.sourceFaultKnown=complete || m.originalFault!=InputFault::None;
 m.durableFrames=m.progress.writtenFrames;m.recoverableFrames=m.durableFrames;m.unknownTailBytes=m.fileBytes-44-m.durableFrames*2;
 m.timestampQuality=m.progress.timestampQuality;
 if(m.source.sourceDigest.size()!=32){m=quarantine(m,"source_read_failed");return inspected;}
 if(f.sealed)
 {
  if(le32(audioHeader.constData()+4)!=m.fileBytes-8 || le32(audioHeader.constData()+40)!=m.fileBytes-44 || m.unknownTailBytes || !sealIntent)
  {m=quarantine(m,"sealed_counter_mismatch");return inspected;}
  if(complete)
  {
   const QByteArray expected(reinterpret_cast<const char*>(m.progress.sourceDigest.data()),32);
   if(!m.progress.sourceDigestKnown || m.source.sourceDigest!=expected){m=quarantine(m,"sealed_digest_mismatch");return inspected;}
   m.state="complete";m.captureIntegrity="complete";m.code="ok";
  }
  else{m.state="saved_unattached";m.code="terminal_not_durable";}
 }
 else{m.state=m.durableFrames?"recoverable":"incomplete";m.code=m.durableFrames?"ok":"no_verified_checkpoint";}
 // Retain only copied parsed metadata; load each bounded companion after releasing the journal buffer.
 bytes.clear();bytes.squeeze();
 readReceipt(m,f.receipt.get());
 if(f.provenance)
 {
  const auto p=checkedMetadata(*f.provenance,65536);std::uint64_t frames=0;
  if(p["schema"]!=1 || p["project_id"]!=project || p["take_id"]!=take || !id(p["recovered_from_project_id"].toString())
   || !id(p["recovered_from_take_id"].toString()) || !count(p["frames"],frames) || frames!=m.source.rawFrames
   || p["audio_sha256"].toString()!=QString::fromLatin1(m.source.sourceDigest.toHex()))
  {m=quarantine(m,"invalid_recovery_metadata");return inspected;}
  const auto quality=p["input_timestamp_quality"].toInt(-1),fault=p["original_fault"].toInt(-1);
  if(quality<0 || quality>2 || fault<0 || fault>int(InputFault::RingContractMismatch)){m=quarantine(m,"invalid_recovery_metadata");return inspected;}
  std::uint64_t volume=0,index=0,journalVolume=0,journalIndex=0,intervals=0,dropped=0;
  if(!count(p["sealed_file_volume"],volume) || !count(p["sealed_file_index"],index)
   || !count(p["journal_file_volume"],journalVolume) || !count(p["journal_file_index"],journalIndex)
   || volume!=m.source.sourceIdentity.volume || index!=m.source.sourceIdentity.index
   || journalVolume!=m.journalIdentity.volume || journalIndex!=m.journalIdentity.index)
  {m=quarantine(m,"source_identity_changed");return inspected;}
  if(!count(p["timestamp_interval_discontinuities"],intervals)
   || (p["observation_loss_known"].toBool() && !count(p["observations_dropped"],dropped)))
  {m=quarantine(m,"invalid_recovery_metadata");return inspected;}
  std::uint64_t durable=0,readable=0,tail=0;
  if(!count(p["source_durable_frames"],durable) || !count(p["original_readable_frames"],readable) || !count(p["unknown_original_tail_bytes"],tail)
   || durable!=frames || readable<durable || readable>m.descriptor.maxRawFrames
   || tail<2*(readable-durable) || tail-2*(readable-durable)>1)
  {m=quarantine(m,"invalid_recovery_metadata");return inspected;}
  m.recoveredFromDurableFrames=durable;m.recoveredFromReadableFrames=readable;m.recoveredFromUnknownTailBytes=tail;
  m.timestampDiscontinuities=intervals;m.observationsDropped=dropped;m.observationLossKnown=p["observation_loss_known"].toBool();
  m.recoveredFromProjectId=p["recovered_from_project_id"].toString();m.recoveredFromTakeId=p["recovered_from_take_id"].toString();
  m.timestampQuality=std::max(m.timestampQuality,InputTimestampQuality(quality));m.originalFault=InputFault(fault);m.sourceFaultKnown=p["source_fault_known"].toBool();
  m.captureIntegrity="incomplete"; // A sealed recovered prefix is not completion of the interrupted original capture.
 }
 if(f.dismissal)
 {
  const auto marker=checkedMetadata(*f.dismissal,1024);
  if(dismissalMatches(marker,m))inspected.dismissed=true;
  else m.diagnosticCode="dismissal_identity_changed";
 }
 return inspected;
}
}
struct RecordingRecovery::Impl
{
 explicit Impl(TakeIdGenerator value):generator(value?std::move(value):TakeIdGenerator([]{return QUuid::createUuid().toString(QUuid::WithoutBraces);})){}
 TakeIdGenerator generator;
 QThread* owner=QThread::currentThread();
 QPointer<RecordingLibrary> library;
 std::unique_ptr<RecordingLibrary::RecoveryScan> scanner;
 QString cursor;
 std::vector<TakeManifest> recent,currentPage;
 void assertOwner() const{Q_ASSERT(QThread::currentThread()==owner);}
 const TakeManifest* lookup(const QString& take) const
 {
  if(!id(take))return nullptr;
  const TakeManifest* result=nullptr;
  for(const auto* list:{&currentPage,&recent})for(const auto& m:*list)if(m.descriptor.takeId==take)
  {if(result && result->descriptor.projectId!=m.descriptor.projectId)return nullptr;result=&m;}
  return result;
 }
 void remember(const TakeManifest& manifest)
 {
  recent.erase(std::remove_if(recent.begin(),recent.end(),[&](const auto& m){return m.descriptor.projectId==manifest.descriptor.projectId && m.descriptor.takeId==manifest.descriptor.takeId;}),recent.end());
  recent.push_back(manifest);if(recent.size()>RecentLimit)recent.erase(recent.begin());
 }
};
RecordingRecovery::RecordingRecovery(TakeIdGenerator generator):m_impl(std::make_unique<Impl>(std::move(generator))){}
RecordingRecovery::~RecordingRecovery()=default;
RecoveryPage RecordingRecovery::scan(RecordingLibrary& library,RecoveryCursor cursor,std::uint32_t limit)
{
 auto& s=*m_impl;s.assertOwner();
 if(!limit || limit>MaximumPage)return {"invalid_page"};
 const auto admission=library.recoveryAdmissionCode();if(admission!="ok")return {admission};
 if(cursor.token.isEmpty())
 {
  if(s.library!=&library){s.recent.clear();s.currentPage.clear();}s.library=&library;
  auto started=library.beginRecoveryScan();if(!started.scan)return {started.code};s.scanner=std::move(started.scan);
 }
 else if(s.library!=&library || !s.scanner || cursor.token!=s.cursor)return {"stale_cursor"};
 RecoveryPage page;page.code="ok";
 for(;page.inspectedCandidates<ScanBudget && page.takes.size()<limit;)
 {
  const auto name=s.scanner->next();++page.inspectedCandidates;
  if(name.code!="ok"){page.code=name.code;s.scanner.reset();s.cursor.clear();return page;}
  if(name.end){s.scanner.reset();break;}
  if(!name.candidate)continue;
  auto inspected=inspect(library.readRecoveryFiles({name.projectId,1},name.takeId),name.projectId,name.takeId);
  if(!inspected.dismissed && inspected.files.dismissal)
   inspected.dismissed=dismissalMatches(checkedMetadata(*inspected.files.dismissal,1024),inspected.manifest);
  if(!inspected.dismissed){s.remember(inspected.manifest);page.takes.push_back(std::move(inspected.manifest));}
 }
 s.currentPage=page.takes;page.hasMore=bool(s.scanner);
 s.cursor=page.hasMore?QUuid::createUuid().toString(QUuid::WithoutBraces):QString{};page.next.token=s.cursor;return page;
}
RecoveryResult RecordingRecovery::recover(const QString& take)
{
 auto& s=*m_impl;s.assertOwner();if(!s.library)return {"unauthorized_library"};
 const auto admission=s.library->recoveryAdmissionCode();if(admission!="ok")return {admission};
 const auto* saved=s.lookup(take);if(!saved)return {"unknown_take"};const auto previous=*saved;
 auto inspected=inspect(s.library->readRecoveryFiles({previous.descriptor.projectId,1},take),previous.descriptor.projectId,take);
 const auto& original=inspected.manifest;
 if(!sameSource(previous,original))return {"source_identity_changed"};
 if((original.state!="recoverable" && original.state!="saved_unattached") || !original.recoverableFrames)return {"not_recoverable"};
 const auto target=s.generator();if(!id(target) || target==take)return {"invalid_recovery_identity"};
 auto descriptor=original.descriptor;descriptor.takeId=target;descriptor.label="Recovered take";
 TakeManifest retained;retained.descriptor=descriptor;retained.state="incomplete";retained.timestampQuality=original.timestampQuality;
 retained.recoveredFromProjectId=original.descriptor.projectId;retained.recoveredFromTakeId=take;
 retained.recoveredFromDurableFrames=original.durableFrames;retained.recoveredFromReadableFrames=original.source.rawFrames;retained.recoveredFromUnknownTailBytes=original.unknownTailBytes;
 retained.originalFault=original.originalFault;retained.sourceFaultKnown=original.sourceFaultKnown;
 retained.source.projectId=descriptor.projectId;retained.source.takeId=target;retained.source.sampleRate=descriptor.sampleRate;
 auto created=s.library->createUniqueTake({descriptor.projectId,1},{target,descriptor.label},descriptor.sampleRate,descriptor.maxRawFrames);
 if(!created.sink){retained.code=created.code;return {created.code,std::move(retained)};}
 auto& sink=*created.sink;
 auto fail=[&](const QString& code)->RecoveryResult{if(created.sink)created.sink->preservePartial();retained.code=code;return {code,retained};};
 if(sink.beginTake(descriptor)!=WriterIoCode::Ok)return fail("recovery_write_failed");
 retained.fileBytes=44;
 if(!inspected.files.audio->seek(44))return fail("recovery_read_failed");
 TakeProgress progress;progress.timestampQuality=original.timestampQuality;
 std::array<std::uint8_t,65536> buffer{};std::uint64_t written=0,lastCheckpoint=0;
 while(written<original.recoverableFrames)
 {
  const auto frames=std::min({std::uint64_t(buffer.size()/2),original.recoverableFrames-written,std::uint64_t(descriptor.sampleRate)-(written-lastCheckpoint)});
  const auto bytes=std::size_t(frames*2);const auto read=inspected.files.audio->read(reinterpret_cast<char*>(buffer.data()),qint64(bytes));
  if(read!=qint64(bytes))return fail("recovery_read_failed");
  std::size_t at=0;
  while(at<bytes){const auto appended=sink.appendPayload({buffer.data()+at,bytes-at});
   if(appended.bytesWritten>bytes-at)return fail("recovery_write_failed");
   at+=appended.bytesWritten;retained.fileBytes+=appended.bytesWritten;
   retained.source.rawFrames=(retained.fileBytes-44)/2;retained.unknownTailBytes=retained.fileBytes-44-retained.durableFrames*2;
   if(appended.code!=WriterIoCode::Ok || !appended.bytesWritten)return fail("recovery_write_failed");}
  written+=frames;progress.receivedFrames=progress.acceptedFrames=progress.writtenFrames=written;retained.progress=progress;retained.source.rawFrames=written;
  if(written-lastCheckpoint==descriptor.sampleRate || written==original.recoverableFrames)
  {++progress.sequence;if(sink.appendCheckpoint(progress)!=WriterIoCode::Ok)return fail("recovery_checkpoint_failed");
   lastCheckpoint=written;retained.durableFrames=written;retained.progress=progress;retained.unknownTailBytes=retained.fileBytes-44-written*2;}
 }
 ++progress.sequence;if(sink.appendSealIntent(progress)!=WriterIoCode::Ok)return fail("recovery_checkpoint_failed");
 const auto sealed=sink.sealPcm16(descriptor.sampleRate,written);
 if(sealed!=WriterIoCode::Ok)return fail(sealed==WriterIoCode::Collision?"recovery_collision":"recovery_seal_failed");
 retained.state="saved_unattached";
 const auto hash=sink.sealedAudioSha256();if(hash.size()!=32)return fail("recovery_seal_failed");
 progress.sourceDigestKnown=true;std::memcpy(progress.sourceDigest.data(),hash.constData(),32);++progress.sequence;
 if(sink.appendTerminal(TakeTerminal::Complete,progress)!=WriterIoCode::Ok)return fail("recovery_terminal_failed");
 retained.progress=progress;
 retained.source.sourceDigest=hash;
 created.sink.reset();created.receipt.reset();
 auto sealedFiles=s.library->readRecoveryFiles({descriptor.projectId,1},target);
 if(sealedFiles.code!="ok" || !sealedFiles.sealed)return fail("recovery_verification_failed");
 retained.source.sourceIdentity=sealedFiles.audioIdentity;retained.journalIdentity=sealedFiles.journalIdentity;
 QJsonObject provenance{{"schema",1},{"project_id",descriptor.projectId},{"take_id",target},
  {"recovered_from_project_id",original.descriptor.projectId},{"recovered_from_take_id",take},
  {"sealed_file_volume",QString::number(sealedFiles.audioIdentity.volume)},{"sealed_file_index",QString::number(sealedFiles.audioIdentity.index)},
  {"journal_file_volume",QString::number(sealedFiles.journalIdentity.volume)},{"journal_file_index",QString::number(sealedFiles.journalIdentity.index)},
  {"original_file_volume",QString::number(original.source.sourceIdentity.volume)},{"original_file_index",QString::number(original.source.sourceIdentity.index)},
  {"original_readable_frames",QString::number(original.source.rawFrames)},
  {"frames",QString::number(written)},{"source_durable_frames",QString::number(original.durableFrames)},
  {"unknown_original_tail_bytes",QString::number(original.unknownTailBytes)},
  {"original_audio_sha256",QString::fromLatin1(original.source.sourceDigest.toHex())},
  {"original_journal_sha256",QString::fromLatin1(original.journalDigest.toHex())},{"audio_sha256",QString::fromLatin1(hash.toHex())},
  {"original_fault",int(original.originalFault)},{"source_fault_known",original.sourceFaultKnown},
  {"input_timestamp_quality",int(original.timestampQuality)},{"timestamp_interval_discontinuities",QString::number(original.timestampDiscontinuities)},
  {"observations_dropped",original.observationLossKnown?QJsonValue(QString::number(original.observationsDropped)):QJsonValue{}},
  {"observation_loss_known",original.observationLossKnown},
  {"placement_quality","uncalibrated"},{"drift_quality","unmeasured"},{"placement_state","saved_unattached"}};
 const auto savedMetadata=s.library->writeRecoveryMetadata({descriptor.projectId,1},target,RecordingLibrary::RecoveryMetadata::Provenance,metadataBytes(provenance));
 if(savedMetadata!="ok")return fail("recovery_metadata_failed");
 auto recovered=inspect(s.library->readRecoveryFiles({descriptor.projectId,1},target),descriptor.projectId,target);
 if(recovered.manifest.state!="complete")return fail("recovery_verification_failed");
 auto token=s.library->mintRecoveryToken(recovered.manifest.source);if(!token)return fail("recovery_verification_failed");
 recovered.manifest.state="saved_unattached";recovered.manifest.code="recovered";s.remember(recovered.manifest);
 return {"recovered",std::move(recovered.manifest),std::move(token)};
}
QString RecordingRecovery::dismiss(const QString& take)
{
 auto& s=*m_impl;s.assertOwner();if(!s.library)return "unauthorized_library";
 const auto admission=s.library->recoveryAdmissionCode();if(admission!="ok")return admission;
 const auto* saved=s.lookup(take);if(!saved)return "unknown_take";const auto previous=*saved;
 auto inspected=inspect(s.library->readRecoveryFiles({previous.descriptor.projectId,1},take),previous.descriptor.projectId,take);
 if(!sameSource(previous,inspected.manifest) || inspected.manifest.source.sourceDigest.size()!=32)return "source_identity_changed";
 if(inspected.dismissed || (inspected.files.dismissal && dismissalMatches(checkedMetadata(*inspected.files.dismissal,1024),inspected.manifest)))return "dismissed";
 const auto& m=inspected.manifest;
 const QJsonObject marker{{"schema",1},{"project_id",m.descriptor.projectId},{"take_id",take},
  {"audio_sha256",QString::fromLatin1(m.source.sourceDigest.toHex())},{"journal_sha256",QString::fromLatin1(m.journalDigest.toHex())}};
 const auto result=s.library->writeRecoveryMetadata({m.descriptor.projectId,1},take,RecordingLibrary::RecoveryMetadata::Dismissal,metadataBytes(marker));
 return result=="ok"?QString("dismissed"):result;
}
std::vector<TakeManifest> RecordingRecovery::recentTakes() const{m_impl->assertOwner();return m_impl->recent;}
}
