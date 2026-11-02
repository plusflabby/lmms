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
#include <QTimer>

#include <atomic>
#include <memory>
#include "Clip.h"
#include "Sample.h"
#include "AudioSourceWindow.h"
#include "AudioImportPolicy.h"
#include "RecordedTakeSource.h"
#include "src/core/vocal/ManagedVocalAssetTypes.h"
#include "src/core/vocal/ManagedVocalPlayback.h"

namespace lmms
{

namespace recording { class RecordingAttachment; }

class SampleBuffer;
class SampleTrack;
class ManagedVocalPlaybackLoadTicket;

namespace gui
{

class SampleClipView;

} // namespace gui


class LMMS_EXPORT SampleClip : public Clip
{
	Q_OBJECT
	Q_PROPERTY(QString audioWindowStatus READ audioWindowStatus NOTIFY audioWindowStatusChanged)
	Q_PROPERTY(QString managedVocalVariantStatus READ managedVocalVariantStatus NOTIFY managedVocalVariantStatusChanged)
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
	bool isRecordedTake() const { return m_recordedTakeMetadataPresent; }
	bool recordedTakeSourceCurrent() const;
	const std::optional<recording::RecordedTakeReference>& recordedTakeReference() const { return m_recordedTakeReference; }
	bool hasAudioSourceWindow() const { return m_audioWindowStorage.kind != AudioWindowStorageKind::Legacy; }
	const AudioWindowStorage& audioWindowStorage() const { return m_audioWindowStorage; }
	QString audioWindowStatus() const;
	QString applyAudioWindow(const AudioSourceWindowV2&, const AudioWindowGeometry&);
	std::shared_ptr<const struct AudioWindowPlaybackView> audioWindowPlaybackView() const;
	bool hasManagedVocalVariant() const
	{
		return m_managedVocalAssociation.has_value() || !m_unavailableManagedVocalVariants.isNull();
	}
	QString managedVocalVariantStatus() const { return m_managedVocalVariantStatus; }
	QString managedVocalVariantAssetId() const;
	QByteArray managedVocalVariantStateDigest() const;
	const std::optional<ManagedVocalVariantAssociation>& managedVocalVariantAssociation() const
	{
		return m_managedVocalAssociation;
	}
	std::shared_ptr<const ManagedVocalPlaybackView> managedVocalPlaybackView() const;
	QString preloadManagedVocalVariant(const ManagedVocalVariantReference& reference);
	QString applyManagedVocalVariantAssociation(const ManagedVocalVariantAssociation& association);
	QString revertManagedVocalVariant();
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
	bool allowsGenericClipEdits() const override
	{
		return (!hasAudioSourceWindow() || m_loadingAudioWindow)
			&& (!hasManagedVocalVariant() || m_loadingManagedVocalAssociation);
	}
	SampleClip( const SampleClip& orig );
	SampleClip(const SampleClip& orig, Track* target);

private:
	friend class recording::RecordingAttachment;
	void initializeAudioWindowConnections();
	void refreshAudioWindowPlayback();
	void initializeManagedVocalConnections();
	void refreshManagedVocalPlayback();
	void pollManagedVocalPlaybackLoad();
	QString requestManagedVocalAsset(const ManagedVocalVariantReference& reference,
		std::shared_ptr<const std::vector<float>>& samples);
	void invalidateManagedVocalPlayback();
	ManagedVocalPlaybackPreparation prepareManagedVocalPlayback(
		const ManagedVocalVariantAssociation& association,
		std::shared_ptr<const std::vector<float>> samples,
		std::shared_ptr<const ManagedVocalPlaybackGuard> guard,
		quint64 expectedEpoch,
		bool autoResize,
		bool checkGeometry) const;
	QString applyManagedVocalVariantAssociationInternal(
		const ManagedVocalVariantAssociation& association,
		bool retainCurrentForRevert);
	void resetManagedVocalVariant();
	void setManagedVocalVariantStatus(const QString& status);
	AudioWindowEligibility audioWindowEligibility() const;
	std::atomic_bool m_loadingAudioWindow{false};
	std::atomic_bool m_loadingManagedVocalAssociation{false};
	std::shared_ptr<const struct AudioWindowPlaybackView> m_audioWindowView;
	std::optional<ManagedVocalVariantAssociation> m_managedVocalAssociation;
	QDomDocument m_unavailableManagedVocalVariants;
	QString m_managedVocalVariantStatus = QStringLiteral("ok");
	std::shared_ptr<ManagedVocalPlaybackGuard> m_managedVocalPlaybackGuard =
		std::make_shared<ManagedVocalPlaybackGuard>();
	std::shared_ptr<const ManagedVocalPlaybackView> m_managedVocalPlaybackView;
	QTimer m_managedVocalPlaybackLoadTimer;
	std::shared_ptr<ManagedVocalPlaybackLoadTicket> m_managedVocalPlaybackLoadTicket;
	ManagedVocalVariantReference m_managedVocalPlaybackLoadingReference;
	ManagedVocalVariantReference m_managedVocalPreloadedReference;
	std::shared_ptr<const std::vector<float>> m_managedVocalPreloadedSamples;
	void setSampleFileImpl(const QString& source, const QByteArray& expectedDigest, const QByteArray& expectedIdentity = {});
	QString adoptRecordedTakeAudio(std::unique_ptr<AudioImportPolicy::Prepared>, const recording::RecordedTakeReference&, bool notify);
	void resetRecordedTakeSource();
	Sample m_sample;
	// Preserve an intended file link when no file-backed buffer could be loaded.
	QString m_unavailableSampleFile;
	QString m_sourceLoadError;
	QByteArray m_boundedSourceDigest;
	QByteArray m_boundedSourceIdentity;
	bool m_invalidSourceIdentity = false;
	std::optional<SampleDecoder::PcmWaveInfo> m_sourceInfo;
	bool m_boundedSampleSource = false;
	bool m_recordedTakeMetadataPresent = false;
	std::optional<recording::RecordedTakeReference> m_recordedTakeReference;
	std::shared_ptr<recording::SealedTakeToken> m_recordedTakeToken;
	QDomElement m_unavailableRecordedTake;
	AudioWindowStorage m_audioWindowStorage;
	std::shared_ptr<AudioWindowPlaybackGuard> m_audioWindowGuard = std::make_shared<AudioWindowPlaybackGuard>();
	BoolModel m_recordModel;
	bool m_isPlaying;
	int m_startFrameOffset;

	friend class gui::SampleClipView;


signals:
	void audioWindowStatusChanged(const QString& code);
	void managedVocalVariantStatusChanged(const QString& code);
	void sampleChanged();
	void wasReversed();
} ;


} // namespace lmms

#endif // LMMS_SAMPLE_CLIP_H
