# Desktop mirror right-eye backbuffer patch

This artifact is for `liu547161153/l4d2vr`.

Goal: mirror the VR right-eye render target into the desktop D3D9 swapchain backbuffer before `Present()`, so the game window shows the right-eye view after entering a map.

## Config

Add to `config.txt`:

```txt
DesktopMirrorEnabled=true
DesktopMirrorEye=right
DesktopMirrorKeepAspect=true
DesktopMirrorLinearFilter=true
```

## `dxvk_new/src/d3d9/d3d9_device.cpp`

Apply this patch. It moves the existing VR eye resolve / D3D aim-line overlay work before desktop `Present()`, then blits the selected eye to the implicit swapchain backbuffer.

```diff
--- /tmp/orig_d3d9.cpp	2026-05-07 18:25:01.026540159 +0000
+++ /tmp/mod_d3d9.cpp	2026-05-07 18:25:01.096010346 +0000
@@ -343,6 +343,90 @@
             device->EndScene();
         }
 
+        static void VrResolveSurfaceToSubmit(
+            D3D9DeviceEx* device,
+            IDirect3DSurface9* source,
+            IDirect3DSurface9* submitTarget) {
+            if (!device || !source || !submitTarget || source == submitTarget)
+                return;
+
+            HRESULT resolveResult = device->StretchRect(source, nullptr, submitTarget, nullptr, D3DTEXF_NONE);
+            if (FAILED(resolveResult))
+                Logger::warn(str::format("VR eye MSAA resolve to submit texture failed: 0x", std::hex, resolveResult));
+        }
+
+        static RECT VrComputeDesktopMirrorDestRect(
+            const D3DSURFACE_DESC& sourceDesc,
+            const D3DSURFACE_DESC& backBufferDesc,
+            bool keepAspect) {
+            RECT dst{};
+            dst.left = 0;
+            dst.top = 0;
+            dst.right = static_cast<LONG>(backBufferDesc.Width);
+            dst.bottom = static_cast<LONG>(backBufferDesc.Height);
+
+            if (!keepAspect || sourceDesc.Width == 0 || sourceDesc.Height == 0 ||
+                backBufferDesc.Width == 0 || backBufferDesc.Height == 0)
+                return dst;
+
+            const double srcAspect = static_cast<double>(sourceDesc.Width) / static_cast<double>(sourceDesc.Height);
+            const double dstAspect = static_cast<double>(backBufferDesc.Width) / static_cast<double>(backBufferDesc.Height);
+            if (!std::isfinite(srcAspect) || !std::isfinite(dstAspect) || srcAspect <= 0.0 || dstAspect <= 0.0)
+                return dst;
+
+            if (dstAspect > srcAspect) {
+                const LONG width = static_cast<LONG>(std::lround(static_cast<double>(backBufferDesc.Height) * srcAspect));
+                dst.left = (static_cast<LONG>(backBufferDesc.Width) - width) / 2;
+                dst.right = dst.left + width;
+            }
+            else if (dstAspect < srcAspect) {
+                const LONG height = static_cast<LONG>(std::lround(static_cast<double>(backBufferDesc.Width) / srcAspect));
+                dst.top = (static_cast<LONG>(backBufferDesc.Height) - height) / 2;
+                dst.bottom = dst.top + height;
+            }
+
+            return dst;
+        }
+
+        static void VrMirrorEyeToDesktopBackBuffer(D3D9DeviceEx* device, VR* vr) {
+            if (!device || !vr || !vr->m_DesktopMirrorEnabled)
+                return;
+
+            IDirect3DSurface9* source = nullptr;
+            if (vr->m_DesktopMirrorEye == 0)
+                source = vr->m_D9LeftEyeSubmitSurface ? vr->m_D9LeftEyeSubmitSurface : vr->m_D9LeftEyeSurface;
+            else
+                source = vr->m_D9RightEyeSubmitSurface ? vr->m_D9RightEyeSubmitSurface : vr->m_D9RightEyeSurface;
+
+            if (!source)
+                return;
+
+            IDirect3DSurface9* backBuffer = nullptr;
+            HRESULT hr = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
+            if (FAILED(hr) || !backBuffer)
+                return;
+
+            D3DSURFACE_DESC sourceDesc{};
+            D3DSURFACE_DESC backBufferDesc{};
+            const bool haveSourceDesc = SUCCEEDED(source->GetDesc(&sourceDesc));
+            const bool haveBackBufferDesc = SUCCEEDED(backBuffer->GetDesc(&backBufferDesc));
+
+            const RECT dstRect = (haveSourceDesc && haveBackBufferDesc)
+                ? VrComputeDesktopMirrorDestRect(sourceDesc, backBufferDesc, vr->m_DesktopMirrorKeepAspect)
+                : RECT{ 0, 0, 0, 0 };
+
+            const RECT* dstRectPtr = (dstRect.right > dstRect.left && dstRect.bottom > dstRect.top) ? &dstRect : nullptr;
+            const D3DTEXTUREFILTERTYPE filter = vr->m_DesktopMirrorLinearFilter ? D3DTEXF_LINEAR : D3DTEXF_NONE;
+            hr = device->StretchRect(source, nullptr, backBuffer, dstRectPtr, filter);
+            if (FAILED(hr) && filter != D3DTEXF_NONE)
+                hr = device->StretchRect(source, nullptr, backBuffer, dstRectPtr, D3DTEXF_NONE);
+
+            if (FAILED(hr))
+                Logger::warn(str::format("VR desktop mirror StretchRect failed: 0x", std::hex, hr));
+
+            backBuffer->Release();
+        }
+
     }
 
     D3D9DeviceEx::D3D9DeviceEx(
@@ -4498,18 +4582,12 @@
             }
         }
 
-        HRESULT result = m_implicitSwapchain->Present(
-            pSourceRect,
-            pDestRect,
-            hDestWindowOverride,
-            pDirtyRegion,
-            dwFlags);
-
         if (g_Game && g_Game->m_VR && g_Game->m_VR->m_CreatedVRTextures) {
             VR* vr = g_Game->m_VR;
             const bool inGame = (g_Game->m_EngineClient && g_Game->m_EngineClient->IsInGame());
             const bool queued = (g_Game->GetMatQueueMode() != 0);
 
+
             // Queued/multicore only: if submit side is reusing stale rendered frames, wait a tiny budget
             // for a fresh render->submit handoff event. Adaptive strategy keeps default path at 0ms wait.
             if (queued && inGame && vr->m_RenderFrameReadyEvent && vr->m_QueuedSubmitWaitMs > 0) {
@@ -4537,6 +4615,40 @@
                     vr->m_QueuedSubmitStaleStreak.store(0, std::memory_order_release);
             }
 
+            // The desktop swapchain is presented below. Do all eye-surface resolve and optional
+            // right/left-eye desktop mirroring before Present(), otherwise the window shows the old
+            // backbuffer for this frame.
+            if (!inGame) {
+                vr->ClearD3DAimLineOverlay();
+            }
+            else if (vr->m_D3DAimLineOverlayEnabled && !queued) {
+                if (vr->m_D9LeftEyeSurface)
+                    VrAimLineDrawOverlayToSurface(this, vr, 0, vr->m_D9LeftEyeSurface);
+                if (vr->m_D9RightEyeSurface)
+                    VrAimLineDrawOverlayToSurface(this, vr, 1, vr->m_D9RightEyeSurface);
+            }
+
+            if (vr->m_D9LeftEyeSubmitSurface)
+                VrResolveSurfaceToSubmit(this, vr->m_D9LeftEyeSurface, vr->m_D9LeftEyeSubmitSurface);
+            if (vr->m_D9RightEyeSubmitSurface)
+                VrResolveSurfaceToSubmit(this, vr->m_D9RightEyeSurface, vr->m_D9RightEyeSubmitSurface);
+
+            VrMirrorEyeToDesktopBackBuffer(this, vr);
+        }
+
+        HRESULT result = m_implicitSwapchain->Present(
+            pSourceRect,
+            pDestRect,
+            hDestWindowOverride,
+            pDirtyRegion,
+            dwFlags);
+
+        if (g_Game && g_Game->m_VR && g_Game->m_VR->m_CreatedVRTextures) {
+            VR* vr = g_Game->m_VR;
+            const bool inGame = (g_Game->m_EngineClient && g_Game->m_EngineClient->IsInGame());
+            const bool queued = (g_Game->GetMatQueueMode() != 0);
+
+
             if (vr->m_RenderPipelineDebugLog) {
                 static DWORD s_lastRenderPipelinePresentLogMs = 0;
                 const DWORD nowMs = ::GetTickCount();
@@ -4561,29 +4673,6 @@
                 }
             }
 
-            auto resolveVrSurfaceToSubmit = [this](IDirect3DSurface9* source, IDirect3DSurface9* submitTarget) {
-                if (source == nullptr || submitTarget == nullptr || source == submitTarget)
-                    return;
-
-                HRESULT resolveResult = StretchRect(source, nullptr, submitTarget, nullptr, D3DTEXF_NONE);
-                if (FAILED(resolveResult))
-                    Logger::warn(str::format("VR eye MSAA resolve to submit texture failed: 0x", std::hex, resolveResult));
-                };
-
-            if (!inGame) {
-                vr->ClearD3DAimLineOverlay();
-            }
-            else if (vr->m_D3DAimLineOverlayEnabled && !queued) {
-                if (vr->m_D9LeftEyeSurface)
-                    VrAimLineDrawOverlayToSurface(this, vr, 0, vr->m_D9LeftEyeSurface);
-                if (vr->m_D9RightEyeSurface)
-                    VrAimLineDrawOverlayToSurface(this, vr, 1, vr->m_D9RightEyeSurface);
-            }
-
-            if (vr->m_D9LeftEyeSubmitSurface)
-                resolveVrSurfaceToSubmit(vr->m_D9LeftEyeSurface, vr->m_D9LeftEyeSubmitSurface);
-            if (vr->m_D9RightEyeSubmitSurface)
-                resolveVrSurfaceToSubmit(vr->m_D9RightEyeSurface, vr->m_D9RightEyeSubmitSurface);
         }
 
         // Keep conservative sync behavior for stability.

```

