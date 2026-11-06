/* Copyright 2013-2017 Matt Tytel
 *
 * helm is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * helm is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with helm.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "helm_editor.h"

#include "default_look_and_feel.h"
#include "helm_common.h"
#include "load_save.h"
#include "mopo.h"
#include "startup.h"
#include "utils.h"

#include <iostream>

#define MAX_OUTPUT_MEMORY 1048576
#define MAX_BUFFER_PROCESS 256

namespace {
  // JUCE's JACK backend opens the first JACK client in port order as the audio "device",
  // without checking it can play audio. Under PipeWire that order changes between boots
  // (filters, monitors, the MIDI bridge...), so Helm sometimes starts with no outputs.
  // If that happens, try the output devices in order (the ones whose name contains
  // HELM_JACK_OUTPUT first, e.g. "Mbox") and keep the first with "playback_*" channels.
  bool hasPlayback(AudioDeviceManager& manager) {
    AudioIODevice* device = manager.getCurrentAudioDevice();
    if (device == nullptr)
      return false;
    for (const String& name : device->getOutputChannelNames()) {
      if (name.startsWith("playback"))
        return true;
    }
    return false;
  }

  void selectJackOutput(AudioDeviceManager& manager) {
    AudioIODevice* device = manager.getCurrentAudioDevice();
    if (device == nullptr || device->getTypeName() != "JACK" ||
        device->getOutputChannelNames().size() > 0)
      return;

    AudioIODeviceType* type = manager.getCurrentDeviceTypeObject();
    if (type == nullptr)
      return;

    String wanted = SystemStats::getEnvironmentVariable("HELM_JACK_OUTPUT", "");
    StringArray candidates;
    for (const String& name : type->getDeviceNames(false)) {
      if (wanted.isNotEmpty() && name.containsIgnoreCase(wanted))
        candidates.insert(0, name);
      else if (name != "Helm" && name != "Midi-Bridge")
        candidates.add(name);
    }

    AudioDeviceManager::AudioDeviceSetup original;
    manager.getAudioDeviceSetup(original);
    for (const String& name : candidates) {
      AudioDeviceManager::AudioDeviceSetup setup = original;
      setup.outputDeviceName = name;
      setup.inputDeviceName = String();
      setup.useDefaultOutputChannels = true;
      setup.useDefaultInputChannels = true;
      if (manager.setAudioDeviceSetup(setup, true).isEmpty() && hasPlayback(manager)) {
        std::cerr << "Helm: JACK output -> " << name << std::endl;
        return;
      }
    }
    std::cerr << "Helm: no JACK output with playback ports found" << std::endl;
  }
}

HelmEditor::HelmEditor(bool use_gui) : SynthGuiInterface(this, use_gui) {
  computer_keyboard_ = new HelmComputerKeyboard(&engine_, keyboard_state_);

  setAudioChannels(0, mopo::NUM_CHANNELS);

  AudioDeviceManager::AudioDeviceSetup setup;
  deviceManager.getAudioDeviceSetup(setup);
  setup.sampleRate = mopo::DEFAULT_SAMPLE_RATE;
  deviceManager.initialise(0, mopo::NUM_CHANNELS, nullptr, true, "", &setup);

  if (deviceManager.getCurrentAudioDevice() == nullptr) {
    const OwnedArray<AudioIODeviceType>& device_types = deviceManager.getAvailableDeviceTypes();

    for (AudioIODeviceType* device_type : device_types) {
      deviceManager.setCurrentAudioDeviceType(device_type->getTypeName(), true);
      if (deviceManager.getCurrentAudioDevice())
        break;
    }
  }

  selectJackOutput(deviceManager);

  const StringArray all_midi_ins(MidiInput::getDevices());

  for (int i = 0; i < all_midi_ins.size(); ++i)
    deviceManager.setMidiInputEnabled(all_midi_ins[i], true);

  deviceManager.addMidiInputCallback("", midi_manager_);

  if (use_gui) {
    setLookAndFeel(DefaultLookAndFeel::instance());
    addAndMakeVisible(gui_);
    gui_->setOutputMemory(getOutputMemory());
    float window_size = LoadSave::loadWindowSize();
    setSize(window_size * mopo::DEFAULT_WINDOW_WIDTH, window_size * mopo::DEFAULT_WINDOW_HEIGHT);

    setWantsKeyboardFocus(true);
    addKeyListener(computer_keyboard_);
    setOpaque(true);
  }
}

HelmEditor::~HelmEditor() {
  shutdownAudio();
  midi_manager_ = nullptr;
  computer_keyboard_ = nullptr;
  gui_ = nullptr;
  keyboard_state_ = nullptr;
}

void HelmEditor::prepareToPlay(int buffer_size, double sample_rate) {
  engine_.setSampleRate(sample_rate);
  engine_.setBufferSize(std::min(buffer_size, MAX_BUFFER_PROCESS));
  engine_.updateAllModulationSwitches();
  midi_manager_->setSampleRate(sample_rate);
}

void HelmEditor::getNextAudioBlock(const AudioSourceChannelInfo& buffer) {
  ScopedLock lock(getCriticalSection());

  int num_samples = buffer.buffer->getNumSamples();
  int synth_samples = std::min(num_samples, MAX_BUFFER_PROCESS);

  processControlChanges();
  processModulationChanges();
  MidiBuffer midi_messages;
  midi_manager_->removeNextBlockOfMessages(midi_messages, num_samples);
  processMidi(midi_messages);
  processKeyboardEvents(midi_messages, num_samples);

  for (int b = 0; b < num_samples; b += synth_samples) {
    int current_samples = std::min<int>(synth_samples, num_samples - b);

    processAudio(buffer.buffer, mopo::NUM_CHANNELS, current_samples, b);
  }
}

void HelmEditor::releaseResources() {
}

void HelmEditor::paint(Graphics& g) {
}

void HelmEditor::resized() {
  if (gui_)
    gui_->setBounds(getBounds());
}

void HelmEditor::animate(bool animate) {
  if (gui_)
    gui_->animate(animate);
}
