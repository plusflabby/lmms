#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QBuffer>
#include <QtEndian>

#include "SampleDecoder.h"

#include <cstring>

class SampleDecoderAdmissionTest : public QObject
{
	Q_OBJECT
	using Decoder = lmms::SampleDecoder;
	QByteArray header(quint32 frames = 337, quint16 channels = 2)
	{
		QByteArray bytes(44, 0);
		std::memcpy(bytes.data(), "RIFF", 4);
		std::memcpy(bytes.data()+8, "WAVEfmt ", 8);
		auto put16=[&](int pos,quint16 v){qToLittleEndian(v,bytes.data()+pos);};
		auto put32=[&](int pos,quint32 v){qToLittleEndian(v,bytes.data()+pos);};
		put32(4,36+frames*channels*2);put32(16,16);put16(20,1);put16(22,channels);
		put32(24,44100);put32(28,44100*channels*2);put16(32,channels*2);put16(34,16);
		std::memcpy(bytes.data()+36,"data",4);put32(40,frames*channels*2);
		return bytes;
	}
	Decoder::PcmWaveLimits limits() { return {20000, 20000}; }
private slots:
	void admitsExactNativeAllocationBoundary()
	{
		auto budget=limits();
		const quint64 expected=337*sizeof(lmms::SampleFrame)+Decoder::PcmWaveScratchBytes;
		budget.maxDecodedBytes=expected;
		const auto result=Decoder::admitPcmWave(header(),1392,budget);
		QCOMPARE(result.code,QStringLiteral("ok"));
		QCOMPARE(result.info.frames,quint64(337));
		QCOMPARE(result.info.channels,2);
		QCOMPARE(result.info.sampleRate,44100);
		QCOMPARE(result.info.decodedBytes,expected);
		--budget.maxDecodedBytes;
		QCOMPARE(Decoder::admitPcmWave(header(),1392,budget).code,QStringLiteral("source_too_large"));
	}
	void rejectsMissingAndFileBudgets()
	{
		QCOMPARE(Decoder::admitPcmWave(header(),1392,{}).code,QStringLiteral("audio_resource_limits_not_configured"));
		auto budget=limits();budget.maxFileBytes=1391;
		QCOMPARE(Decoder::admitPcmWave(header(),1392,budget).code,QStringLiteral("source_too_large"));
	}
	void rejectsHugeMetadataBeforeDecodeAllocation()
	{
		auto bytes=header(100000000,2);
		auto budget=limits();budget.maxFileBytes=400000044;
		QCOMPARE(Decoder::admitPcmWave(bytes,400000044,budget).code,QStringLiteral("source_too_large"));
	}
	void rejectsMalformedAndUnsupportedHeaders_data()
	{
		QTest::addColumn<QByteArray>("bytes");QTest::addColumn<quint64>("size");QTest::addColumn<QString>("code");
		auto bytes=header();bytes[0]='X';
		QTest::newRow("container")<<bytes<<quint64(1392)<<QStringLiteral("unsupported_audio_format");
		bytes=header();qToLittleEndian<quint16>(3,bytes.data()+20);
		QTest::newRow("float-codec")<<bytes<<quint64(1392)<<QStringLiteral("unsupported_audio_format");
		bytes=header();qToLittleEndian<quint16>(3,bytes.data()+22);
		QTest::newRow("multichannel")<<bytes<<quint64(1392)<<QStringLiteral("unsupported_audio_format");
		bytes=header();qToLittleEndian<quint16>(24,bytes.data()+34);
		QTest::newRow("24bit")<<bytes<<quint64(1392)<<QStringLiteral("unsupported_audio_format");
		bytes=header();qToLittleEndian<quint32>(0,bytes.data()+24);
		QTest::newRow("zero-rate")<<bytes<<quint64(1392)<<QStringLiteral("audio_decode_failed");
		bytes=header();qToLittleEndian<quint16>(2,bytes.data()+32);
		QTest::newRow("bad-alignment")<<bytes<<quint64(1392)<<QStringLiteral("audio_decode_failed");
		bytes=header();qToLittleEndian<quint32>(1,bytes.data()+28);
		QTest::newRow("bad-byte-rate")<<bytes<<quint64(1392)<<QStringLiteral("audio_decode_failed");
		bytes=header();qToLittleEndian<quint32>(1347,bytes.data()+40);
		QTest::newRow("partial-frame")<<bytes<<quint64(1391)<<QStringLiteral("audio_decode_failed");
		bytes=header();bytes.replace(36,4,"JUNK");
		QTest::newRow("extra-metadata-chunk")<<bytes<<quint64(1392)<<QStringLiteral("unsupported_audio_format");
		QTest::newRow("truncated-file")<<header()<<quint64(1391)<<QStringLiteral("audio_decode_failed");
		QTest::newRow("truncated-header")<<header().left(20)<<quint64(1392)<<QStringLiteral("audio_decode_failed");
		QTest::newRow("zero-frames")<<header(0)<<quint64(44)<<QStringLiteral("audio_decode_failed");
	}
	void rejectsMalformedAndUnsupportedHeaders()
	{
		QFETCH(QByteArray,bytes);QFETCH(quint64,size);QFETCH(QString,code);
		QCOMPARE(Decoder::admitPcmWave(bytes,size,limits()).code,code);
	}
	void decodesExistingFixtureReadOnlyAndPreservesBytes()
	{
		QFile file(QFINDTESTDATA("../../../data/samples/shapes/vowel_u.wav"));
		QVERIFY(file.open(QIODevice::ReadOnly));
		const auto hash=QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256);
		const auto result=Decoder::decodePcmWave(file,limits());
		QCOMPARE(result.code,QStringLiteral("ok"));
		QVERIFY(result.sample.has_value());
		QCOMPARE(result.sample->data.size(),size_t(337));
		QCOMPARE(result.sample->sampleRate,44100);
		QCOMPARE(result.info.channels,2);
		QVERIFY(file.seek(0));
		QCOMPARE(QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256),hash);
	}
	void decodesMonoSamplesExactlyWithoutTemporaryFiles()
	{
		auto bytes = header(2, 1);
		bytes.append(4, char(0));
		qToLittleEndian<qint16>(-32768, bytes.data() + 44);
		qToLittleEndian<qint16>(32767, bytes.data() + 46);
		QBuffer source(&bytes);
		QVERIFY(source.open(QIODevice::ReadOnly));
		const auto result = Decoder::decodePcmWave(source, limits());
		QCOMPARE(result.code, QStringLiteral("ok"));
		QVERIFY(result.sample.has_value());
		QCOMPARE(result.sample->data.size(), size_t(2));
		QCOMPARE(result.sample->data[0][0], -1.0f);
		QCOMPARE(result.sample->data[0][1], -1.0f);
		QCOMPARE(result.sample->data[1][0], 32767 / 32768.0f);
		QCOMPARE(result.sample->data[1][1], result.sample->data[1][0]);
	}
	void rejectsUnexpectedShortReadAfterAdmission()
	{
		class ShortSource : public QBuffer
		{
		public:
			using QBuffer::QBuffer;
			qint64 size() const override { return 52; }
		};
		auto bytes = header(4, 1);
		bytes.append(2, char(0));
		ShortSource source(&bytes);
		QVERIFY(source.open(QIODevice::ReadOnly));
		const auto result = Decoder::decodePcmWave(source, limits());
		QCOMPARE(result.code, QStringLiteral("audio_decode_failed"));
		QVERIFY(!result.sample.has_value());
	}
	void rejectsUnopenedSource()
	{
		QFile file;
		const auto result=Decoder::decodePcmWave(file,limits());
		QCOMPARE(result.code,QStringLiteral("audio_decode_failed"));
		QVERIFY(!result.sample.has_value());
	}
};
QTEST_MAIN(SampleDecoderAdmissionTest)
#include "SampleDecoderAdmissionTest.moc"
