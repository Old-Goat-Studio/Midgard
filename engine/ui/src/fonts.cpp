// SPDX-License-Identifier: Apache-2.0
#include "midgard/ui/fonts.h"

#include <cstddef>

namespace midgard::embedded {
extern const std::uint8_t font_inconsolata[];
extern const std::size_t font_inconsolata_size;
extern const std::uint8_t font_opendyslexic_regular[];
extern const std::size_t font_opendyslexic_regular_size;
extern const std::uint8_t font_opendyslexic_bold[];
extern const std::size_t font_opendyslexic_bold_size;
}  // namespace midgard::embedded

namespace midgard::ui::fonts {

std::span<const uint8_t> inconsolata() {
    return {embedded::font_inconsolata, embedded::font_inconsolata_size};
}
std::span<const uint8_t> opendyslexic_regular() {
    return {embedded::font_opendyslexic_regular, embedded::font_opendyslexic_regular_size};
}
std::span<const uint8_t> opendyslexic_bold() {
    return {embedded::font_opendyslexic_bold, embedded::font_opendyslexic_bold_size};
}

}  // namespace midgard::ui::fonts
