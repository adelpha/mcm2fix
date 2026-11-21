// MenuMaX without RmlUi (built without -DMCM2MaX_RMLUI): the new screens are off.
#include "ui.h"

#include "../config.h"
#include "../patch.h"

namespace ui {
void Install() {
  if (GetConfig().menu_max) patch::Log("MenuMaX: this build has no RmlUi; the new menu screens are off");
}
bool Active() { return false; }
bool Open(const char*, const char*) { return false; }
void Close(const char*) {}
void CloseScreen(const char*, const char*) {}
bool IsOpen(const char*) { return false; }
void Present11(ID3D11RenderTargetView*, UINT, UINT, const RECT&, const dd::BlitParams*) {}
void Present9(IDirect3DSurface9*, UINT, UINT, const RECT&, const dd::BlitParams*) {}
void DeviceGone9() {}
void DialogMade(const char*) {}
bool HoldPresent() { return false; }
void KeyEvent(unsigned, bool) {}
void MouseButton(int, bool) {}
void MouseMove(int, int) {}
void MouseWheel(int) {}
void JoyState(DWORD, DWORD) {}
}  // namespace ui
