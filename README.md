# Write #

Cross-platform (Windows, Mac, Linux, iOS, Android) application for handwritten notes.

[styluslabs.com](http://styluslabs.com) | [Help](http://styluslabs.com/write/Help.html) | [FAQ](http://styluslabs.com/faq)

<img alt="Screenshot" src="https://github.com/user-attachments/assets/cc3f1690-073b-4c0e-81ce-f1b4e5ff9fff" width="768">


## Additions in This Fork ##

* **Hold-to-Shape with Drag-Resize:** Hold the pen at the end of a stroke to snap lines/rectangles/ellipses, then keep dragging before lifting to resize; includes improved rectangle detection.
* **Direct PDF Import:** Built-in PDF import dialog with **Light** (raster) and **Vector (Background)** modes plus custom `.svgz` output filename input. Looks for `pdf2lightwrite.cmd` / `pdf2write.cmd` alongside `Write.exe`, in `C:\Tools\`, or on `PATH`.
* **Surface Slim Pen 2 Haptics (Windows):** Direct Bluetooth LE HID haptic feedback supporting per-tool/eraser drag textures, adjustable intensity in Preferences → Input, and a tactile click pulse on hold-to-shape snap.
* **Hue-Preserving Soft Dark Page Mode:** Quick toolbar toggle button that renders white pages as soft charcoal (`#1E1E1E`) and dark workspace (`#121212`), inverts raster images/PDF pages cleanly, and preserves ink hues without altering saved `.svgz` files.

## Building ##

Checkout: `git clone --recurse-submodules https://github.com/silasl21/Write`

To build executable `syncscribble/Release/Write`:
* Linux: `cd syncscribble && make USE_SYSTEM_SDL=1`; On Debian/Ubuntu, `apt install build-essential libsdl2-dev`.  Copy fonts from scribbleres/fonts to syncscribble/Release before running Write.
* Android (on Linux): `cd syncscribble/android && ./gww installRelease`; to install Android SDK and NDK, run `gww --install-sdk`
* iOS:
 * build SDL: `cd SDL && git checkout write-mac && make -f ../scribbleres/SDL-Makefile.ios`
 * see [nanovgXC readme](https://github.com/styluslabs/nanovgXC?tab=readme-ov-file#example-app) for setup and then run `cd syncscribble && make` or use the [Xcode project](xcode/Write).
* macOS: `cd syncscribble && make MACOS=1`
* Windows:
 * install Visual Studio (free Community Edition is fine) and [GNU make for Windows](http://www.equation.com/servlet/equation.cmd?fa=make), ensuring it is available in the path
 * in `syncscribble/Makefile`, set `DEPENDBASE` to the parent folder containing all dependencies that `make` should track
 * build SDL: `cd SDL && git checkout write-win && make -f ../scribbleres/SDL-Makefile.msvc`
 * open a Visual Studio command prompt (from the Start Menu) and run `cd syncscribble && make`
