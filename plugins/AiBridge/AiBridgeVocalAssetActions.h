#ifndef LMMS_AI_BRIDGE_VOCAL_ASSET_ACTIONS_H
#define LMMS_AI_BRIDGE_VOCAL_ASSET_ACTIONS_H

#include "AiBridgeVocalEditPlanner.h"
#include "AiBridgeVocalTypes.h"
#include "src/core/vocal/ManagedVocalAssetTypes.h"
#include "src/core/vocal/ManagedVocalAssetStore.h"
#include <atomic>
#include <memory>
#include <optional>

namespace lmms {

class ManagedVocalAssetRoot;
class ManagedVocalSaveAsSource;

struct VocalEditPlanTicket
{
	VocalEditRequest request;
	VocalEditPlanningSnapshot planning;
	VocalEditPlan plan;
	VocalInputSnapshot sources;
	VocalEditLimits limits;
};

struct VocalEditPlanCaptureResult
{
	QString code;
	std::optional<VocalEditPlanTicket> value;
};

struct VocalEditRenderInput
{
	VocalEditPlanTicket ticket;
	std::shared_ptr<ManagedVocalAssetRoot> assetRoot;
	QString projectId;
	std::shared_ptr<ManagedVocalSaveAsSource> managedProjectHold;
};

struct VocalEditRenderInputResult
{
	QString code;
	std::optional<VocalEditRenderInput> value;
};

struct VocalRenderBudget
{
	QString code;
	quint64 renderInputFrames = 0;
	quint64 outputFrames = 0;
	quint64 sessionBytes = 0;
	quint64 jobBytes = 0;
	quint64 stagedBytes = 0;
	int sampleRate = 0;
	int channels = 0;
};

struct VocalRenderMeasurements
{
	quint64 frames = 0;
	int sampleRate = 0;
	int channels = 0;
	double sourcePeak = 0.0;
	double sourceRms = 0.0;
	bool sourceOverrange = false;
	double peak = 0.0;
	double rms = 0.0;
	bool overrange = false;
};

struct VocalVariantRenderResult
{
	QString code;
	std::optional<ManagedVocalVariantReference> variant;
	VocalRenderMeasurements measurements;
	quint64 accountedBytes = 0;
};

class AiBridgeVocalAssetActions
{
public:
	static VocalEditPlanCaptureResult plan(
		const VocalEditRequest& request,
		const VocalEditLimits& limits);
	static ManagedVocalAssetInspectResult inspectVariant(const QString& assetId);
	static VocalEditRenderInputResult prepareRender(const VocalEditPlanTicket& ticket);
	static VocalRenderBudget estimate(const VocalEditPlanTicket& ticket);
	static VocalRenderBudget dryRun(const VocalEditPlanTicket& ticket);
	static VocalVariantRenderResult render(
		const VocalEditRenderInput& input,
		const std::atomic_bool& cancelled,
		const VocalPhaseCallback& phase);
};

} // namespace lmms

#endif
