#include "sound_player.hpp"
#include "../synthetiser/constants.h"
#include "../utils/sound_conversions.hpp"
#include "../utils/synth_utils.hpp"
#include <alsa/control.h>
#include <alsa/error.h>
#include <alsa/global.h>
#include <alsa/pcm.h>
#include <fstream>
#include <iostream>
#include <unistd.h>

/// this function has been adapted from juce's source code
/// https://github.com/juce-framework/JUCE/blob/master/modules/juce_audio_devices/native/juce_ALSA_linux.cpp
bool SoundPlayer::setParameters(unsigned int sampleRate, int numChannels,
                                int bufferSize) {
  if (handle == nullptr)
    return false;

  snd_pcm_hw_params_t *hwParams;
  snd_pcm_hw_params_alloca(&hwParams);

  if (snd_pcm_hw_params_any(handle, hwParams) < 0) {
    // this is the error message that aplay returns when an error happens here,
    // it is a bit more explicit that "Invalid parameter"
    std::cout
        << "Broken configuration for this PCM: no configurations available\n";
    return false;
  }

  if (snd_pcm_hw_params_set_access(handle, hwParams,
                                   SND_PCM_ACCESS_RW_INTERLEAVED) <
      0) // works better for plughw..
  {
    std::cout << "error setting pcm access\n";
    return false;
  }

  enum {
    isFloatBit = 1 << 16,
    isLittleEndianBit = 1 << 17,
    onlyUseLower24Bits = 1 << 18
  };

  const int formatsToTry[] = {
      SND_PCM_FORMAT_FLOAT_LE, 32 | isFloatBit | isLittleEndianBit,
      SND_PCM_FORMAT_FLOAT_BE, 32 | isFloatBit,
      SND_PCM_FORMAT_S32_LE,   32 | isLittleEndianBit,
      SND_PCM_FORMAT_S32_BE,   32,
      SND_PCM_FORMAT_S24_3LE,  24 | isLittleEndianBit,
      SND_PCM_FORMAT_S24_3BE,  24,
      SND_PCM_FORMAT_S24_LE,   32 | isLittleEndianBit | onlyUseLower24Bits,
      SND_PCM_FORMAT_S16_LE,   16 | isLittleEndianBit,
      SND_PCM_FORMAT_S16_BE,   16};
  bitDepth = 0;

  for (int i = 0; i < 18; i += 2) {
    // maybe weird behaviour here
    // as soon as we get a valid format we have to select it
    // else the next attempts will fail with error
    // "bad argument"
    if (snd_pcm_hw_params_set_format(handle, hwParams,
                                     (_snd_pcm_format)formatsToTry[i]) >= 0) {
      const int type = formatsToTry[i + 1];
      bitDepth = type & 255;

      std::cout << "format "
                << snd_pcm_format_description((_snd_pcm_format)formatsToTry[i])
                << " is selected\n";
      break;
    }
  }

  if (bitDepth == 0) {
    std::cout << "device doesn't support a compatible PCM format\n";
    return false;
  }

  int dir = 0;
  unsigned int periods = 4;
  snd_pcm_uframes_t samplesPerPeriod = (snd_pcm_uframes_t)bufferSize;

  if (snd_pcm_hw_params_set_rate_near(handle, hwParams, &sampleRate, nullptr) <
          0 ||
      snd_pcm_hw_params_set_channels(handle, hwParams,
                                     (unsigned int)numChannels) < 0 ||
      snd_pcm_hw_params_set_periods_near(handle, hwParams, &periods, &dir) <
          0 ||
      snd_pcm_hw_params_set_period_size_near(handle, hwParams,
                                             &samplesPerPeriod, &dir) < 0 ||
      snd_pcm_hw_params(handle, hwParams) < 0) {
    std::cout << "error setting hw params\n";
    return false;
  }

  snd_pcm_uframes_t frames = 0;

  if (snd_pcm_hw_params_get_period_size(hwParams, &frames, &dir) < 0 ||
      snd_pcm_hw_params_get_periods(hwParams, &periods, &dir) < 0)
    latency = 0;
  else
    latency = (int)frames *
              ((int)periods -
               1); // (this is the method JACK uses to guess the latency..)

  snd_pcm_sw_params_t *swParams;
  snd_pcm_sw_params_alloca(&swParams);
  snd_pcm_uframes_t boundary;

  if (snd_pcm_sw_params_current(handle, swParams) < 0 ||
      snd_pcm_sw_params_get_boundary(swParams, &boundary) < 0 ||

      snd_pcm_sw_params_set_silence_threshold(handle, swParams, 0) < 0 ||

      snd_pcm_sw_params_set_silence_size(handle, swParams, boundary) < 0 ||
      snd_pcm_sw_params_set_start_threshold(handle, swParams,
                                            samplesPerPeriod) < 0 ||

      snd_pcm_sw_params_set_stop_threshold(handle, swParams, boundary) < 0 ||
      snd_pcm_sw_params(handle, swParams) < 0) {
    std::cout << "error setting sw params\n";
    return false;
  }

  numChannelsRunning = numChannels;

  return true;
}

