# libntr

> **This is a fork** of [cybervisi0n/libntr](https://github.com/cybervisi0n/libntr) used by
> [pokeplatinum-thor](https://github.com/SilasMayes/pokeplatinum-thor) and a HeartGold/SoulSilver port. The
> `thor-android` branch adds an Android target with an OpenGL ES 3.2 renderer, the second-screen output and
> companion panel for dual-screen devices such as the AYN Thor, and the settings UI those ports use.
> cybervisi0n isn't involved in this fork, so please report problems with it here. The original MIT license and
> copyright are unchanged (see [LICENSE](LICENSE)).

![logo](images/libntr.png)
Drop-in replacement for NitroSDK that targets Nintendo DS/DSi, PC, and other platforms.

libntr makes it easier to port Nintendo DS/DSi games to PC and other platforms by handling the logic for common libraries used by nearly all Nintendo DS games.

See also: 
 * [libntrsystem](https://github.com/cybervisi0n/libntrsystem)
 * [libntrwifi](https://github.com/cybervisi0n/libntrwifi)
 * [libntrdwc](https://github.com/cybervisi0n/libntrdwc)

## Features
 * Fully compatible with NitroSDK
 * Translate DS 3D graphics APIs to OpenGL 4.2 with support for 3D upscaling
 * Simulate DS 2D graphics
 * Includes support for adding GUIs via Dear ImGui
 * Simulate local wireless multiplayer over LAN or the internet

## Available Build Targets
Select the build target with the 'build_target' meson option.
 * arm (Nintendo DS hardware)
 * linux (x86_64 Linux PC)
 * win64 (x86_64 Windows PC)
 * nx (Switch via libnx)

## DSi Support (BETA)
libntr supports building TWL applications. Set the meson options:
 * sdk_version_major=5
 * hardware_type=(twl_hyb) or (twl_ltd)
 * * twl_hyb: DSi enhanced application, supports running on original NDS and DSi
 * * twl_ltd: DSiWare, DSi only