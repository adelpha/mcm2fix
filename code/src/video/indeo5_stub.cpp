// Used when mcm2fix is built without FFmpeg (e.g. with Visual Studio): no
// Indeo 5 decoder, so the game's videos are skipped.
#include "indeo5.h"

namespace video {

std::unique_ptr<Indeo5Decoder> CreateIndeo5Decoder(int, int) { return nullptr; }

}  // namespace video