## `L4D2VR/vr.h`

Insert these fields near the existing D3D eye texture/surface members, before the D3D eye-surface state is used by DXVK:

```cpp
	// Desktop-window mirror. This copies one VR eye into the implicit D3D9 swapchain
	// backbuffer before Present(), so the normal desktop game window can show a live
	// mirror without re-rendering the scene.
	//   DesktopMirrorEnabled=true/false
	//   DesktopMirrorEye=right/left or 1/0
	//   DesktopMirrorKeepAspect=true/false
	//   DesktopMirrorLinearFilter=true/false
	bool m_DesktopMirrorEnabled = true;
	int  m_DesktopMirrorEye = 1; // 0 = left eye, 1 = right eye
	bool m_DesktopMirrorKeepAspect = true;
	bool m_DesktopMirrorLinearFilter = true;
```

## `L4D2VR/vr/vr_viewmodel_config.inl`

Inside `VR::ParseConfigFile(...)`, after the helper lambdas (`getBool`, `getString`, etc.) are available and before the config parse finishes, add:

```cpp
    m_DesktopMirrorEnabled = getBool("DesktopMirrorEnabled", m_DesktopMirrorEnabled);
    {
        std::string eye = getString("DesktopMirrorEye", m_DesktopMirrorEye == 0 ? "left" : "right");
        std::transform(eye.begin(), eye.end(), eye.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (eye == "left" || eye == "0")
            m_DesktopMirrorEye = 0;
        else if (eye == "right" || eye == "1")
            m_DesktopMirrorEye = 1;
    }
    m_DesktopMirrorKeepAspect = getBool("DesktopMirrorKeepAspect", m_DesktopMirrorKeepAspect);
    m_DesktopMirrorLinearFilter = getBool("DesktopMirrorLinearFilter", m_DesktopMirrorLinearFilter);
```

