#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>

#include "ugui/svggui.h"

enum WinPenHapticTool {
  PEN_HAPTIC_NONE = 0,
  PEN_HAPTIC_INK,
  PEN_HAPTIC_PENCIL,
  PEN_HAPTIC_MARKER,
  PEN_HAPTIC_BRUSH,
  PEN_HAPTIC_CHISEL,
  PEN_HAPTIC_ERASER
};

void initTouchInput(SDL_Window* sdlwin, bool useWintab);
//bool touchInputFilter(SDL_Event* event);
void setDPIAware();
Image getClipboardImage();
std::string winOpenPdfDialog(SDL_Window* sdlwin, const char* initialDir = NULL);
int winRunCommand(const std::string& cmdLine, std::string* output = NULL);
void winSetPenHapticsConfig(bool enabled, double intensity01);
void winSetPenHapticsTool(WinPenHapticTool tool);
void winTriggerPenHapticClick();
