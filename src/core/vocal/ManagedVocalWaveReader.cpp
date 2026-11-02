#include "ManagedVocalWaveReader.h"

#include <QCryptographicHash>
#include <QtEndian>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <new>

namespace lmms {
namespace {

quint16 readLe16(const char* bytes)
{
	return qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(bytes));
}

quint32 readLe32(const char* bytes)
{
	return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(bytes));
}

bool readExact(QIODevice& source, char* destination, qint64 length)
{
	qint64 total = 0;
	while (total < length)
	{
		const auto count = source.read(destination + total, length - total);
		if (count <= 0)
		{
			return false;
		}
		total += count;
	}
	return true;
}

ManagedVocalWaveReadResult failure(const QString& code)
{
	return {code, {}, {}};
}

} // namespace

ManagedVocalWaveReadResult ManagedVocalWaveReader::read(
	QIODevice& source,
	const ManagedVocalWaveLimits& limits,
	bool retainSamples)
{
	if (!source.isOpen() || !source.isReadable() || source.isSequential() || !limits.maximumFileBytes
		|| !limits.maximumDecodedBytes || !limits.maximumFrames)
	{
		return failure(QStringLiteral("invalid_source"));
	}
	const auto fileBytes = source.size();
	if (fileBytes < 56)
	{
		return failure(QStringLiteral("invalid_audio_header"));
	}
	if (quint64(fileBytes) > limits.maximumFileBytes)
	{
		return failure(QStringLiteral("resource_limit"));
	}
	if (!source.seek(0))
	{
		return failure(QStringLiteral("invalid_source"));
	}

	char riff[12]{};
	if (!readExact(source, riff, sizeof(riff)) || QByteArray(riff, 4) != QByteArrayLiteral("RIFF")
		|| QByteArray(riff + 8, 4) != QByteArrayLiteral("WAVE"))
	{
		return failure(QStringLiteral("invalid_audio_header"));
	}
	const quint64 riffEnd = quint64(readLe32(riff + 4)) + 8;
	if (riffEnd != quint64(fileBytes) || riffEnd < sizeof(riff))
	{
		return failure(QStringLiteral("invalid_audio_header"));
	}

	bool foundFormat = false;
	bool foundFact = false;
	bool foundData = false;
	quint16 format = 0;
	quint16 channels = 0;
	quint16 bitsPerSample = 0;
	quint16 blockAlign = 0;
	quint32 sampleRate = 0;
	quint32 byteRate = 0;
	quint32 factFrames = 0;
	quint64 dataOffset = 0;
	quint64 dataBytes = 0;
	quint64 offset = sizeof(riff);
	while (offset < riffEnd)
	{
		if (riffEnd - offset < 8)
		{
			return failure(QStringLiteral("invalid_audio_header"));
		}
		char chunkHeader[8]{};
		if (!readExact(source, chunkHeader, sizeof(chunkHeader)))
		{
			return failure(QStringLiteral("invalid_audio_header"));
		}
		offset += sizeof(chunkHeader);
		const auto chunkBytes = quint64(readLe32(chunkHeader + 4));
		if (chunkBytes > riffEnd - offset)
		{
			return failure(QStringLiteral("invalid_audio_header"));
		}
		const auto payloadEnd = offset + chunkBytes;
		const auto paddedEnd = payloadEnd + (chunkBytes & 1);
		if (paddedEnd > riffEnd)
		{
			return failure(QStringLiteral("invalid_audio_header"));
		}
		const QByteArray tag(chunkHeader, 4);
		if (tag == QByteArrayLiteral("fmt "))
		{
			if (foundFormat || chunkBytes < 16 || chunkBytes > 4096)
			{
				return failure(QStringLiteral("invalid_audio_header"));
			}
			char formatBytes[16]{};
			if (!readExact(source, formatBytes, sizeof(formatBytes)))
			{
				return failure(QStringLiteral("invalid_audio_header"));
			}
			format = readLe16(formatBytes);
			channels = readLe16(formatBytes + 2);
			sampleRate = readLe32(formatBytes + 4);
			byteRate = readLe32(formatBytes + 8);
			blockAlign = readLe16(formatBytes + 12);
			bitsPerSample = readLe16(formatBytes + 14);
			foundFormat = true;
		}
		else if (tag == QByteArrayLiteral("fact"))
		{
			if (foundFact || chunkBytes < 4 || chunkBytes > 4096)
			{
				return failure(QStringLiteral("invalid_audio_header"));
			}
			char factBytes[4]{};
			if (!readExact(source, factBytes, sizeof(factBytes)))
			{
				return failure(QStringLiteral("invalid_audio_header"));
			}
			factFrames = readLe32(factBytes);
			foundFact = true;
		}
		else if (tag == QByteArrayLiteral("data"))
		{
			if (foundData || !foundFormat || paddedEnd != riffEnd)
			{
				return failure(QStringLiteral("invalid_audio_header"));
			}
			dataOffset = offset;
			dataBytes = chunkBytes;
			foundData = true;
		}
		if (tag != QByteArrayLiteral("data") && !source.seek(qint64(paddedEnd)))
		{
			return failure(QStringLiteral("invalid_audio_header"));
		}
		offset = paddedEnd;
	}

	if (!foundFormat || !foundData)
	{
		return failure(QStringLiteral("invalid_audio_header"));
	}
	if (format != 3 || (channels != 1 && channels != 2)
		|| (sampleRate != 44100 && sampleRate != 48000) || bitsPerSample != 32)
	{
		return failure(QStringLiteral("unsupported_audio_format"));
	}
	if (blockAlign != quint16(channels * sizeof(float)) || byteRate != sampleRate * quint32(blockAlign))
	{
		return failure(QStringLiteral("invalid_audio_header"));
	}
	if (!dataBytes || dataBytes % blockAlign)
	{
		return failure(QStringLiteral("invalid_audio_header"));
	}
	const auto frames = dataBytes / blockAlign;
	if (frames > limits.maximumFrames || (foundFact && factFrames != frames))
	{
		return failure(frames > limits.maximumFrames ? QStringLiteral("resource_limit")
			: QStringLiteral("invalid_audio_header"));
	}
	if (frames > std::numeric_limits<quint64>::max() / quint64(channels * sizeof(float)))
	{
		return failure(QStringLiteral("resource_limit"));
	}
	const quint64 decodedBytes = frames * quint64(channels * sizeof(float));
	const quint64 sampleCount = frames * quint64(channels);
	if (decodedBytes > limits.maximumDecodedBytes || sampleCount > std::numeric_limits<size_t>::max())
	{
		return failure(QStringLiteral("resource_limit"));
	}

	ManagedVocalWaveReadResult result;
	result.code = QStringLiteral("ok");
	result.info.frames = frames;
	result.info.dataBytes = dataBytes;
	result.info.sampleRate = int(sampleRate);
	result.info.channels = int(channels);
	if (retainSamples)
	{
		try
		{
			result.samples.reserve(size_t(sampleCount));
		}
		catch (const std::bad_alloc&)
		{
			return failure(QStringLiteral("resource_limit"));
		}
	}

	if (!source.seek(qint64(dataOffset)))
	{
		return failure(QStringLiteral("invalid_source"));
	}
	QCryptographicHash digest(QCryptographicHash::Sha256);
	const quint64 blockBytes = (65536 / blockAlign) * blockAlign;
	QByteArray block(int(blockBytes), Qt::Uninitialized);
	quint64 remaining = dataBytes;
	while (remaining)
	{
		const auto count = int(std::min(remaining, blockBytes));
		if (!readExact(source, block.data(), count))
		{
			return failure(QStringLiteral("invalid_audio_header"));
		}
		digest.addData(block.constData(), count);
		for (int at = 0; at < count; at += int(sizeof(float)))
		{
			const auto bits = qFromLittleEndian<quint32>(
				reinterpret_cast<const uchar*>(block.constData() + at));
			const auto sample = std::bit_cast<float>(bits);
			if (!std::isfinite(sample))
			{
				return failure(QStringLiteral("invalid_audio_samples"));
			}
			const auto magnitude = std::abs(double(sample));
			result.info.peak = std::max(result.info.peak, magnitude);
			result.info.overrange = result.info.overrange || magnitude > 1.0;
			if (retainSamples)
			{
				result.samples.push_back(sample);
			}
		}
		remaining -= quint64(count);
	}
	result.info.pcmSha256 = QString::fromLatin1(digest.result().toHex());
	return result;
}

} // namespace lmms
