// SPDX-License-Identifier: Apache-2.0
// Midgard's built-in UI fonts (engine/ui/fonts, SIL Open Font License 1.1), compiled into the
// engine so every game, and every test, has them.
#pragma once

#include <cstdint>
#include <span>

namespace midgard::ui::fonts {

std::span<const uint8_t> inconsolata();           // the default: Inconsolata Medium
std::span<const uint8_t> opendyslexic_regular();  // OpenDyslexic 3
std::span<const uint8_t> opendyslexic_bold();

}  // namespace midgard::ui::fonts
