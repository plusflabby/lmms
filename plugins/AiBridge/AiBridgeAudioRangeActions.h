#ifndef LMMS_AIBRIDGE_AUDIO_RANGE_ACTIONS_H
#define LMMS_AIBRIDGE_AUDIO_RANGE_ACTIONS_H
#include "AiBridgeAudioRangePlanner.h"
#include "AiBridgeAudioActions.h"
namespace lmms {
AiBridgeAudioRangePlan prepareAudioRange(const AiBridgeAudioRangeRequest&);
AiBridgeAudioEditResult executeAudioRange(const AiBridgeAudioRangePlan&,bool dryRun);
#ifdef LMMS_AUDIO_RANGE_TESTING
class SampleClip;
enum class AudioRangeTestStage { BeforeWrite, AfterPublication, AfterConstruction, BeforeVerification };
using AudioRangeTestHook=void(*)(AudioRangeTestStage,int,SampleClip*);
void setAudioRangeTestHook(AudioRangeTestHook);
#endif
}
#endif
