// ControllerHub without SDL3 (built without -DMCM2MaX_SDL3): never available.
#include "pad_hub.h"

namespace hub {

bool Available() { return false; }
bool Init(bool, const char*) { return false; }
void SetKeyboard(bool, const char* const[8]) {}
void Pump() {}
bool Read(PadState* out, DWORD* packet) {
  *out = PadState{};
  *packet = 0;
  return false;
}
bool ReadAll(std::vector<PadState>* out) {
  out->clear();
  return false;
}
bool ReadSticks(std::vector<StickState>* out) {
  out->clear();
  return false;
}
void Rumble(WORD, WORD) {}
std::vector<std::pair<WORD, WORD>> GamepadIds() { return {}; }
Family ActiveFamily() { return kXbox; }
std::string ActiveName() { return ""; }
int Count() { return 0; }
void LogSummary() {}

}  // namespace hub
