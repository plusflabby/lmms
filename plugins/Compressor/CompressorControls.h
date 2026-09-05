/*
 * CompressorControls.h
 *
 * Copyright (c) 2020 Lost Robot <r94231@gmail.com>
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

#ifndef COMPRESSOR_CONTROLS_H
#define COMPRESSOR_CONTROLS_H

#include "CompressorControlDialog.h"

#include "EffectControls.h"

namespace lmms
{

class CompressorEffect;


class CompressorControls : public EffectControls
{
	Q_OBJECT
public:
	CompressorControls(CompressorEffect* effect);

	void saveSettings(QDomDocument & _doc, QDomElement & _parent) override;
	void loadSettings(const QDomElement & _this) override;
	inline QString nodeName() const override
	{
		return "CompressorControls";
	}

	int controlCount() override
	{
		return 28;
	}
	FloatModel& thresholdModel() { return m_thresholdModel; }
	FloatModel& ratioModel() { return m_ratioModel; }
	FloatModel& attackModel() { return m_attackModel; }
	FloatModel& releaseModel() { return m_releaseModel; }
	FloatModel& kneeModel() { return m_kneeModel; }
	FloatModel& holdModel() { return m_holdModel; }
	FloatModel& rangeModel() { return m_rangeModel; }
	FloatModel& rmsModel() { return m_rmsModel; }
	IntModel& midsideModel() { return m_midsideModel; }
	IntModel& peakmodeModel() { return m_peakmodeModel; }
	FloatModel& lookaheadLengthModel() { return m_lookaheadLengthModel; }
	FloatModel& inBalanceModel() { return m_inBalanceModel; }
	FloatModel& outBalanceModel() { return m_outBalanceModel; }
	IntModel& limiterModel() { return m_limiterModel; }
	FloatModel& outGainModel() { return m_outGainModel; }
	FloatModel& inGainModel() { return m_inGainModel; }
	FloatModel& blendModel() { return m_blendModel; }
	FloatModel& stereoBalanceModel() { return m_stereoBalanceModel; }
	BoolModel& autoMakeupModel() { return m_autoMakeupModel; }
	BoolModel& auditionModel() { return m_auditionModel; }
	BoolModel& feedbackModel() { return m_feedbackModel; }
	FloatModel& autoAttackModel() { return m_autoAttackModel; }
	FloatModel& autoReleaseModel() { return m_autoReleaseModel; }
	BoolModel& lookaheadModel() { return m_lookaheadModel; }
	FloatModel& tiltModel() { return m_tiltModel; }
	FloatModel& tiltFreqModel() { return m_tiltFreqModel; }
	IntModel& stereoLinkModel() { return m_stereoLinkModel; }
	FloatModel& mixModel() { return m_mixModel; }

	gui::EffectControlDialog* createView() override
	{
		return new gui::CompressorControlDialog(this);
	}

private:
	CompressorEffect * m_effect;

	FloatModel m_thresholdModel;
	FloatModel m_ratioModel;
	FloatModel m_attackModel;
	FloatModel m_releaseModel;
	FloatModel m_kneeModel;
	FloatModel m_holdModel;
	FloatModel m_rangeModel;
	FloatModel m_rmsModel;
	IntModel m_midsideModel;
	IntModel m_peakmodeModel;
	FloatModel m_lookaheadLengthModel;
	FloatModel m_inBalanceModel;
	FloatModel m_outBalanceModel;
	IntModel m_limiterModel;
	FloatModel m_outGainModel;
	FloatModel m_inGainModel;
	FloatModel m_blendModel;
	FloatModel m_stereoBalanceModel;
	BoolModel m_autoMakeupModel;
	BoolModel m_auditionModel;
	BoolModel m_feedbackModel;
	FloatModel m_autoAttackModel;
	FloatModel m_autoReleaseModel;
	BoolModel m_lookaheadModel;
	FloatModel m_tiltModel;
	FloatModel m_tiltFreqModel;
	IntModel m_stereoLinkModel;
	FloatModel m_mixModel;

	float m_inPeakL;
	float m_inPeakR;
	float m_outPeakL;
	float m_outPeakR;

	friend class gui::CompressorControlDialog;
	friend class CompressorEffect;

} ;


} // namespace lmms

#endif
