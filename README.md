# L4D2 VR Prototype
### Use this mod at your own risk of getting VAC banned. Use the -insecure launch option to help protect yourself. (Also contains lots of flashing lights)

#### [Video demo](https://www.youtube.com/watch?v=zU-8-9qe6wQ)

## Things that work
* Singleplayer and multiplayer (see below)
* 6DoF VR view
* Motion controls for guns and melee weapons
* Desktop mirror
* Workshop content
* Multi-core rendering support(Set it in L4D2VRConfigTool.exe, not in-game.When enabled, moving the HMD causes ghosting, so only seated play is supported.)
* AntiAliasing support(Set it in L4D2VRConfigTool.exe, not in-game.)
  
## Things that need fixing
* Interactions and throwables require you to aim with your face(Except for servers that do not support non‑VR)
* 
## How to play multiplayer
* You can join any server to play, but if the server wasn't created by VR some VR-exclusive features. 
* Versus works but it's barely been tested.
* 
## How to use
1. Download [L4D2VR.zip](https://github.com/liu547161153/l4d2vr/releases) and extract the files to your Left 4 Dead 2 directory (steamapps\common\Left 4 Dead 2)
2. Launch SteamVR, then launch Left 4 Dead 2 with these launch options:
   
   ``-heapsize 524288 -processheap -high -novid```

If you use a desktop client resolution of 2k or higher, add -bigfonts to the launch options to make in-game text larger; otherwise the text on the HUD will be very small.

3. Join or create your campaign and enjoy the game.
4. To recenter the camera height, press down on the left stick. To see the HUD, aim the left controller up or down.




## Troubleshooting
If the game isn't loading in VR:
* Disable SteamVR theater in [Steam settings](https://external-preview.redd.it/1WdLExouo_YKhTGT6C5GGrOjeWO7qNdIdDRvIRBhw-0.png?auto=webp&s=0d4447a9d954e1ec15b2c010cf50eeabd51f4197)

If the game shows "Failed to create D3D device!":
* L4D2VR uses a custom `d3d9.dll` based on DXVK. Even though the error says D3D, the failure is often the Vulkan backend failing to initialize.
* Update the GPU driver from NVIDIA, AMD, or Intel. Windows Update drivers are often too old for DXVK 2.x.
* Make sure the GPU and driver support Vulkan 1.3. Very old GPUs and some virtual/remote desktop display adapters will not work.
* Do not launch the Steam "Vulkan" launch option and do not add `-vulkan`. Launch the normal DirectX 9 game with the L4D2VR files installed.
* Remove forced display launch options such as `-w`, `-h`, `-fullscreen`, or unusual refresh-rate settings, then try windowed/default video settings.
* Check `left4dead2_d3d9.log` next to `left4dead2.exe`; lines such as "No adapters found" or "A Vulkan 1.3 capable driver is required" indicate a driver/GPU support problem.

If the game is stuttering, try: 
* Steam Settings -> Shader Pre-Caching -> Allow background processing of Vulkan shaders

If the game is crashing, try:
* Lowering video settings
* Disabling all add-ons then verifying integrity of game files
* Re-installing the game

## Build instructions
1. ```git clone --recurse-submodules https://github.com/liu547161153/l4d2vr.git ```
2. Initialize submodules:
   ```powershell
   git submodule update --init --recursive
   ```
3. Run the fixed build script (locks target to `Release|x86`):
   ```powershell
   .\build_release_x86.ps1
   ```
   or:
   ```cmd
   build_release_x86.cmd
   ```
4. (Optional/manual) Open l4d2vr.sln and build `Release|x86`.

Note: After building, it will attempt to copy the new d3d9.dll to your L4D2 directory.

## Dev note: VTable lookup
For quick vtable inspection, use [asherkin's VTable Dumper](https://asherkin.github.io/vtable/).
It can be used for `server.dll`-side symbols and also for `engine.dll` targets (drop the binary and search symbol names like `bob`, `viewmodel`, etc.).

## Utilizes code from
* [VirtualFortress2](https://github.com/PinkMilkProductions/VirtualFortress2)
* [gmcl_openvr](https://github.com/Planimeter/gmcl_openvr/)
* [DXVK](https://github.com/doitsujin/dxvk)
* [source-sdk-2013](https://github.com/ValveSoftware/source-sdk-2013/)
