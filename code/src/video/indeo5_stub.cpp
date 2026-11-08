// Placeholder until the Indeo 5 decoder is added: the player then falls
// back to the game's DirectShow path.
#include "indeo5.h"

namespace video {

std::unique_ptr<Indeo5Decoder> CreateIndeo5Decoder(int, int) { return nullptr; }

}  // namespace video