// end of juce adapted code
////////////////

SoundPlayer::SoundPlayer(
    std::function<size_t(std::vector<std::string> *)> selectDeviceCallback) {
  int error;

  // device selection logic
  char **hints;
  int err = snd_device_name_hint(-1, "pcm", (void ***)&hints);

  if (err != 0) {
    // nothing for now
    std::cout
        << "could not detect any usable device on the system, exiting...\n";
    exit(-1);
  } else {
    std::vector<std::string> device_names{};
    char **n = hints;
    int device_number = 0;
    while (*n != NULL) {
      std::string name = std::string(snd_device_name_get_hint(*n, "NAME"));

      if ("null" != name) {
        // valid name here, select only plughw types as they are most reliable
        if (name.length() > 7 && name.substr(0, 7) == "plughw:") {
          device_names.push_back(name);
        }
      }
      n++;
    }

    size_t device_idx = selectDeviceCallback(&device_names);

    // assume always receiving correct idx
    selected_device = device_names.at(device_idx);
    std::cout << "selected device is: " << selected_device << std::endl;
  }

  snd_device_name_free_hint((void **)hints);
}

void SoundPlayer::setup_pipe() {
  unsigned int minChansOut = 0, maxChansOut = 0;

  getDeviceProperties(selected_device, minChansOut, maxChansOut);

  if (snd_pcm_open(&handle, selected_device.c_str(), SND_PCM_STREAM_PLAYBACK,
                   0) < 0) {
    std::cout
        << "could not open communication with selected device, exiting...\n";
  }

  if (!setParameters(44'100, minChansOut, BUFFER_SIZE)) {
    std::cout
        << "could not set parameters correctly for selected device, exiting\n";
    exit(-1);
  }
}

void SoundPlayer::playAsync(float *buffer, Operator *synth_operator,
                            Operator *synth_operator2,
                            Operator *synth_operator3,
                            Operator *synth_operator4, NoteMap *note_map,
                            std::mutex *map_mutex) {
  int error = 0;
  setup_pipe();
  private_data data{
      .buffer = buffer,
      .synth_operator = synth_operator,
      .synth_operator2 = synth_operator2,
      .synth_operator3 = synth_operator3,
      .synth_operator4 = synth_operator4,
      .note_map = note_map,
      .map_mutex = map_mutex,
      .recording = &temp_output_buffer,
      .is_recording = &is_recording,
  };
  error = snd_async_add_pcm_handler(&ahandler, handle, async_player_callback,
                                    &data);
  if (error < 0) {
    std::cout << "could not add async handler " << error << " "
              << snd_strerror(error) << std::endl;
    exit(-1);
  }

  /// initial write
  map_mutex->lock();
  for (size_t i = 0; i < note_map->notes.size(); ++i) {

    auto note = calculate_frequency(note_map->notes[i].note_value);
    synth_operator->updateFrequency(i, note);
    synth_operator2->updateFrequency(i, note);
    synth_operator3->updateFrequency(i, note);
    synth_operator4->updateFrequency(i, note);

    *note_map->has_updated_value = false;
    map_mutex->unlock();
    for (int count = 0; count < 2; count++) {
      for (int i = 0; i < BUFFER_SIZE; ++i) {
        buffer[i] = 0; // floatTo16bits(osc->advance());
      }

      do {
        error = snd_pcm_writei(handle, buffer, BUFFER_SIZE);

        if (error < 0) {
          std::cout << "error initial write " << snd_strerror(error)
                    << std::endl;

          if (try_recover_pipe(handle, error) < 0) {
            std::cout << "could not recover pipe\n";
            exit(-1);
          }
        }
      } while (error < 0);

      if (error != BUFFER_SIZE) {
        std::cout << "initial write unexpected amount\n";
      }
    }
  }
  // not started yet, but ring buffer populated
  if (snd_pcm_state(handle) == SND_PCM_STATE_PREPARED) {
    error = snd_pcm_start(handle);
    if (error < 0) {
      std::cout << "error starting handle " << snd_strerror(error) << std::endl;
      exit(-1);
    }
  }
  // we don't really expect to exit this loop for now
  // so since async callback is doing work in the background
  // this thread can simply suspend
  while (true) {
    sleep(1);
  }

  snd_pcm_drain(handle);
  snd_pcm_close(handle);
}

void SoundPlayer::startRecording() {
  temp_output_buffer.open(".tmp_recording");
  is_recording = true;
}

void SoundPlayer::stopRecording() {
  is_recording = false;
  temp_output_buffer.close();
}
