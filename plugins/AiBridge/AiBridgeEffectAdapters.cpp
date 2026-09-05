#include "AiBridgeEffectAdapters.h"

#include "AmplifierControls.h"
#include "CompressorControls.h"
#include "CrossoverEQControls.h"
#include "DelayControls.h"
#include "ReverbSCControls.h"
#include "AutomatableModel.h"

#include <cmath>
#include <QSet>
#include <algorithm>

namespace lmms
{
namespace
{
using Type = AiBridgeEffectParameterType;
using Getter = AutomatableModel* (*)(EffectControls*);
struct Spec
{
	const char* id;
	const char* name;
	Type type;
	float minimum;
	float maximum;
	float defaultValue;
	float step;
	const char* unit;
	QStringList enums;
	Getter getter;
};

#define GETTER(C, M) [](EffectControls* c) -> AutomatableModel* { return &static_cast<C*>(c)->M(); }
#define F(ID, NAME, C, M, MIN, MAX, DEF, STEP, UNIT) {ID, NAME, Type::Float, MIN, MAX, DEF, STEP, UNIT, {}, GETTER(C, M)}
#define B(ID, NAME, C, M) {ID, NAME, Type::Boolean, 0, 1, 0, 1, "", {}, GETTER(C, M)}
#define E(ID, NAME, C, M, VALUES) {ID, NAME, Type::Enum, 0, static_cast<float>((VALUES).size() - 1), 0, 1, "", VALUES, GETTER(C, M)}

const QVector<Spec>& specs(const QString& id)
{
	static const QVector<Spec> amplifier = {
		F("volume", "Volume", AmplifierControls, volumeModel, 0, 200, 100, .1f, "%"),
		F("pan", "Panning", AmplifierControls, panModel, -100, 100, 0, .1f, "%"),
		F("left_gain", "Left gain", AmplifierControls, leftModel, 0, 200, 100, .1f, "%"),
		F("right_gain", "Right gain", AmplifierControls, rightModel, 0, 200, 100, .1f, "%")};
	static const QVector<Spec> crossover = {
		F("band_1_2_crossover", "Band 1/2 Crossover", CrossoverEQControls, xover12Model, 50, 10000, 125, 1, "Hz"),
		F("band_2_3_crossover", "Band 2/3 Crossover", CrossoverEQControls, xover23Model, 50, 20000, 1250, 1, "Hz"),
		F("band_3_4_crossover", "Band 3/4 Crossover", CrossoverEQControls, xover34Model, 50, 20000, 5000, 1, "Hz"),
		F("band_1_gain", "Band 1 Gain", CrossoverEQControls, gain1Model, -60, 30, 0, .1f, "dBFS"),
		F("band_2_gain", "Band 2 Gain", CrossoverEQControls, gain2Model, -60, 30, 0, .1f, "dBFS"),
		F("band_3_gain", "Band 3 Gain", CrossoverEQControls, gain3Model, -60, 30, 0, .1f, "dBFS"),
		F("band_4_gain", "Band 4 Gain", CrossoverEQControls, gain4Model, -60, 30, 0, .1f, "dBFS"),
		B("band_1_mute", "Mute Band 1", CrossoverEQControls, mute1Model), B("band_2_mute", "Mute Band 2", CrossoverEQControls, mute2Model),
		B("band_3_mute", "Mute Band 3", CrossoverEQControls, mute3Model), B("band_4_mute", "Mute Band 4", CrossoverEQControls, mute4Model)};
	static const QVector<Spec> delay = {
		F("delay_time", "Delay samples", DelayControls, delayTimeModel, .01f, 5, .5f, .0001f, "seconds"),
		F("feedback", "Feedback", DelayControls, feedbackModel, 0, 1, 0, .01f, "native"),
		F("lfo_time", "LFO frequency", DelayControls, lfoTimeModel, .01f, 5, 2, .0001f, "seconds"),
		F("lfo_amount", "LFO amount", DelayControls, lfoAmountModel, 0, .5f, 0, .0001f, "native"),
		F("output_gain", "Output gain", DelayControls, outGainModel, -60, 20, 0, .01f, "dBFS")};
	static const QVector<Spec> reverb = {
		F("input_gain", "Input gain", ReverbSCControls, inputGainModel, -60, 15, 0, .1f, "dB"),
		F("size", "Size", ReverbSCControls, sizeModel, 0, 1, .89f, .01f, "normalized"),
		F("color", "Color", ReverbSCControls, colorModel, 100, 15000, 10000, .1f, "Hz"),
		F("output_gain", "Output gain", ReverbSCControls, outputGainModel, -60, 15, 0, .1f, "dB")};
	static const QVector<Spec> compressor = {
		F("threshold", "Threshold", CompressorControls, thresholdModel, -60, 0, -8, .001f, "dBFS"), F("ratio", "Ratio", CompressorControls, ratioModel, 1, 20, 1.8f, .001f, "ratio"),
		F("attack", "Attack", CompressorControls, attackModel, .005f, 250, 10, .001f, "ms"), F("release", "Release", CompressorControls, releaseModel, 1, 2500, 100, .001f, "ms"),
		F("knee", "Knee", CompressorControls, kneeModel, 0, 96, 12, .01f, "dB"), F("hold", "Hold", CompressorControls, holdModel, 0, 500, 0, .01f, "ms"),
		F("range", "Range", CompressorControls, rangeModel, -240, 0, -240, .01f, "dBFS"), F("rms_size", "RMS Size", CompressorControls, rmsModel, 0, 250, 1, .01f, "native"),
		E("mid_side", "Mid/Side", CompressorControls, midsideModel, (QStringList{"left_right", "mid_side"})),
		E("peak_mode", "Peak Mode", CompressorControls, peakmodeModel, (QStringList{"rms", "peak"})),
		F("lookahead_length", "Lookahead Length", CompressorControls, lookaheadLengthModel, 0, 20, 0, .0001f, "ms"), F("input_balance", "Input Balance", CompressorControls, inBalanceModel, -1, 1, 0, .0001f, "native"),
		F("output_balance", "Output Balance", CompressorControls, outBalanceModel, -1, 1, 0, .0001f, "native"),
		E("limiter", "Limiter", CompressorControls, limiterModel, (QStringList{"compressor", "limiter"})),
		F("output_gain", "Output Gain", CompressorControls, outGainModel, -60, 30, 0, .01f, "dBFS"), F("input_gain", "Input Gain", CompressorControls, inGainModel, -60, 30, 0, .01f, "dBFS"),
		F("blend", "Blend", CompressorControls, blendModel, 0, 3, 1, .0001f, "native"), F("stereo_balance", "Stereo Balance", CompressorControls, stereoBalanceModel, -1, 1, 0, .0001f, "native"),
		B("auto_makeup", "Auto Makeup Gain", CompressorControls, autoMakeupModel), B("audition", "Audition", CompressorControls, auditionModel), B("feedback", "Feedback", CompressorControls, feedbackModel),
		F("auto_attack", "Auto Attack", CompressorControls, autoAttackModel, 0, 100, 0, .01f, "%"), F("auto_release", "Auto Release", CompressorControls, autoReleaseModel, 0, 100, 0, .01f, "%"),
		B("lookahead", "Lookahead", CompressorControls, lookaheadModel), F("tilt", "Tilt", CompressorControls, tiltModel, -6, 6, 0, .0001f, "dB"),
		F("tilt_frequency", "Tilt Frequency", CompressorControls, tiltFreqModel, 20, 20000, 150, .1f, "Hz"),
		E("stereo_link", "Stereo Link", CompressorControls, stereoLinkModel, (QStringList{"unlinked", "maximum", "average", "minimum", "blend"})),
		F("mix", "Mix", CompressorControls, mixModel, 0, 100, 100, .01f, "%")};
	if (id == QLatin1String("amplifier")) return amplifier;
	if (id == QLatin1String("compressor")) return compressor;
	if (id == QLatin1String("crossovereq")) return crossover;
	if (id == QLatin1String("delay")) return delay;
	if (id == QLatin1String("reverbsc")) return reverb;
	static const QVector<Spec> empty;
	return empty;
}
}

QVector<AiBridgeEffectParameterMetadata> AiBridgeEffectAdapters::parameters(const QString& effectId, EffectControls* controls)
{
	QVector<AiBridgeEffectParameterMetadata> result;
	for (const auto& spec : specs(effectId)) {
		const auto* model = spec.getter(controls);
		result.append({QString::fromUtf8(spec.id), QString::fromUtf8(spec.name), spec.type, model->value<float>(), spec.minimum, spec.maximum, spec.defaultValue, spec.step, QString::fromUtf8(spec.unit), spec.enums, true});
	}
	return result;
}

bool AiBridgeEffectAdapters::setParameter(const QString& effectId, EffectControls* controls, const QString& parameterId, float value)
{
	if (!std::isfinite(value)) return false;
	for (const auto& spec : specs(effectId)) if (parameterId == QLatin1String(spec.id)) {
		if (value < spec.minimum || value > spec.maximum) return false;
		if (spec.type == Type::Boolean || spec.type == Type::Enum) {
			if (value != std::round(value)) return false;
		}
		spec.getter(controls)->setValue(value);
		return true;
	}
	return false;
}

bool AiBridgeEffectAdapters::validateBatch(const QVector<AiBridgeEffectParameterMetadata>& parameters,
	const QVector<AiBridgeEffectParameterChange>& changes, QString* error)
{
	QSet<QString> seen;
	for (const auto& change : changes) {
		if (seen.contains(change.id)) { if (error) *error = "duplicate_parameter_id"; return false; }
		seen.insert(change.id);
		const auto it = std::find_if(parameters.cbegin(), parameters.cend(), [&](const auto& p) { return p.id == change.id; });
		if (it == parameters.cend()) { if (error) *error = "parameter_not_found"; return false; }
		if (!it->writable) { if (error) *error = "read_only"; return false; }
		if (change.type != it->type) { if (error) *error = "wrong_type"; return false; }
		if (!std::isfinite(change.value) || change.value < it->minimum || change.value > it->maximum) { if (error) *error = "out_of_range"; return false; }
		if ((it->type == AiBridgeEffectParameterType::Boolean || it->type == AiBridgeEffectParameterType::Enum) && change.value != std::round(change.value)) { if (error) *error = "invalid_enum"; return false; }
		if (it->type == AiBridgeEffectParameterType::Float && it->step > 0.0f)
		{
			const auto index = (change.value - it->minimum) / it->step;
			const auto nearest = it->minimum + std::round(index) * it->step;
			const auto tolerance = std::max(0.000001f, std::abs(it->step) * 0.01f);
			if (std::abs(change.value - nearest) > tolerance)
			{
				if (error) *error = "invalid_step";
				return false;
			}
		}
	}
	return true;
}
}
