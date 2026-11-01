#include "voice/voiceprint.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>

namespace cs {

namespace {

constexpr int kSr = 16000, kFrameLen = 640, kShift = 384, kFft = 1024, kBins = kFft / 2 + 1, kMel = 80, kChunk = 20, kWindow = 300;
constexpr double kPi = 3.14159265358979323846;

// An in-place radix-2 FFT of 1024 points.
void fft(std::complex<double> *x) {
  for (int i = 1, j = 0; i < kFft; i++) {
    int bit = kFft >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(x[i], x[j]);
  }
  for (int len = 2; len <= kFft; len <<= 1) {
    const double ang = -2.0 * kPi / len;
    const std::complex<double> wl(std::cos(ang), std::sin(ang));
    for (int i = 0; i < kFft; i += len) {
      std::complex<double> w(1.0, 0.0);
      for (int k = 0; k < len / 2; k++) {
        const std::complex<double> u = x[i + k], v = x[i + k + len / 2] * w;
        x[i + k] = u + v;
        x[i + k + len / 2] = u - v;
        w *= wl;
      }
    }
  }
}

double mel_of(double f) { return 1127.0 * std::log(1.0 + f / 700.0); }

}  // namespace

bool VoicePrint::load(const std::string &dir, std::string *err) {
  const std::string base = dir + "/tflite-online-ecapa-tdnn-frontend/";
  if (!body_.load(base + "body.tflite", err) || !head_.load(base + "head.tflite", err)) return false;
  std::string ignore;
  if (!age_.load(dir + "/tflite-linear-age-classifier/age_classifier.tflite", &ignore)) age_ = TfliteMini();         // the classifiers are a bonus
  if (!gender_.load(dir + "/tflite-linear-gender-classifier/gender_classifier.tflite", &ignore)) gender_ = TfliteMini();
  // the mel filters of Kaldi: triangles that are even on the mel scale, 0 Hz to the Nyquist frequency
  mel_.assign(size_t(kMel) * kBins, 0.f);
  const double mlo = mel_of(0.0), mhi = mel_of(kSr / 2.0), delta = (mhi - mlo) / (kMel + 1);
  for (int b = 0; b < kMel; b++) {
    const double left = mlo + b * delta, center = mlo + (b + 1) * delta, right = mlo + (b + 2) * delta;
    for (int k = 0; k < kBins; k++) {
      const double m = mel_of(double(k) * kSr / kFft);
      const double up = (m - left) / (center - left), down = (right - m) / (right - center);
      mel_[size_t(b) * kBins + size_t(k)] = float(std::max(0.0, std::min(up, down)));
    }
  }
  return true;
}

void VoicePrint::fbank(const int16_t *pcm, size_t n, std::vector<float> *feat, int *frames) const {
  *frames = n < size_t(kFrameLen) ? 0 : int(1 + (n - size_t(kFrameLen)) / size_t(kShift));
  feat->assign(size_t(*frames) * kMel, 0.f);
  static double window[kFrameLen];
  static bool made = false;
  if (!made) {
    for (int i = 0; i < kFrameLen; i++) window[i] = 0.54 - 0.46 * std::cos(2.0 * kPi * i / (kFrameLen - 1));
    made = true;
  }
  std::vector<std::complex<double>> buf(kFft);
  double fr[kFrameLen];
  for (int f = 0; f < *frames; f++) {
    const int16_t *p = pcm + size_t(f) * kShift;
    double mean = 0;
    for (int i = 0; i < kFrameLen; i++) mean += p[i];
    mean /= kFrameLen;
    for (int i = 0; i < kFrameLen; i++) fr[i] = p[i] - mean;                                   // remove the DC offset
    for (int i = kFrameLen - 1; i > 0; i--) fr[i] -= 0.97 * fr[i - 1];                         // pre-emphasis
    fr[0] -= 0.97 * fr[0];
    for (int i = 0; i < kFft; i++) buf[size_t(i)] = i < kFrameLen ? std::complex<double>(fr[i] * window[i], 0.0) : std::complex<double>(0.0, 0.0);
    fft(buf.data());
    double power[kBins];
    for (int k = 0; k < kBins; k++) power[k] = std::norm(buf[size_t(k)]);
    for (int b = 0; b < kMel; b++) {
      double e = 0;
      const float *w = &mel_[size_t(b) * kBins];
      for (int k = 0; k < kBins; k++) e += double(w[k]) * power[k];
      (*feat)[size_t(f) * kMel + size_t(b)] = float(std::log(std::max(e, 1.1920929e-07)));
    }
  }
}

bool VoicePrint::embed(const int16_t *pcm, size_t n, std::vector<float> *emb, float *age, float *gender, std::string *err) {
  if (!loaded()) {
    if (err) *err = "the voiceprint models are not loaded";
    return false;
  }
  std::vector<float> feat;
  int frames = 0;
  fbank(pcm, n, &feat, &frames);
  if (frames < kChunk) {
    if (err) *err = "too little speech for a voiceprint";
    return false;
  }
  // the body takes 20 frames at a time and gives 128 numbers for each; the head takes up to 300 of them (the last ones) and gives the voiceprint
  std::vector<float> seq;
  for (int s = 0; s + kChunk <= frames; s += kChunk) {
    std::vector<float> out;
    if (!body_.run({1, kChunk, kMel}, &feat[size_t(s) * kMel], &out, nullptr, err)) return false;
    seq.insert(seq.end(), out.begin(), out.end());
  }
  size_t t = seq.size() / 128;
  size_t from = t > size_t(kWindow) ? t - size_t(kWindow) : 0;
  std::vector<float> v;
  if (!head_.run({1, int(t - from), 128}, &seq[from * 128], &v, nullptr, err)) return false;
  if (int(v.size()) != kDim) {
    if (err) *err = "the head gave an unexpected size";
    return false;
  }
  *emb = v;
  std::vector<float> o;
  if (age) *age = age_.loaded() && age_.run({1, kDim}, v.data(), &o, nullptr, nullptr) && !o.empty() ? o[0] : -1.f;
  if (gender) *gender = gender_.loaded() && gender_.run({1, kDim}, v.data(), &o, nullptr, nullptr) && !o.empty() ? o[0] : -1.f;
  return true;
}

float VoicePrint::cosine(const std::vector<float> &a, const std::vector<float> &b) {
  double d = 0, na = 0, nb = 0;
  for (size_t i = 0; i < a.size() && i < b.size(); i++) {
    d += double(a[i]) * b[i];
    na += double(a[i]) * a[i];
    nb += double(b[i]) * b[i];
  }
  return na > 0 && nb > 0 ? float(d / std::sqrt(na * nb)) : 0.f;
}

}  // namespace cs
