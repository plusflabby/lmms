#include "RecordingPcm16.h"
#include <QtTest>
#include <algorithm>
#include <array>
#include <cfenv>
#include <limits>
using namespace lmms::recording;
class RecordingPcm16Test : public QObject
{
 Q_OBJECT
private slots:
 void finiteExtremesUseSaturationAndTiesAwayFromZero()
 {
  constexpr float halfLsb=0.5f/32768.f;
  const std::array<float,9> source{-1.f,-1.1f,-halfLsb,0.f,halfLsb,1.f,1.1f,.9999847412109375f,-.999969482421875f};
  std::array<std::int16_t,source.size()> converted{};const auto stats=RecordingPcm16::convert(source.data(),converted.data(),source.size());
  QVERIFY(stats.valid);QCOMPARE(stats.invalid,0u);QCOMPARE(stats.clipped,2u);
  const std::array<std::int16_t,source.size()> expected{-32768,-32768,-1,0,1,32767,32767,32767,-32767};
  QCOMPARE(converted,expected);
 }
 void conversionIsIndependentOfFloatingPointRoundingMode()
 {
  const std::array<float,3> source{0.5f/32768.f,1.5f/32768.f,-1.5f/32768.f};std::array<std::int16_t,3> out{};
  for(const auto mode:{FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO}){const auto previous=std::fegetround();std::fesetround(mode);const auto result=RecordingPcm16::convert(source.data(),out.data(),source.size());std::fesetround(previous);QVERIFY(result.valid);QCOMPARE(out,(std::array<std::int16_t,3>{1,2,-2}));}
 }
 void nonFiniteSamplesRejectWholeBlockWithoutReplacingData()
 {
  const auto nan=std::numeric_limits<float>::quiet_NaN(),inf=std::numeric_limits<float>::infinity();
  for(const auto invalid:{nan,inf,-inf}){const std::array<float,3> source{.5f,invalid,-.5f};std::array<std::int16_t,3> out{11,12,13};const auto result=RecordingPcm16::convert(source.data(),out.data(),source.size());QVERIFY(!result.valid);QCOMPARE(result.invalid,1u);QCOMPARE(result.convertedFrames,0u);QCOMPARE(out,(std::array<std::int16_t,3>{11,12,13}));}
 }
 void pcmWordsEncodeExplicitLittleEndian()
 {
  const std::array<std::int16_t,5> source{-32768,-1,0,1,32767};std::array<std::uint8_t,10> out{};
  QVERIFY(RecordingPcm16::encodeLittleEndian(source.data(),out.data(),source.size()));
  const std::array<std::uint8_t,10> expected{0x00,0x80,0xff,0xff,0x00,0x00,0x01,0x00,0xff,0x7f};QCOMPARE(out,expected);
 }
 void invalidCountsAndPointersRejectWithoutOverflow()
 {
  float input=0;std::int16_t output=55;
  QVERIFY(!RecordingPcm16::convert(nullptr,&output,1).valid);QVERIFY(!RecordingPcm16::convert(&input,nullptr,1).valid);
  QVERIFY(!RecordingPcm16::convert(&input,&output,0).valid);
  std::array<std::uint8_t,2> bytes{};QVERIFY(!RecordingPcm16::encodeLittleEndian(nullptr,bytes.data(),1));
  QVERIFY(!RecordingPcm16::encodeLittleEndian(&output,nullptr,1));QVERIFY(!RecordingPcm16::encodeLittleEndian(&output,bytes.data(),std::numeric_limits<size_t>::max()));
 }
};
QTEST_APPLESS_MAIN(RecordingPcm16Test)
#include "RecordingPcm16Test.moc"
