/*
 * SampleClip.h
 *
 * Copyright (c) 2005-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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

#ifndef LMMS_SAMPLE_CLIP_H
#define LMMS_SAMPLE_CLIP_H

#include <QByteArray>

#include <memory>
#include "Clip.h"
#include "Sample.h"
#include "AudioSourceWindow.h"
#include "AudioImportPolicy.h"

namespace lmms
{

class SampleBuffer;
class SampleTrack;

namespace gui
{

class SampleClipView;

} // namespace gui


class LMMS_EXPORT SampleClip : public Clip
{
	Q_OBJECT
	Q_PROPERTY(QString audioWindowStatus READ audioWindowStatus NOTIFY audioWindowStatusChanged)
	mapPropertyFromModel(bool,isRecord,setRecord,m_recordModel);
public:
	SampleClip(Track* track, Sample sample, bool isPlaying);
	SampleClip(Track* track);
	~SampleClip() override;

	SampleClip& operator=( const SampleClip& that ) = delete;

	void changeLength( const TimePos & _length ) override;
	void updateLength() override;

	const QString& sampleFile() const;
	bool hasSampleFileLoaded(const QString & filename) const;
	const QString& sourceLoadError() const { return m_sourceLoadError; }
	bool usesBoundedSampleSource() const { return m_boundedSampleSource; }
	bool hasAudioSourceWindow() const { return m_audioWindowStorage.kind != AudioWindowStorageKind::Legacy; }
	const AudioWindowStorage& audioWindowStorage() const { return m_audioWindowStorage; }
	QString audioWindowStatus() const;
	QString adoptAuthorizedAudio(std::unique_ptr<AudioImportPolicy::Prepared> audio);
	const QByteArray& sourceFileIdentity() const { return m_boundedSourceIdentity; }
	const QByteArray& expectedSourceDigest() const { return m_boundedSourceDigest; }
	const std::optional<SampleDecoder::PcmWaveInfo>& authorizedSourceInfo() const { return m_sourceInfo; }

	void saveSettings( QDomDocument & _doc, QDomElement & _parent ) override;
	void loadSettings( const QDomElement & _this ) override;
	inline QString nodeName() const override
	{
		return "sampleclip";
	}

	Sample& sample()
	{
		return m_sample;
	}

	TimePos sampleLength() const;
	void setSampleStartFrame( f_cnt_t startFrame );
	void setSamplePlayLength( f_cnt_t length );
	void setStartTimeOffset(const TimePos& startTimeOffset) override;
	gui::ClipView * createView( gui::TrackView * _tv ) override;


	bool isPlaying() const;
	void setIsPlaying(bool isPlaying);
	void setSampleBuffer(std::shared_ptr<const SampleBuffer> sb);

	SampleClip* clone() override
	{
		return new SampleClip(*this);
	}
	virtual SampleClip* cloneToTrack(SampleTrack* target) const;

public slots:
	void setSampleFile(const QString& sf);
	void toggleRecord();
	void playbackPositionChanged();
	void updateTrackClips();
	void tempoChanged();

protected:
	SampleClip( const SampleClip& orig );
	SampleClip(const SampleClip& orig, Track* target);

private:
	void setSampleFileImpl(const QString& source, const QByteArray& expectedDigest, const QByteArray& expectedIdentity = {});
	Sample m_sample;
	// Preserve an intended file link when no file-backed buffer could be loaded.
	QString m_unavailableSampleFile;
	QString m_sourceLoadError;
	QByteArray m_boundedSourceDigest;
	QByteArray m_boundedSourceIdentity;
	bool m_invalidSourceIdentity = false;
	std::optional<SampleDecoder::PcmWaveInfo> m_sourceInfo;
	bool m_boundedSampleSource = false;
	AudioWindowStorage m_audioWindowStorage;
	std::shared_ptr<AudioWindowPlaybackGuard> m_audioWindowGuard = std::make_shared<AudioWindowPlaybackGuard>();
	BoolModel m_recordModel;
	bool m_isPlaying;
	int m_startFrameOffset;

	friend class gui::SampleClipView;


signals:
	void audioWindowStatusChanged(const QString& code);
	void sampleChanged();
	void wasReversed();
} ;


} // namespace lmms

#endif // LMMS_SAMPLE_CLIP_H
