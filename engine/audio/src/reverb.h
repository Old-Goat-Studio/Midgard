// SPDX-License-Identifier: Apache-2.0
// The mixer's reverb: Freeverb (Jezar at Dreampoint's public-domain design: eight damped comb
// filters and four all-passes a side) as a miniaudio node in line, stereo in, the sound with its
// echo added out (in line rather than on a send: a splitter's cache would delay every sound). Its
// settings are set from the game's thread and read on the audio thread; the echo's level eases
// to a new one over a twentieth of a second, so moving between zones never clicks.
#pragma once

#include <array>
#include <atomic>
#include <vector>

#include "opus_backend.h"  // (miniaudio, quietly)

namespace midgard::audio {

class Freeverb {
public:
    explicit Freeverb(unsigned sample_rate);
    void set(float room_size, float damping, float width);
    // Adds the echo of `in` (interleaved stereo) times `wet` to `out`.
    void process(const float* in, float* out, unsigned frames, float wet_from, float wet_to);

private:
    struct Comb {
        std::vector<float> buffer;
        size_t at = 0;
        float store = 0.0f;
    };
    struct Allpass {
        std::vector<float> buffer;
        size_t at = 0;
    };
    std::array<std::array<Comb, 8>, 2> combs_;
    std::array<std::array<Allpass, 4>, 2> allpasses_;
    float feedback_ = 0.84f, damp_ = 0.2f, width_ = 1.0f;
};

struct ReverbNode {
    ma_node_base base;  // (first: miniaudio treats this as the node)
    Freeverb* verb = nullptr;
    std::atomic<float> room_size{0.5f}, damping{0.5f}, wet{0.0f}, width{1.0f};
    float wet_now = 0.0f;  // (the audio thread's, easing to wet)
    float applied_room = -1.0f, applied_damping = -1.0f, applied_width = -1.0f;
    unsigned sample_rate = 48000;
};

// Makes the node (2 channels in and out) in a node graph; uninit with ma_node_uninit, then
// delete its verb.
ma_result reverb_node_init(ma_node_graph* graph, unsigned sample_rate, ReverbNode* node);

}  // namespace midgard::audio
