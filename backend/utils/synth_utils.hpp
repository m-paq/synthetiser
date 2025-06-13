#ifndef SYNTH_UTILS
#define SYNTH_UTILS

#include "../synthetiser/operator/operator.hpp"
#include "../synthetiser/oscillator/oscillator.hpp"
#include "note_map.hpp"
#include <alsa/asoundlib.h>
#include <atomic>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

void writeDataToBuffer(std::vector<short> *buffer, Oscillator *osc,
                       int nombreTick);

struct private_data {
  float *buffer;
  Operator *synth_operator;
  Operator *synth_operator2;
  Operator *synth_operator3;
  Operator *synth_operator4;
  NoteMap *note_map;
  std::mutex *map_mutex;
  std::ofstream *recording;
  std::atomic<bool> *is_recording;
};

void async_player_callback(snd_async_handler_t *ahandler);

int try_recover_pipe(snd_pcm_t *handle, int err);

void getDeviceProperties(const std::string &deviceID, unsigned int &minChansOut,
                         unsigned int &maxChansOut);

#endif
