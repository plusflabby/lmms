/*
 * Engine.cpp - implementation of LMMS' engine-system
 *
 * Copyright (c) 2006-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
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


#include "Engine.h"
#include "AudioEngine.h"
#include "ConfigManager.h"
#include "Mixer.h"
#include "Ladspa2LMMS.h"
#include "Lv2Manager.h"
#include "PatternStore.h"
#include "Plugin.h"
#include "PresetPreviewPlayHandle.h"
#include "ProjectJournal.h"
#include "Song.h"
#include "BandLimitedWave.h"
#include "Oscillator.h"
#include "RecordingPortAudioInput.h"
#include "RecordingServiceHost.h"
#include "RecordingSessionManager.h"

namespace lmms
{

float Engine::s_framesPerTick;
AudioEngine* Engine::s_audioEngine = nullptr;
Mixer * Engine::s_mixer = nullptr;
PatternStore * Engine::s_patternStore = nullptr;
Song * Engine::s_song = nullptr;
ProjectJournal * Engine::s_projectJournal = nullptr;
#ifdef LMMS_HAVE_LV2
Lv2Manager * Engine::s_lv2Manager = nullptr;
#endif
Ladspa2LMMS * Engine::s_ladspaManager = nullptr;
void* Engine::s_dndPluginKey = nullptr;
recording::RecordingServiceHost* s_recordingServiceHost = nullptr;
std::unique_ptr<recording::RecordingTransportControl> s_recordingTransport;
std::unique_ptr<recording::RecordingSessionManager> s_recordingSessionManager;




void Engine::init( bool renderOnly )
{
	Engine *engine = inst();

	emit engine->initProgress(tr("Generating wavetables"));
	// generate (load from file) bandlimited wavetables
	BandLimitedWave::generateWaves();
	//initialize oscillators
	Oscillator::waveTableInit();

	emit engine->initProgress(tr("Initializing data structures"));
	s_projectJournal = new ProjectJournal;
	s_audioEngine = new AudioEngine( renderOnly );
	s_song = new Song;
	if(s_recordingServiceHost == nullptr)
	{
		s_recordingServiceHost = new recording::RecordingServiceHost(recording::createPlatformRecordingInputDriver());
	}
	s_mixer = new Mixer;
	s_patternStore = new PatternStore;

#ifdef LMMS_HAVE_LV2
	s_lv2Manager = new Lv2Manager;
	s_lv2Manager->initPlugins();
#endif
	s_ladspaManager = new Ladspa2LMMS;
	if (!s_recordingTransport)
	{
		s_recordingTransport = recording::createNativeRecordingTransport(*s_song);
	}
	if (!s_recordingSessionManager && s_recordingTransport)
	{
		s_recordingSessionManager = std::make_unique<recording::RecordingSessionManager>(
			*s_recordingServiceHost, *s_recordingTransport);
	}

	s_projectJournal->setJournalling( true );

	emit engine->initProgress(tr("Opening audio and midi devices"));
	s_audioEngine->initDevices();

	PresetPreviewPlayHandle::init();

	emit engine->initProgress(tr("Launching audio engine threads"));
	s_audioEngine->startProcessing();
}




void Engine::destroy()
{
	bool recordingHostClosed = false;
	if (s_recordingSessionManager)
	{
		const auto closing = s_recordingSessionManager->requestShutdown();
		if (!closing.ok || !s_recordingSessionManager->quiescent()) return;
		s_recordingSessionManager.reset();
		s_recordingTransport.reset();
		recordingHostClosed = true;
	}
	if(s_recordingServiceHost)
	{
		if(!recordingHostClosed && !s_recordingServiceHost->requestShutdown().ok) { return; }
		delete s_recordingServiceHost;
		s_recordingServiceHost = nullptr;
	}
	s_projectJournal->stopAllJournalling();
	s_audioEngine->stopProcessing();

	PresetPreviewPlayHandle::cleanup();

	s_song->clearProject();

	deleteHelper( &s_patternStore );

	deleteHelper( &s_mixer );
	deleteHelper( &s_audioEngine );

#ifdef LMMS_HAVE_LV2
	deleteHelper( &s_lv2Manager );
#endif
	deleteHelper( &s_ladspaManager );

	//delete ConfigManager::inst();
	deleteHelper( &s_projectJournal );

	deleteHelper( &s_song );

	delete ConfigManager::inst();

	// The oscillator FFT plans remain throughout the application lifecycle
	// due to being expensive to create, and being used whenever a userwave form is changed
	Oscillator::destroyFFTPlans();
}


recording::RecordingServiceHost* Engine::recordingServiceHost()
{
	return s_recordingServiceHost;
}

recording::RecordingSessionManager* Engine::recordingSessionManager()
{
	return s_recordingSessionManager.get();
}




float Engine::framesPerTick(sample_rate_t sampleRate)
{
	return sampleRate * 60.0f * 4 /
			DefaultTicksPerBar / s_song->getTempo();
}




void Engine::updateFramesPerTick()
{
	s_framesPerTick = s_audioEngine->outputSampleRate() * 60.0f * 4 / DefaultTicksPerBar / s_song->getTempo();
}




void Engine::setDndPluginKey(void *newKey)
{
	Q_ASSERT(static_cast<Plugin::Descriptor::SubPluginFeatures::Key*>(newKey));
	s_dndPluginKey = newKey;
}




void *Engine::pickDndPluginKey()
{
	return s_dndPluginKey;
}




Engine * Engine::s_instanceOfMe = nullptr;

} // namespace lmms
