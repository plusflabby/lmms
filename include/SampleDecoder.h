/*
 * SampleDecoder.h - Decodes audio files in various formats
 *
 * Copyright (c) 2023 saker <sakertooth@gmail.com>
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */

#ifndef LMMS_SAMPLE_DECODER_H
#define LMMS_SAMPLE_DECODER_H

#include <QString>
#include <QByteArray>

class QIODevice;
#include <optional>
#include <string>
#include <vector>

#include "SampleFrame.h"
#include "lmms_export.h"

namespace lmms {
class SampleDecoder
{
public:
	struct Result
	{
		std::vector<SampleFrame> data;
		int sampleRate;
	};

	// Narrow import admission; zero budgets fail closed. Legacy decode is unchanged.
	struct PcmWaveLimits
	{
		quint64 maxFileBytes = 0;
		quint64 maxDecodedBytes = 0;
	};
	struct PcmWaveInfo
	{
		quint64 frames = 0;
		quint64 fileBytes = 0;
		quint64 decodedBytes = 0;
		int channels = 0;
		int sampleRate = 0;
		QByteArray sourceSha256;
	};
	struct PcmWaveAdmission
	{
		QString code;
		PcmWaveInfo info;
	};
	struct PcmWaveResult
	{
		QString code;
		PcmWaveInfo info;
		std::optional<Result> sample;
	};
	// Source-sized allocation budget: decoded vector plus fixed input/header/digest payload.
	// Constant Qt/device/allocator bookkeeping is separate from these PCM buffer bytes.
	static constexpr quint64 PcmWaveScratchBytes = 4096 + 44 + 32;
	static LMMS_EXPORT PcmWaveLimits configuredImportLimits();
	static LMMS_EXPORT PcmWaveAdmission admitPcmWave(const QByteArray& header, quint64 fileBytes, const PcmWaveLimits& limits);
	static LMMS_EXPORT PcmWaveResult decodePcmWave(QIODevice& source, const PcmWaveLimits& limits);

	struct AudioType
	{
		std::string name;
		std::string extension;
	};

	static auto decode(const QString& audioFile) -> std::optional<Result>;
	static auto supportedAudioTypes() -> const std::vector<AudioType>&;
};
} // namespace lmms

#endif // LMMS_SAMPLE_DECODER_H
