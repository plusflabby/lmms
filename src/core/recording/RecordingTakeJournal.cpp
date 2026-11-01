#include "RecordingTakeJournal.h"
#include <QUuid>
#include <algorithm>
#include <cstring>
#include <limits>

namespace lmms::recording {
namespace {
constexpr std::uint8_t HeaderType=1, ProgressType=2, TerminalType=3, Version=1;
constexpr std::size_t PrefixBytes=4, ChecksumBytes=4;
void put16(QByteArray& out,std::uint16_t value){out.append(char(value));out.append(char(value>>8));}
void put32(QByteArray& out,std::uint32_t value){for(unsigned i=0;i<4;++i)out.append(char(value>>(8*i)));}
void put64(QByteArray& out,std::uint64_t value){for(unsigned i=0;i<8;++i)out.append(char(value>>(8*i)));}
std::uint16_t get16(const char* p){return std::uint16_t(std::uint8_t(p[0]))|std::uint16_t(std::uint8_t(p[1]))<<8;}
std::uint32_t get32(const char* p){std::uint32_t v=0;for(unsigned i=0;i<4;++i)v|=std::uint32_t(std::uint8_t(p[i]))<<(8*i);return v;}
std::uint64_t get64(const char* p){std::uint64_t v=0;for(unsigned i=0;i<8;++i)v|=std::uint64_t(std::uint8_t(p[i]))<<(8*i);return v;}
std::uint32_t checksum(const char* data,std::size_t size) noexcept
{
 std::uint32_t value=0xffffffffu;
 for(std::size_t i=0;i<size;++i){value^=std::uint8_t(data[i]);for(unsigned bit=0;bit<8;++bit)value=(value>>1)^(0xedb88320u&std::uint32_t(-std::int32_t(value&1)));}
 return ~value;
}
bool validId(const QString& value)
{
 const QUuid uuid(value);return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces)==value;
}
bool validDescriptor(const TakeDescriptor& d)
{
 return validId(d.projectId) && validId(d.takeId) && d.epoch && d.projectGeneration>0 && d.epoch.projectGeneration==d.projectGeneration
  && (d.sampleRate==44100 || d.sampleRate==48000) && d.maxRawFrames>0
  && d.maxRawFrames<=std::uint64_t(d.sampleRate)*120 && d.targetTick>=0 && d.countInBars<=16
  && d.label.toUtf8().size()<=96;
}
QByteArray checkedRecord(std::uint8_t type,const QByteArray& body)
{
 QByteArray out;out.reserve(int(PrefixBytes+body.size()+ChecksumBytes));out.append(char(type));out.append(char(Version));put16(out,std::uint16_t(PrefixBytes+body.size()+ChecksumBytes));out+=body;put32(out,checksum(out.constData(),std::size_t(out.size())));return out;
}
QByteArray progressBody(const TakeProgress& p)
{
 QByteArray body;body.reserve(100);
 put64(body,p.sequence);put64(body,p.receivedFrames);put64(body,p.acceptedFrames);put64(body,p.writtenFrames);
 put64(body,p.applicationDroppedFrames);put64(body,p.intentionallyUnrecordedFrames);put64(body,p.clippedSamples);
 put64(body,p.invalidSamples);put64(body,p.hardwareLostFrames);body.append(char(p.fault));body.append(char(p.timestampQuality));
 body.append(char(p.hardwareLossCountKnown?1:0));body.append(char(p.sourceDigestKnown?1:0));
 body.append(reinterpret_cast<const char*>(p.sourceDigest.data()),int(p.sourceDigest.size()));return body;
}
bool validProgress(const TakeProgress& p) noexcept
{
 return p.sequence>0 && p.sequence<std::numeric_limits<std::uint64_t>::max()
  && p.receivedFrames>=p.acceptedFrames && p.acceptedFrames>=p.writtenFrames
  && std::uint8_t(p.fault)<=std::uint8_t(InputFault::RingContractMismatch)
  && std::uint8_t(p.timestampQuality)<=std::uint8_t(InputTimestampQuality::Discontinuous);
}
bool validComplete(const TakeProgress& p) noexcept
{
 return p.acceptedFrames>0 && p.acceptedFrames==p.writtenFrames
  && !p.applicationDroppedFrames && !p.invalidSamples && p.fault==InputFault::None && p.sourceDigestKnown
  && p.hardwareLossCountKnown && !p.hardwareLostFrames;
}
}

QByteArray RecordingTakeJournalCodec::encodeHeader(const TakeDescriptor& d)
{
 if(!validDescriptor(d))return {};
 const auto project=d.projectId.toUtf8(),take=d.takeId.toUtf8(),label=d.label.toUtf8();
 QByteArray body;body.reserve(64+project.size()+take.size()+label.size());
 body.append(char(Version));body.append(char(project.size()));body.append(char(take.size()));body.append(char(label.size()));
 put64(body,d.epoch.projectGeneration);put64(body,d.epoch.sessionRevision);put64(body,d.projectGeneration);put64(body,d.maxRawFrames);
 put64(body,std::uint64_t(d.targetTick));put32(body,d.sampleRate);put32(body,d.countInBars);
 body+=project;body+=take;body+=label;
 const auto record=checkedRecord(HeaderType,body);return record.size()<=int(TakeJournalRecordMaximum)?record:QByteArray{};
}

