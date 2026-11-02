#include <QtTest>
#include <QBuffer>
#include <QCryptographicHash>
#include <QtEndian>
#include <bit>
#include <cmath>
#include <limits>
#include "ManagedVocalWaveReader.h"

using namespace lmms;

class ManagedVocalWaveReaderTest : public QObject
{
	Q_OBJECT

	static QByteArray makeWave(const std::vector<float>& samples, int channels = 1, int rate = 48000, quint16 format = 3)
	{
		QByteArray data;
		data.reserve(int(samples.size() * sizeof(float)));
		for (const auto sample : samples)
		{
			const auto bits = qToLittleEndian(std::bit_cast<quint32>(sample));
			data.append(reinterpret_cast<const char*>(&bits), sizeof(bits));
		}
		QByteArray bytes;
		auto append32 = [&bytes](quint32 value) {
			const auto encoded = qToLittleEndian(value);
			bytes.append(reinterpret_cast<const char*>(&encoded), sizeof(encoded));
		};
		auto append16 = [&bytes](quint16 value) {
			const auto encoded = qToLittleEndian(value);
			bytes.append(reinterpret_cast<const char*>(&encoded), sizeof(encoded));
		};
		bytes.append("RIFF", 4);
		append32(quint32(48 + data.size()));
		bytes.append("WAVEfmt ", 8);
		append32(16);
		append16(format);
		append16(quint16(channels));
		append32(quint32(rate));
		append32(quint32(rate * channels * (format == 3 ? 4 : 2)));
		append16(quint16(channels * (format == 3 ? 4 : 2)));
		append16(quint16(format == 3 ? 32 : 16));
		bytes.append("fact", 4);
		append32(4);
		append32(quint32(samples.size() / std::size_t(channels)));
		bytes.append("data", 4);
		append32(quint32(data.size()));
		bytes.append(data);
		return bytes;
	}

	static ManagedVocalWaveLimits limits()
	{
		return {1024 * 1024, 1024 * 1024, 120 * 48000};
	}

private slots:
	void readsFloatFramesAndReportsExactPcmDigest()
	{
		const std::vector<float> samples{0.0f, 0.5f, -1.25f};
		auto bytes = makeWave(samples);
		QBuffer source(&bytes);
		QVERIFY(source.open(QIODevice::ReadOnly));
		const auto result = ManagedVocalWaveReader::read(source, limits());
		QCOMPARE(result.code, QStringLiteral("ok"));
		QCOMPARE(result.info.sampleRate, 48000);
		QCOMPARE(result.info.channels, 1);
		QCOMPARE(result.info.frames, quint64(3));
		QCOMPARE(result.info.pcmSha256,
			QString::fromLatin1(QCryptographicHash::hash(bytes.mid(56), QCryptographicHash::Sha256).toHex()));
		QCOMPARE(result.samples, samples);
		QCOMPARE(result.info.peak, 1.25);
		QVERIFY(result.info.overrange);
	}

	void handlesStereoAndBoundedScanWithoutRetainingSamples()
	{
		auto bytes = makeWave({0.1f, -0.2f, 0.3f, 0.4f}, 2, 44100);
		QBuffer source(&bytes);
		QVERIFY(source.open(QIODevice::ReadOnly));
		const auto result = ManagedVocalWaveReader::read(source, limits(), false);
		QCOMPARE(result.code, QStringLiteral("ok"));
		QCOMPARE(result.info.channels, 2);
		QCOMPARE(result.info.frames, quint64(2));
		QVERIFY(result.samples.empty());
	}

	void rejectsMalformedFormatNonfiniteAndResourceExcess()
	{
		auto bytes = makeWave({0.0f, std::numeric_limits<float>::quiet_NaN()});
		QBuffer source(&bytes);
		QVERIFY(source.open(QIODevice::ReadOnly));
		QCOMPARE(ManagedVocalWaveReader::read(source, limits()).code, QStringLiteral("invalid_audio_samples"));

		source.close();
		bytes = makeWave({0.0f, 0.5f}, 1, 48000, 1);
		source.setBuffer(&bytes);
		QVERIFY(source.open(QIODevice::ReadOnly));
		QCOMPARE(ManagedVocalWaveReader::read(source, limits()).code, QStringLiteral("unsupported_audio_format"));

		source.close();
		bytes = makeWave({0.0f, 0.5f});
		bytes.chop(1);
		source.setBuffer(&bytes);
		QVERIFY(source.open(QIODevice::ReadOnly));
		QCOMPARE(ManagedVocalWaveReader::read(source, limits()).code, QStringLiteral("invalid_audio_header"));

		source.close();
		bytes = makeWave({0.0f, 0.5f});
		source.setBuffer(&bytes);
		QVERIFY(source.open(QIODevice::ReadOnly));
		auto tooSmall = limits();
		tooSmall.maximumFileBytes = quint64(bytes.size() - 1);
		QCOMPARE(ManagedVocalWaveReader::read(source, tooSmall).code, QStringLiteral("resource_limit"));
	}
};

QTEST_APPLESS_MAIN(ManagedVocalWaveReaderTest)
#include "ManagedVocalWaveReaderTest.moc"
