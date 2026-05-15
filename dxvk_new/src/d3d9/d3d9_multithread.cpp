#include "d3d9_device.h"

namespace dxvk {

  std::atomic<bool> g_l4d2vrForceDeviceLock { false };

  D3D9Multithread::D3D9Multithread(
          BOOL                  Protected)
    : m_protected( Protected ) { }

}
extern "C" void L4D2VR_D3D9_SetForceDeviceLock(int enabled) {
  dxvk::g_l4d2vrForceDeviceLock.store(enabled != 0, std::memory_order_relaxed);
}
