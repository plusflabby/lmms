#ifndef LMMS_MANAGED_VOCAL_ASSET_STORE_H
#define LMMS_MANAGED_VOCAL_ASSET_STORE_H

#include "ManagedVocalAssetRoot.h"
#include "ManagedVocalAssetTypes.h"
#include "ManagedVocalWaveReader.h"
#include <memory>
#include <optional>
#include <span>
#include <cstddef>
#include <vector>
#include "lmms_export.h"

namespace lmms {

struct ManagedVocalAssetCreateResult
{
	QString code;
	QString assetId;
	std::unique_ptr<class ManagedVocalAssetWriter> writer;
};

struct ManagedVocalAssetPublishResult
{
	QString code;
	std::optional<ManagedVocalVariantReference> value;
};

struct ManagedVocalAssetInspectResult
{
	QString code;
	std::optional<ManagedVocalVariantReference> reference;
	ManagedVocalMedia media;
	QString recipeHash;
	QString rendererId;
	QString rendererVersion;
	size_t sourceCount = 0;
};

struct ManagedVocalAssetOpenResult
{
	QString code;
	std::optional<ManagedVocalManifest> manifest;
	ManagedVocalWaveInfo media;
	std::vector<float> samples;
};

struct ManagedVocalAssetRecoveryItem
{
	QString assetId;
	QString state;
	bool stagedAudioPresent = false;
	bool stagedManifestPresent = false;
	bool finalAudioPresent = false;
	bool finalManifestPresent = false;
	quint64 stagedAudioBytes = 0;
	quint64 stagedManifestBytes = 0;
	quint64 finalAudioBytes = 0;
	quint64 finalManifestBytes = 0;
};

struct ManagedVocalAssetRecoveryScanResult
{
	QString code;
	std::vector<ManagedVocalAssetRecoveryItem> items;
	size_t totalItems = 0;
	bool truncated = false;
};

class LMMS_EXPORT ManagedVocalAssetWriter
{
public:
	~ManagedVocalAssetWriter();
	ManagedVocalAssetWriter(const ManagedVocalAssetWriter&) = delete;
	ManagedVocalAssetWriter& operator=(const ManagedVocalAssetWriter&) = delete;

	QString assetId() const;
	QString appendFrames(std::span<const float> interleavedSamples);
	quint64 framesWritten() const;
	quint64 expectedFrames() const;

private:
	friend class ManagedVocalAssetStore;
	struct State;
	explicit ManagedVocalAssetWriter(std::unique_ptr<State> state);
	std::unique_ptr<State> m_state;
};

class LMMS_EXPORT ManagedVocalAssetStore
{
public:
	explicit ManagedVocalAssetStore(
		std::shared_ptr<ManagedVocalAssetRoot> root,
		quint64 maximumStagedBytes = 256ULL * 1024 * 1024);

	ManagedVocalAssetCreateResult beginCreate(quint64 frames, int sampleRate, int channels);
	ManagedVocalAssetPublishResult publish(
		std::unique_ptr<ManagedVocalAssetWriter> writer,
		ManagedVocalManifest manifest);
	ManagedVocalAssetInspectResult inspect(const ManagedVocalVariantReference& reference) const;
	// Internal native use only. The manifest includes private authorized source references.
	ManagedVocalAssetOpenResult openVerified(const ManagedVocalVariantReference& reference) const;
	ManagedVocalAssetRecoveryScanResult scanRecovery(size_t limit = 128) const;

private:
	ManagedVocalAssetOpenResult readVariant(const ManagedVocalVariantReference&, bool retainSamples) const;
	std::shared_ptr<ManagedVocalAssetRoot> m_root;
	quint64 m_maximumStagedBytes = 0;
};

} // namespace lmms

#endif
