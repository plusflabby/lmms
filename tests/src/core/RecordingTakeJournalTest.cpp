#include "RecordingTakeJournal.h"
#include <QtTest>
#include <limits>
using namespace lmms::recording;
class RecordingTakeJournalTest : public QObject
{
 Q_OBJECT
 static TakeDescriptor descriptor()
 { TakeDescriptor d;d.projectId=QStringLiteral("00000000-0000-4000-8000-000000000001");d.takeId=QStringLiteral("00000000-0000-4000-8000-000000000002");
  d.label=QStringLiteral("Verse Take 1");d.epoch={7,3};d.projectGeneration=7;d.maxRawFrames=5760000;d.targetTick=18432;d.sampleRate=48000;d.countInBars=2;return d; }
 static TakeProgress progress(std::uint64_t seq=1,std::uint64_t frames=48000)
 {
  TakeProgress p;p.sequence=seq;p.receivedFrames=frames;p.acceptedFrames=frames;p.writtenFrames=frames;
  p.timestampQuality=InputTimestampQuality::Unavailable;p.sourceDigestKnown=true;p.sourceDigest.fill(0x5a);return p;
 }
private slots:
 void headerRoundTripsBoundedTakeIdentity()
 {
  const auto source=descriptor();const auto bytes=RecordingTakeJournalCodec::encodeHeader(source);
  QVERIFY(!bytes.isEmpty());QVERIFY(bytes.size()<=1024);TakeDescriptor decoded;
  QVERIFY(RecordingTakeJournalCodec::validateHeader(bytes,decoded));
  QCOMPARE(decoded.projectId,source.projectId);QCOMPARE(decoded.takeId,source.takeId);QCOMPARE(decoded.label,source.label);
  QCOMPARE(decoded.projectGeneration,quint64(7));QCOMPARE(decoded.sampleRate,48000u);QCOMPARE(decoded.maxRawFrames,quint64(5760000));
 }
 void malformedOrOversizedHeaderIsRefused()
 {
  auto invalid=descriptor();invalid.sampleRate=32000;QVERIFY(RecordingTakeJournalCodec::encodeHeader(invalid).isEmpty());
  invalid=descriptor();invalid.maxRawFrames=std::numeric_limits<std::uint64_t>::max();QVERIFY(RecordingTakeJournalCodec::encodeHeader(invalid).isEmpty());
  auto id=descriptor();id.takeId=QStringLiteral("../../take.wav");QVERIFY(RecordingTakeJournalCodec::encodeHeader(id).isEmpty());
  auto oversized=descriptor();oversized.label=QString(10000,'x');QVERIFY(RecordingTakeJournalCodec::encodeHeader(oversized).isEmpty());
  QVERIFY(!RecordingTakeJournalCodec::validateHeader(QByteArray(65537,'x'),invalid));
 }
 void progressRecordHasStableSequenceCountsAndChecksum()
 {
  const auto input=progress(3,123456);const auto encoded=RecordingTakeJournalCodec::encodeProgress(input);
  QVERIFY(encoded.valid);QVERIFY(encoded.size<=TakeJournalRecordMaximum);TakeJournalCursor cursor;
  QVERIFY(RecordingTakeJournalCodec::validateRecord(encoded.bytes.data(),encoded.size,cursor));
  QCOMPARE(cursor.lastSequence,quint64(3));QCOMPARE(cursor.lastProgress.receivedFrames,quint64(123456));
  QCOMPARE(cursor.lastProgress.writtenFrames,quint64(123456));QCOMPARE(cursor.lastProgress.timestampQuality,InputTimestampQuality::Unavailable);
 }
 void corruptAndTornRecordsNeverAdvanceCheckpoint()
 {
  const auto encoded=RecordingTakeJournalCodec::encodeProgress(progress());QVERIFY(encoded.valid);
  TakeJournalCursor cursor;auto damaged=encoded.bytes;damaged[encoded.size/2]^=0x40;
  QVERIFY(!RecordingTakeJournalCodec::validateRecord(damaged.data(),encoded.size,cursor));QCOMPARE(cursor.lastSequence,quint64(0));
  QVERIFY(!RecordingTakeJournalCodec::validateRecord(encoded.bytes.data(),encoded.size-1,cursor));QCOMPARE(cursor.lastSequence,quint64(0));
 }
 void reorderedOrNonmonotonicCheckpointIsRefused()
 {
  auto first=RecordingTakeJournalCodec::encodeProgress(progress(1,100));auto second=RecordingTakeJournalCodec::encodeProgress(progress(2,50));
  QVERIFY(first.valid);QVERIFY(second.valid);TakeJournalCursor cursor;
  QVERIFY(RecordingTakeJournalCodec::validateRecord(first.bytes.data(),first.size,cursor));
  QVERIFY(!RecordingTakeJournalCodec::validateRecord(second.bytes.data(),second.size,cursor));QCOMPARE(cursor.lastSequence,quint64(1));
  QVERIFY(!RecordingTakeJournalCodec::validateRecord(first.bytes.data(),first.size,cursor));
 }
 void zeroSequenceAndCounterOverflowRefused()
 {
  QVERIFY(!RecordingTakeJournalCodec::encodeProgress(progress(0,1)).valid);
  auto bad=progress(2);bad.acceptedFrames=3;bad.writtenFrames=4;QVERIFY(!RecordingTakeJournalCodec::encodeProgress(bad).valid);
  bad=progress(std::numeric_limits<std::uint64_t>::max(),1);QVERIFY(!RecordingTakeJournalCodec::encodeProgress(bad).valid);
 }
 void terminalRecordsCannotClaimUnwrittenOrFaultedTakesComplete()
 {
  auto valid=progress(2,48000);QVERIFY(RecordingTakeJournalCodec::encodeTerminal(TakeTerminal::Complete,valid).valid);
  valid.writtenFrames=47999;QVERIFY(!RecordingTakeJournalCodec::encodeTerminal(TakeTerminal::Complete,valid).valid);
  valid=progress(2,48000);valid.fault=InputFault::HardwareDiscontinuity;
  QVERIFY(!RecordingTakeJournalCodec::encodeTerminal(TakeTerminal::Complete,valid).valid);
  QVERIFY(RecordingTakeJournalCodec::encodeTerminal(TakeTerminal::Incomplete,valid).valid);
 }
 void fullTakeJournalRemainsBelowTheHardByteLimit()
 {
  std::uint64_t total=std::uint64_t(RecordingTakeJournalCodec::encodeHeader(descriptor()).size());
  TakeJournalCursor cursor;
  for(std::uint64_t sequence=1;sequence<=120;++sequence)
  {
   auto record=RecordingTakeJournalCodec::encodeProgress(progress(sequence,sequence*48000));QVERIFY(record.valid);
   total+=record.size;QVERIFY(total<=65536);QVERIFY(RecordingTakeJournalCodec::validateRecord(record.bytes.data(),record.size,cursor));
  }
  auto sealing=progress(121,5760000);auto intent=RecordingTakeJournalCodec::encodeSealIntent(sealing);QVERIFY(intent.valid);
  total+=intent.size;QVERIFY(total<=65536);QVERIFY(RecordingTakeJournalCodec::validateRecord(intent.bytes.data(),intent.size,cursor));
  auto terminal=progress(122,5760000);auto complete=RecordingTakeJournalCodec::encodeTerminal(TakeTerminal::Complete,terminal);QVERIFY(complete.valid);
  total+=complete.size;QVERIFY(total<=65536);QVERIFY(RecordingTakeJournalCodec::validateRecord(complete.bytes.data(),complete.size,cursor));
 }
};
QTEST_GUILESS_MAIN(RecordingTakeJournalTest)
#include "RecordingTakeJournalTest.moc"
