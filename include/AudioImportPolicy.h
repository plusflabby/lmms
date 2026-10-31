#ifndef LMMS_AUDIO_IMPORT_POLICY_H
#define LMMS_AUDIO_IMPORT_POLICY_H
#include "AudioFileAuthorization.h"
#include "SampleDecoder.h"
#include <QStringList>
#include <vector>
namespace lmms {
class SampleBuffer;
class SampleClip;
namespace recording { class SealedTakeToken; class RecordingAttachment; }
class LMMS_EXPORT AudioImportPolicy
{
	struct Epoch { bool active = true; };
	struct Slot { bool busy = false; };
public:
	class LMMS_EXPORT Prepared
	{
	public:
		~Prepared();
		bool isCurrent() const;
		const SampleDecoder::PcmWaveInfo& info() const { return m_info; }
		const QByteArray& identity() const { return m_lease.identity; }
		const QString& sourceReference() const { return m_path; }
		bool isRecordedTake() const noexcept { return bool(m_recordedTake); }
	private:
		friend class AudioImportPolicy;
		friend class SampleClip;
		friend class recording::RecordingAttachment;
		Prepared() = default;
		std::shared_ptr<Epoch> m_epoch;
		std::shared_ptr<Slot> m_slot;
		QStringList m_snapshot;
		quint64 m_revision = 0;
		AudioFileAuthorization::Result m_lease;
		std::shared_ptr<const SampleBuffer> m_buffer;
		std::shared_ptr<recording::SealedTakeToken> m_recordedTake;
		SampleDecoder::PcmWaveInfo m_info;
		QString m_path;
	};
	struct Result { QString code; std::unique_ptr<Prepared> audio; };
	AudioImportPolicy();
	~AudioImportPolicy();
	QString refresh();
	Result prepare(const QString& path, const QByteArray& expectedDigest = {}, const QByteArray& expectedIdentity = {});
	Result prepareRecordedTake(recording::SealedTakeToken&& source);
	static AudioImportPolicy& configured();
private:
	void revoke();
	QStringList m_snapshot;
	quint64 m_revision = 0;
	QString m_status;
	std::vector<std::shared_ptr<AudioFileAuthorization::Root>> m_roots;
	std::shared_ptr<Epoch> m_epoch;
	std::shared_ptr<Slot> m_slot;
};
}
#endif
