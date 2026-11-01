// The voiceprint of the speaker, on the models that the firmware of the speaker already has (/system/vendor/quasar/bio_lingware: an ECAPA-TDNN voiceprint of 512 numbers, a classifier of
// age and one of gender; the files are read there at run time and never copied). Speech in, one vector out: two utterances of one person give vectors that point the same way
// (cosine about 0.85 and above), of two people clearly less (0.66 and below is "somebody else" in the thresholds of the firmware).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "voice/tflite_mini.h"

namespace cs {

class VoicePrint {
 public:
  static constexpr int kDim = 512;
  bool load(const std::string &dir, std::string *err);
  bool loaded() const { return body_.loaded() && head_.loaded(); }
  // pcm: 16 kHz, 16 bit, mono. Needs about half a second of speech at least (20 frames). The vector is returned as it is (not normalised); age and gender are the
  // raw answers of the two small classifiers (0..1) when the models are there, else -1.
  bool embed(const int16_t *pcm, size_t n, std::vector<float> *emb, float *age, float *gender, std::string *err);
  static float cosine(const std::vector<float> &a, const std::vector<float> &b);

 private:
  TfliteMini body_, head_, age_, gender_;
  std::vector<float> mel_;     // 80 x 513 weights of the mel filters
  void fbank(const int16_t *pcm, size_t n, std::vector<float> *feat, int *frames) const;
};

}  // namespace cs
