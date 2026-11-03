// SPDX-License-Identifier: Apache-2.0
#include "reverb.h"

#include <algorithm>
#include <cmath>

namespace midgard::audio {
namespace {

// Freeverb's tuning (in samples at 44.1 kHz; the right side's lines a little longer).
constexpr std::array<int, 8> kCombs = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
constexpr std::array<int, 4> kAllpasses = {556, 441, 341, 225};
constexpr int kStereoSpread = 23;
constexpr float kInputGain = 0.015f;
constexpr float kAllpassFeedback = 0.5f;
constexpr float kWetScale = 3.0f;  // (Freeverb's: its input is scaled down by kInputGain)

void process(ma_node* node, const float** in, ma_uint32* frames_in, float** out,
             ma_uint32* frames_out) {
    auto* n = reinterpret_cast<ReverbNode*>(node);
    const ma_uint32 frames = *frames_out;
    std::copy(in[0], in[0] + frames * 2, out[0]);  // (the sound itself, then its echo added)
    const float room = n->room_size.load(std::memory_order_relaxed);
    const float damping = n->damping.load(std::memory_order_relaxed);
    const float width = n->width.load(std::memory_order_relaxed);
    if (room != n->applied_room || damping != n->applied_damping || width != n->applied_width) {
        n->verb->set(room, damping, width);
        n->applied_room = room;
        n->applied_damping = damping;
        n->applied_width = width;
    }
    // The echo's level eases to its new one over a twentieth of a second.
    const float target = n->wet.load(std::memory_order_relaxed);
    const float step = float(frames) / (0.05f * float(n->sample_rate));
    const float from = n->wet_now;
    n->wet_now = from < target ? std::min(target, from + step) : std::max(target, from - step);
    if (from > 0.0f || n->wet_now > 0.0f) n->verb->process(in[0], out[0], frames, from, n->wet_now);
    *frames_in = frames;
}

ma_node_vtable g_vtable = {process, nullptr, 1, 1, 0};

}  // namespace

Freeverb::Freeverb(unsigned sample_rate) {
    const double scale = double(sample_rate) / 44100.0;
    for (int side = 0; side < 2; ++side) {
        for (size_t i = 0; i < kCombs.size(); ++i)
            combs_[side][i].buffer.assign(
                size_t(std::max(1.0, (kCombs[i] + side * kStereoSpread) * scale)), 0.0f);
        for (size_t i = 0; i < kAllpasses.size(); ++i)
            allpasses_[side][i].buffer.assign(
                size_t(std::max(1.0, (kAllpasses[i] + side * kStereoSpread) * scale)), 0.0f);
    }
}

void Freeverb::set(float room_size, float damping, float width) {
    feedback_ = std::clamp(room_size, 0.0f, 1.0f) * 0.28f + 0.7f;
    damp_ = std::clamp(damping, 0.0f, 1.0f) * 0.4f;
    width_ = std::clamp(width, 0.0f, 1.0f);
}

void Freeverb::process(const float* in, float* out, unsigned frames, float wet_from, float wet_to) {
    for (unsigned f = 0; f < frames; ++f) {
        const float wet = wet_from + (wet_to - wet_from) * (float(f) + 1.0f) / float(frames);
        const float input = (in[f * 2] + in[f * 2 + 1]) * kInputGain;
        float sides[2] = {0.0f, 0.0f};
        for (int side = 0; side < 2; ++side) {
            float sum = 0.0f;
            for (Comb& c : combs_[side]) {
                const float y = c.buffer[c.at];
                c.store = y * (1.0f - damp_) + c.store * damp_;
                c.buffer[c.at] = input + c.store * feedback_;
                if (++c.at == c.buffer.size()) c.at = 0;
                sum += y;
            }
            for (Allpass& a : allpasses_[side]) {
                const float held = a.buffer[a.at];
                const float y = held - sum;
                a.buffer[a.at] = sum + held * kAllpassFeedback;
                if (++a.at == a.buffer.size()) a.at = 0;
                sum = y;
            }
            sides[side] = sum;
        }
        const float wet1 = kWetScale * wet * (width_ / 2.0f + 0.5f);
        const float wet2 = kWetScale * wet * ((1.0f - width_) / 2.0f);
        out[f * 2] += sides[0] * wet1 + sides[1] * wet2;
        out[f * 2 + 1] += sides[1] * wet1 + sides[0] * wet2;
    }
}

ma_result reverb_node_init(ma_node_graph* graph, unsigned sample_rate, ReverbNode* node) {
    const ma_uint32 channels[1] = {2};
    ma_node_config config = ma_node_config_init();
    config.vtable = &g_vtable;
    config.pInputChannels = channels;
    config.pOutputChannels = channels;
    node->sample_rate = sample_rate;
    node->verb = new Freeverb(sample_rate);
    const ma_result r = ma_node_init(graph, &config, nullptr, &node->base);
    if (r != MA_SUCCESS) {
        delete node->verb;
        node->verb = nullptr;
    }
    return r;
}

}  // namespace midgard::audio
