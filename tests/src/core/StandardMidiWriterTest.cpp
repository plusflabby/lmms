#include <QtTest/QtTest>
#include <QBuffer>
#include <QVector>

#include "StandardMidiWriter.h"

class StandardMidiWriterTest : public QObject
{
	Q_OBJECT

private slots:
	void writesLargeFormatOneTrackWithoutTruncation();
	void rejectsUnrepresentableDeltaBeforeWriting();
	void ordersNoteOffBeforeNoteOnAtTheSameTick();
	void detectsShortDestinationWrites();
	void encodesConductorAndChannelMetadata();
	void acceptsValidTempoValuesWhoseMostSignificantByteIsZero();
};

void StandardMidiWriterTest::writesLargeFormatOneTrackWithoutTruncation()
{
	lmms::MidiExportPlan plan;
	plan.ppq = 480;
	plan.tracks.append({QStringLiteral("Piano"), {
		{0, lmms::MidiExportEventType::NoteOn, 0, 60, 90},
		{600000, lmms::MidiExportEventType::NoteOff, 0, 60, 0}
	}, 1200000});
	for (int i = 0; i < 10000; ++i)
	{
		const auto tick = quint64(i) * 120;
		plan.tracks[0].events.append({tick, lmms::MidiExportEventType::NoteOn, 0,
			static_cast<quint8>(48 + i % 36), 80});
		plan.tracks[0].events.append({tick + 60, lmms::MidiExportEventType::NoteOff, 0,
			static_cast<quint8>(48 + i % 36), 0});
	}
	QByteArray bytes;
	QBuffer buffer(&bytes);
	QVERIFY(buffer.open(QIODevice::WriteOnly));
	const auto result = lmms::writeStandardMidi(plan, buffer);
	QVERIFY2(result.success, qPrintable(result.code));
	QVERIFY(bytes.size() > 50 * 1024);
	QCOMPARE(bytes.left(4), QByteArrayLiteral("MThd"));
	QCOMPARE(bytes.mid(8, 2), QByteArray::fromHex("0001"));
	QCOMPARE(bytes.mid(12, 2), QByteArray::fromHex("01e0"));
}

void StandardMidiWriterTest::rejectsUnrepresentableDeltaBeforeWriting()
{
	lmms::MidiExportPlan plan;
	plan.tracks.append({QStringLiteral("Piano"), {
		{0x10000000, lmms::MidiExportEventType::NoteOn, 0, 60, 90}
	}, 0x10000000});
	QByteArray bytes;
	QBuffer buffer(&bytes);
	QVERIFY(buffer.open(QIODevice::WriteOnly));
	const auto result = lmms::writeStandardMidi(plan, buffer);
	QVERIFY(!result.success);
	QCOMPARE(result.code, QStringLiteral("invalid_event_or_delta"));
	QVERIFY(bytes.isEmpty());
}

void StandardMidiWriterTest::ordersNoteOffBeforeNoteOnAtTheSameTick()
{
	lmms::MidiExportPlan plan;
	plan.tracks.append({QStringLiteral("Piano"), {
		{10, lmms::MidiExportEventType::NoteOn, 0, 60, 90},
		{20, lmms::MidiExportEventType::NoteOn, 0, 60, 80},
		{20, lmms::MidiExportEventType::NoteOff, 0, 60, 0}
	}, 20});
	QByteArray bytes;
	QBuffer buffer(&bytes);
	QVERIFY(buffer.open(QIODevice::WriteOnly));
	QVERIFY(lmms::writeStandardMidi(plan, buffer).success);
	const auto noteOff = bytes.indexOf(QByteArray::fromHex("0a803c00"));
	const auto noteOn = bytes.indexOf(QByteArray::fromHex("00903c50"));
	QVERIFY(noteOff >= 0);
	QVERIFY(noteOn > noteOff);
}

void StandardMidiWriterTest::detectsShortDestinationWrites()
{
	class ShortDevice final : public QIODevice
	{
	public:
		bool isSequential() const override { return true; }
		QByteArray bytes;
	protected:
		qint64 readData(char*, qint64) override { return -1; }
		qint64 writeData(const char* data, qint64 length) override
		{
			const auto count = qMin<qint64>(length, 8);
			bytes.append(data, static_cast<int>(count));
			return count;
		}
	};

	lmms::MidiExportPlan plan;
	plan.tracks.append({QStringLiteral("Piano"), {}, 0});
	ShortDevice device;
	QVERIFY(device.open(QIODevice::WriteOnly));
	const auto result = lmms::writeStandardMidi(plan, device);
	QVERIFY(result.success);
	QCOMPARE(result.bytesWritten, device.bytes.size());
	QVERIFY(device.bytes.size() > 8);
}

void StandardMidiWriterTest::encodesConductorAndChannelMetadata()
{
	lmms::MidiExportPlan plan;
	plan.tracks.append({QStringLiteral("Conductor"), {
		{0,lmms::MidiExportEventType::Tempo,0,0x07,0xa1,0x20},
		{0,lmms::MidiExportEventType::TimeSignature,0,4,2,24}
	},0});
	plan.tracks.append({QStringLiteral("Synth"), {
		{0,lmms::MidiExportEventType::ProgramChange,2,9},
		{0,lmms::MidiExportEventType::ControlChange,2,64,127},
		{0,lmms::MidiExportEventType::PitchBend,2,0,64},
		{0,lmms::MidiExportEventType::NoteOn,2,60,100},
		{120,lmms::MidiExportEventType::NoteOff,2,60,0}
	},120});
	QByteArray bytes;QBuffer buffer(&bytes);QVERIFY(buffer.open(QIODevice::WriteOnly));
	QVERIFY(lmms::writeStandardMidi(plan,buffer).success);
	QVERIFY(bytes.contains(QByteArray::fromHex("00ff510307a120")));
	QVERIFY(bytes.contains(QByteArray::fromHex("00ff580404021808")));
	QVERIFY(bytes.contains(QByteArray::fromHex("00c20900b2407f00e2004000923c64")));
}

void StandardMidiWriterTest::acceptsValidTempoValuesWhoseMostSignificantByteIsZero()
{
	lmms::MidiExportPlan plan;
	plan.tracks.append({QStringLiteral("Conductor"),{{0,lmms::MidiExportEventType::Tempo,0,0x00,0xea,0x9c}},0});
	QByteArray bytes;QBuffer buffer(&bytes);QVERIFY(buffer.open(QIODevice::WriteOnly));
	const auto result=lmms::writeStandardMidi(plan,buffer);
	QVERIFY2(result.success,qPrintable(result.code));
	QVERIFY(bytes.contains(QByteArray::fromHex("00ff510300ea9c")));
}

QTEST_APPLESS_MAIN(StandardMidiWriterTest)
#include "StandardMidiWriterTest.moc"
