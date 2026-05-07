# Desktop mirror right-eye backbuffer patch

PR marker update.

The intended source change is to mirror the selected VR eye into the desktop D3D9 swapchain backbuffer before Present. See the previous version of this file in main for the full patch notes.

Config:

```txt
DesktopMirrorEnabled=true
DesktopMirrorEye=right
DesktopMirrorKeepAspect=true
DesktopMirrorLinearFilter=true
```
