// MenuMaX: the game's own menu art, read from its UI\<NAME>.DAT archives
// while the game runs (nothing of the game's is shipped).
//
// A screen names a sprite as "/dat/<ARCHIVE>/<path in the archive>", e.g.
// image(/dat/OPTBASE/Options/OptTab01.tga). Options after a '?', joined by
// '&':
//   black     pure black is see-through too (sprites the game draws with a
//             colour key, e.g. the Options tabs)
//   opaque    nothing is see-through (no magenta key)
//   frame=N   only the outer N pixels are kept (a border-only box plate)
//   bar       a slider track's bar only, without its tick marks (the rows
//             above the first row that is see-through in the middle)
// Magenta (the game's colour key) is always see-through unless "opaque".
// A PNG of the same name under UI\MaX\dat\ replaces the sprite (HD art).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ui {

// rel: "<ARCHIVE>/<path>[?options]" (the part after "dat/"). Fills straight
// alpha B, G, R, A rows. Thread: the caller's (reads files).
bool LoadGameArt(const std::string& rel, std::vector<uint8_t>& bgra, unsigned& w, unsigned& h, std::string* err);

}  // namespace ui