bool RecordingTakeJournalCodec::validateHeader(const QByteArray& bytes,TakeDescriptor& d)
{
 if(bytes.size()<4+4+40+4 || bytes.size()>int(TakeJournalRecordMaximum))return false;
 const auto* p=bytes.constData();const auto size=std::size_t(bytes.size());
 if(std::uint8_t(p[0])!=HeaderType || std::uint8_t(p[1])!=Version || get16(p+2)!=size
  || get32(p+size-4)!=checksum(p,size-4))return false;
 const auto body=p+4;const auto projectLen=std::uint8_t(body[1]),takeLen=std::uint8_t(body[2]),labelLen=std::uint8_t(body[3]);
 if(std::uint8_t(body[0])!=Version || !projectLen || !takeLen || labelLen>96
  || size!=4+4+8*5+4*2+projectLen+takeLen+labelLen+4)return false;
 TakeDescriptor parsed;std::size_t at=4;
 parsed.epoch.projectGeneration=get64(body+at);at+=8;parsed.epoch.sessionRevision=get64(body+at);at+=8;
 parsed.projectGeneration=get64(body+at);at+=8;parsed.maxRawFrames=get64(body+at);at+=8;parsed.targetTick=std::int64_t(get64(body+at));at+=8;
 parsed.sampleRate=get32(body+at);at+=4;parsed.countInBars=get32(body+at);at+=4;
 parsed.projectId=QString::fromUtf8(body+at,projectLen);at+=projectLen;parsed.takeId=QString::fromUtf8(body+at,takeLen);at+=takeLen;parsed.label=QString::fromUtf8(body+at,labelLen);
 if(!validDescriptor(parsed))return false;d=std::move(parsed);return true;
}

EncodedTakeJournalRecord RecordingTakeJournalCodec::encodeProgress(const TakeProgress& p) noexcept
{
 EncodedTakeJournalRecord result;if(!validProgress(p))return result;
 auto record=checkedRecord(ProgressType,progressBody(p));if(record.size()>int(result.bytes.size()))return result;
 result.size=std::uint16_t(record.size());std::memcpy(result.bytes.data(),record.constData(),std::size_t(record.size()));result.valid=true;return result;
}

EncodedTakeJournalRecord RecordingTakeJournalCodec::encodeSealIntent(const TakeProgress& p) noexcept
{
 EncodedTakeJournalRecord result;if(!validProgress(p))return result;
 auto body=progressBody(p);body.prepend(char(TakeTerminal::SealIntent));auto record=checkedRecord(TerminalType,body);
 if(record.size()>int(result.bytes.size()))return result;result.size=std::uint16_t(record.size());
 std::memcpy(result.bytes.data(),record.constData(),std::size_t(record.size()));result.valid=true;return result;
}

EncodedTakeJournalRecord RecordingTakeJournalCodec::encodeTerminal(TakeTerminal terminal,const TakeProgress& p) noexcept
{
 EncodedTakeJournalRecord result;if(!validProgress(p) || std::uint8_t(terminal)>std::uint8_t(TakeTerminal::SavedUnattached)
  || (terminal==TakeTerminal::Complete && !validComplete(p)))return result;
 auto body=progressBody(p);body.prepend(char(terminal));auto record=checkedRecord(TerminalType,body);
 if(record.size()>int(result.bytes.size()))return result;result.size=std::uint16_t(record.size());
 std::memcpy(result.bytes.data(),record.constData(),std::size_t(record.size()));result.valid=true;return result;
}

bool RecordingTakeJournalCodec::validateRecord(const std::uint8_t* data,std::size_t size,TakeJournalCursor& cursor) noexcept
{
 constexpr std::size_t bodySize=9*8+3+1+32;
 if(!data || size<4+bodySize+4 || size>TakeJournalRecordMaximum)return false;
 if((data[0]!=ProgressType && data[0]!=TerminalType) || data[1]!=Version || get16(reinterpret_cast<const char*>(data+2))!=size
  || get32(reinterpret_cast<const char*>(data+size-4))!=checksum(reinterpret_cast<const char*>(data),size-4))return false;
 const bool terminal=data[0]==TerminalType;const auto offset=terminal?1u:0u;
 if(size!=4+bodySize+4+offset)return false;
 const auto* p=data+4+offset;TakeProgress progress;std::size_t at=0;
 progress.sequence=get64(reinterpret_cast<const char*>(p+at));at+=8;progress.receivedFrames=get64(reinterpret_cast<const char*>(p+at));at+=8;
 progress.acceptedFrames=get64(reinterpret_cast<const char*>(p+at));at+=8;progress.writtenFrames=get64(reinterpret_cast<const char*>(p+at));at+=8;
 progress.applicationDroppedFrames=get64(reinterpret_cast<const char*>(p+at));at+=8;progress.intentionallyUnrecordedFrames=get64(reinterpret_cast<const char*>(p+at));at+=8;
 progress.clippedSamples=get64(reinterpret_cast<const char*>(p+at));at+=8;progress.invalidSamples=get64(reinterpret_cast<const char*>(p+at));at+=8;
 progress.hardwareLostFrames=get64(reinterpret_cast<const char*>(p+at));at+=8;progress.fault=InputFault(p[at++]);progress.timestampQuality=InputTimestampQuality(p[at++]);
 if(p[at]>1)return false;progress.hardwareLossCountKnown=p[at++]!=0;
 if(p[at]>1)return false;progress.sourceDigestKnown=p[at++]!=0;
 std::memcpy(progress.sourceDigest.data(),p+at,progress.sourceDigest.size());
 if(!validProgress(progress) || progress.sequence<=cursor.lastSequence || progress.writtenFrames<cursor.lastProgress.writtenFrames
  || progress.acceptedFrames<cursor.lastProgress.acceptedFrames || (terminal && std::uint8_t(data[4])>std::uint8_t(TakeTerminal::SealIntent)))return false;
 if(terminal && data[4]==std::uint8_t(TakeTerminal::Complete) && !validComplete(progress))return false;
 cursor.lastSequence=progress.sequence;cursor.lastProgress=progress;return true;
}
}