## `L4D2VR/Options.cpp`

Add these defaults into `kAllowedDefaultsText`:

```txt
DesktopMirrorEnabled=true
DesktopMirrorEye=right
DesktopMirrorKeepAspect=true
DesktopMirrorLinearFilter=true
```

Add this visibility rule into `IsOptionVisible(...)`:

```cpp
    if (std::strcmp(key, "DesktopMirrorEye") == 0 ||
        std::strcmp(key, "DesktopMirrorKeepAspect") == 0 ||
        std::strcmp(key, "DesktopMirrorLinearFilter") == 0)
    {
        return IsEnabled("DesktopMirrorEnabled", true);
    }
```

Add these option entries in the option list near other rendering/display options:

```cpp
    {
        "DesktopMirrorEnabled",
        OPT_BOOL,
        { "Desktop Mirror", "桌面镜像" },
        { "Enable desktop eye mirror", "启用桌面端眼睛画面镜像" },
        { "Copies one VR eye to the normal desktop window backbuffer before Present().", "在 Present 前把一个 VR 眼睛画面复制到普通桌面窗口 backbuffer。" },
        { "Useful for recording or watching the VR view on the monitor without rendering the scene twice.", "用于录制或在显示器上观察 VR 画面，不需要额外重渲染一遍场景。" },
        "true"
    },
    {
        "DesktopMirrorEye",
        OPT_TEXT,
        { "Desktop Mirror", "桌面镜像" },
        { "Mirror eye", "镜像眼睛" },
        { "Which eye to show in the desktop window. Use right or left.", "桌面窗口显示哪只眼睛。填写 right 或 left。" },
        { "Default is right because it usually matches weapon/controller aiming better.", "默认 right，因为通常更接近武器/控制器瞄准画面。" },
        "right"
    },
    {
        "DesktopMirrorKeepAspect",
        OPT_BOOL,
        { "Desktop Mirror", "桌面镜像" },
        { "Keep aspect ratio", "保持比例" },
        { "Preserve the VR eye aspect ratio and letterbox/pillarbox when needed.", "保持 VR 单眼画面比例，必要时加黑边。" },
        { "Disable only if you want the mirror stretched to fill the window.", "只有想强制拉伸铺满窗口时才关闭。" },
        "true"
    },
    {
        "DesktopMirrorLinearFilter",
        OPT_BOOL,
        { "Desktop Mirror", "桌面镜像" },
        { "Linear filter", "线性过滤" },
        { "Use linear filtering when scaling the eye image to the desktop backbuffer.", "缩放眼睛画面到桌面 backbuffer 时使用线性过滤。" },
        { "If StretchRect fails on a driver/runtime path, the code falls back to point/no filtering automatically.", "如果某些驱动/运行时路径下 StretchRect 失败，代码会自动退回无过滤。" },
        "true"
    },
```

## Notes

The important ordering is: render eye surfaces -> resolve to submit surfaces -> mirror selected eye to desktop backbuffer -> desktop `Present()` -> OpenVR submit.

If the desktop window remains black, check that `DesktopMirrorEnabled=true` is actually loaded and that the DXVK DLL with the changed `d3d9_device.cpp` is the one being used by the game.
