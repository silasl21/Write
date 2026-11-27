#include "winhelper.h"
//#include "wintab/wmpointer.h" ... this was to support *compilation* on Win 7 and earlier
#include "basics.h"
#include "SDL_syswm.h"
#include <commdlg.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpropdef.h>
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}
#include <array>
#include <cmath>
#include <deque>
#include <map>
#include <vector>


#define USE_WINTAB 1

// always link WM_POINTER fns at runtime so we support *running* on Win 7 and earlier
#ifndef WMPOINTER_NEEDED
// use different names for fns if already defined
#define GetPointerInfo FnGetPointerInfo
#define GetPointerFrameInfo FnGetPointerFrameInfo
#define GetPointerPenInfo FnGetPointerPenInfo
#define GetPointerPenInfoHistory FnGetPointerPenInfoHistory
#define GetPointerDeviceRects FnGetPointerDeviceRects
#define InjectTouchInput FnInjectTouchInput
#define InitializeTouchInjection FnInitializeTouchInjection
#define WMPOINTER_NEEDED 1
#endif

#ifdef WMPOINTER_NEEDED
// WM_POINTER functions
typedef BOOL (WINAPI *PtrGetPointerInfo)(UINT32, POINTER_INFO*);
typedef BOOL (WINAPI *PtrGetPointerFrameInfo)(UINT32, UINT32*, POINTER_INFO*);
typedef BOOL (WINAPI *PtrGetPointerPenInfo)(UINT32, POINTER_PEN_INFO*);
typedef BOOL (WINAPI *PtrGetPointerPenInfoHistory)(UINT32, UINT32*, POINTER_PEN_INFO*);
typedef BOOL (WINAPI *PtrGetPointerDeviceRects)(HANDLE, RECT*, RECT*);
typedef BOOL (WINAPI *PtrInjectTouchInput)(UINT32, POINTER_TOUCH_INFO*);
typedef BOOL (WINAPI *PtrInitializeTouchInjection)(UINT32, DWORD);

static PtrGetPointerInfo GetPointerInfo = NULL;
static PtrGetPointerFrameInfo GetPointerFrameInfo;
static PtrGetPointerPenInfo GetPointerPenInfo;
static PtrGetPointerPenInfoHistory GetPointerPenInfoHistory;
static PtrGetPointerDeviceRects GetPointerDeviceRects;
static PtrInjectTouchInput InjectTouchInput;
static PtrInitializeTouchInjection InitializeTouchInjection;
#endif

#define MAX_N_POINTERS 10
static POINTER_INFO pointerInfo[MAX_N_POINTERS];
static POINTER_PEN_INFO penPointerInfo[MAX_N_POINTERS];
static bool winTabProximity = false;

enum TouchPointState { TouchPointPressed = SDL_FINGERDOWN, TouchPointMoved = SDL_FINGERMOTION, TouchPointReleased = SDL_FINGERUP };
enum PenEventType { TabletPress = SDL_FINGERDOWN, TabletMove = SDL_FINGERMOTION, TabletRelease = SDL_FINGERUP };

static void stopHapticTexture();
static void onPenHapticSeen();
static void onPenHapticInput(PenEventType eventtype, Dim x, Dim y, double pressure, bool inContact, bool isEraser);

struct TouchPoint
{
  Point screenPos;
  Dim pressure;
  TouchPointState state;
  uint32_t id;
};

static Rect clientRect;
static double g_dpiScale = 1.0;

static void updateClientRect(HWND hwnd)
{
  // same code that SDL uses for WM_TOUCH, so hopefully it will work
  RECT rect;
  if(!GetClientRect(hwnd, &rect) || (rect.right == rect.left && rect.bottom == rect.top))
    return;
  ClientToScreen(hwnd, (LPPOINT)&rect);
  ClientToScreen(hwnd, (LPPOINT)&rect + 1);
  clientRect = Rect::ltrb(rect.left, rect.top, rect.right, rect.bottom);
  HDC hdc = GetDC(hwnd);
  if(hdc) {
    int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
    if(dpi > 0)
      g_dpiScale = dpi / 96.0;
    ReleaseDC(hwnd, hdc);
  }
}

static void notifyTabletEvent(PenEventType eventtype, const Point& globalpos,
    Dim pressure, Dim tiltX, Dim tiltY, bool eraser, int button, int buttons, int deviceid, uint32_t t)
{
  // combine all extra buttons into RightButton
  //if (button & ~0x1) button = (button & 0x1) | 0x2;  if (buttons & ~0x1) buttons = (buttons & 0x1) | 0x2;

  SDL_Event event;
  SDL_zero(event);
  event.type = eventtype;
  event.tfinger.timestamp = t;  // SDL_GetTicks();  // normally done by SDL_PushEvent()
  event.tfinger.touchId = eraser ? PenPointerEraser : PenPointerPen;
  //event.tfinger.fingerId = 0;  // if SDL sees more than one finger id for a touch id, that's multitouch
  // POINTER_FLAGS >> 4 == SDL_BUTTON_LMASK for pen down w/ no barrel buttons pressed, as desired
  event.tfinger.fingerId = eventtype == SDL_FINGERMOTION ? buttons : button;
  event.tfinger.x = (globalpos.x - clientRect.left);  // / clientRect.width();
  event.tfinger.y = (globalpos.y - clientRect.top);  // / clientRect.height();
  // stick buttons in dx, dy for now
  event.tfinger.dx = tiltX;
  event.tfinger.dy = tiltY;
  event.tfinger.pressure = pressure;
  // PeepEvents bypasses gesture recognizer and event filters
  SDL_PeepEvents(&event, 1, SDL_ADDEVENT, 0, 0);  //SDL_PushEvent(&event);
}

static void notifyTouchEvent(TouchPointState touchstate, const std::vector<TouchPoint>& _points, uint32_t t)
{
  for (const TouchPoint& p : _points) {
    SDL_Event event;
    SDL_zero(event);
    event.type = p.state;
    event.tfinger.timestamp = t;  // SDL_GetTicks();  // normally done by SDL_PushEvent()
    event.tfinger.touchId = 0;
    event.tfinger.fingerId = p.id;
    event.tfinger.x = (p.screenPos.x - clientRect.left);  // /clientRect.width();
    event.tfinger.y = (p.screenPos.y - clientRect.top);  // /clientRect.height();
    event.tfinger.pressure = p.pressure;
    SDL_PeepEvents(&event, 1, SDL_ADDEVENT, 0, 0);  //SDL_PushEvent(&event);
  }
}

#ifdef USE_WINTAB
#include "wintab/utils.h"
#include "wintab/msgpack.h"

// these defines are required for pktdef.h
#define PACKETDATA (PK_X | PK_Y | PK_BUTTONS | PK_NORMAL_PRESSURE | PK_CURSOR)
//#define PACKETDATA (PK_X | PK_Y | PK_BUTTONS | PK_NORMAL_PRESSURE | PK_TANGENT_PRESSURE | PK_ORIENTATION | PK_CURSOR | PK_Z | PK_TIME)
#define PACKETMODE 0
#include "wintab/pktdef.h"

static constexpr int PACKET_BUFF_SIZE = 128;
static PACKET localPacketBuf[PACKET_BUFF_SIZE];
static HCTX hTab = NULL;

static Rect tabletArea;
static Rect desktopArea;
static int minPressure = 0, maxPressure = 0;
static DWORD btnPrev = 0;
//static SDL_Window* sdlWindow = NULL;

static void initWinTab(HWND hWnd)
{
  // attempt to load wintab
  if(!LoadWintab() || !gpWTInfoA || !gpWTInfoA(0, 0, NULL) || !gpWTPacketsGet)
    return;

  LOGCONTEXTA lc;
  // WTI_DEFSYSCTX has CXO_SYSTEM set to enable as "system cursor context" so pen moves cursor (WTI_DEFCONTEXT doesn't)
  gpWTInfoA(WTI_DEFSYSCTX, 0, &lc);

  lc.lcOptions |= CXO_MESSAGES;
  lc.lcPktData = PACKETDATA;
  lc.lcMoveMask = PACKETDATA;
  lc.lcPktMode = PACKETMODE;
  lc.lcBtnUpMask = lc.lcBtnDnMask;

  // set output range to input range to get full available precision (default output range is screen resolution)
  lc.lcOutOrgX = 0;
  lc.lcOutExtX = lc.lcInExtX;
  lc.lcOutOrgY = 0;
  lc.lcOutExtY = -lc.lcInExtY;  // invert Y axis to match screen coords
  tabletArea = Rect::ltwh(lc.lcOutOrgX, lc.lcOutOrgY, std::abs(lc.lcOutExtX), std::abs(lc.lcOutExtY));
  desktopArea = Rect::ltwh(lc.lcSysOrgX, lc.lcSysOrgY, lc.lcSysExtX, lc.lcSysExtY);

  hTab = gpWTOpenA(hWnd, &lc, TRUE);
  // set queue size
  gpWTQueueSizeSet(hTab, PACKET_BUFF_SIZE);
  PLATFORM_LOG("Wintab initialized.\n");
}

// Wintab references:
//  https://developer-docs.wacom.com/display/DevDocs/Windows+Wintab+Documentation
//  Qt: http://qt.gitorious.org/qt/qt/blobs/raw/4.8/src/gui/kernel/qapplication_win.cpp  qwidget_win.cpp
// WM_POINTER references:
//  http://msdn.microsoft.com/en-us/library/windows/desktop/hh454916(v=vs.85).aspx
//  http://software.intel.com/en-us/articles/comparing-touch-coding-techniques-windows-8-desktop-touch-sample
// Windows' "interaction context" stuff can be used with WM_POINTER to recognize gestures (see the
//  InteractionContextSample from Intel) but is a bit too high level, e.g., no way to select single finger
//  pan vs. two finger pan.  Also, we already have "gesture recognition" code

// WM_POINTER observations (from Win 8.0 on Microsoft Surface Pro):
// * Multiple touch points: we receive a move event with the two pointers before the press event for the second
// * Pen entering proximity when touch points are down: WM_POINTERUP is sent for each
//  touch point, but each GetPointerFrameInfo only returns a frame with one point, even for the first point
//  to go up.  WM_POINTERENTER is sent for the pen pointer AFTER touch points go up.
// The POINTER_INFO associated with the pointer up event is identical to a normal pointer up event,
//  except the POINTER_FLAG_CONFIDENCE flag is set, but there is no reason to think this is a reliable
//  indicator across devices.
// If we want to use relative timing, compare touch pointer up time to WM_POINTERENTER time

// Debugging w/ Visual Studio VM:
// 1. share C:\Program Files (x86)\Microsoft Visual Studio\2017\Community\Common7\IDE\Remote Debugger in VM
// 1. open share on host and run x64/msvsmon.exe
// 1. build Write and run on host
// 1. In VS: Debug -> Attach to Process..., enter host's network name or IP, choose Write process
// Things that don't work:
// - touchscreen.vusb.usePen = "TRUE" ... Win 8.1 crashes, Win 10 mouse breaks
// - connect Wacom device to VM ... fails, breaks pen and touch on host, requiring restart

static bool processWTPacket(MSG* msg)
{
  // primary barrel button is 0x2 for single button pen, but 0x4 with default config of two button pen!
  static DWORD tipBtn = 0x00000001;
  int numPackets = gpWTPacketsGet((HCTX)(msg->lParam), PACKET_BUFF_SIZE, &localPacketBuf);
  for(int ii = 0; ii < numPackets; ii++) {
    DWORD btnNew = localPacketBuf[ii].pkButtons;
    UINT pressureNew = localPacketBuf[ii].pkNormalPressure;
    // on my X1 Yoga, if barrel button is pressed before pen touches screen, tipBtn never gets set
    // remaining issue is that button release becomes pen up too ... remove btnNew != 0 condition?
    if(pressureNew && btnNew)
      btnNew |= tipBtn;
    PenEventType eventtype = TabletMove;
    if((btnNew & tipBtn) && !(btnPrev & tipBtn))
      eventtype = TabletPress;
    else if(!(btnNew & tipBtn) && (btnPrev & tipBtn))
      eventtype = TabletRelease;

    Dim globalx = ((localPacketBuf[ii].pkX - tabletArea.left) * desktopArea.width() / tabletArea.width()) + desktopArea.left;
    Dim globaly = ((localPacketBuf[ii].pkY - tabletArea.top) * desktopArea.height() / tabletArea.height()) + desktopArea.top;
    Dim pressure = btnNew ? pressureNew / Dim(maxPressure - minPressure) : 0;
    // docs say to check this upon WT_PROXIMITY ... but it can change afterwards too!
    bool eraser = (localPacketBuf[ii].pkCursor % 3 == 2);

    int uniqueId = 1;
    // TODO: enable and use pkTime field
    onPenHapticInput(eventtype, globalx, globaly, pressure, (btnNew & tipBtn) != 0, eraser);
    notifyTabletEvent(eventtype, Point(globalx, globaly), pressure, 0, 0,
        eraser, btnNew ^ btnPrev, btnNew, uniqueId, SDL_GetTicks());
    btnPrev = btnNew;
  }
  return true;
}

static bool winTabEvent(MSG* msg) //, long* result)
{
  switch(msg->message) {
  case WM_ACTIVATE:
    gpWTEnable(hTab, GET_WM_ACTIVATE_STATE(msg->wParam, msg->lParam));
    if(GET_WM_ACTIVATE_STATE(msg->wParam, msg->lParam))
      gpWTOverlap(hTab, TRUE);
    else {
      stopHapticTexture();
      SDL_EventState(SDL_MOUSEMOTION, SDL_ENABLE);
      SDL_EventState(SDL_MOUSEBUTTONUP, SDL_ENABLE);
      SDL_EventState(SDL_MOUSEBUTTONDOWN, SDL_ENABLE);
    }
    break;
  case WM_DESTROY:
    gpWTClose(hTab);
    break;
  case WT_PROXIMITY:
    // only handle proximity enter
    if(LOWORD(msg->lParam) != 0) {  // entering proximity
      LOGCONTEXTA lc;
      AXIS np;
      // get the current context
      gpWTGetA((HCTX)(msg->wParam), &lc);
      // this shouldn't be necessary - output range shouldn't change
      tabletArea = Rect::ltwh(lc.lcOutOrgX, lc.lcOutOrgY, std::abs(lc.lcOutExtX), std::abs(lc.lcOutExtY));
      // this doesn't seem to change (w/o closing and reopening Wintab) so we could do only in init
      desktopArea = Rect::ltwh(lc.lcSysOrgX, lc.lcSysOrgY, lc.lcSysExtX, lc.lcSysExtY);
      // get the size of the pressure axis
      gpWTInfoA(WTI_DEVICES + lc.lcDevice, DVC_NPRESSURE, &np);
      minPressure = int(np.axMin);
      maxPressure = int(np.axMax);
      // TODO: get uniqueID and other cursor info
      btnPrev = 0;
      // this seems to be the only way to get virtual screen size from Windows, but it doesn't work right w/o
      //  PROCESS_PER_MONITOR_DPI_AWARE set; I don't know what happens on earlier versions of Windows
      //int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

      updateClientRect(msg->hwnd);
      // WinTab generates WM_MOUSEMOVE (and thus SDL mouse events) for cursor context (CXO_SYSTEM set)
      // ... disable them to prevent ScribbleArea from changing cursor to mouse cursor
      SDL_EventState(SDL_MOUSEMOTION, SDL_DISABLE);
      SDL_EventState(SDL_MOUSEBUTTONUP, SDL_DISABLE);
      SDL_EventState(SDL_MOUSEBUTTONDOWN, SDL_DISABLE);
      winTabProximity = true;
      onPenHapticSeen();
    }
    else {  // leaving proximity
      stopHapticTexture();
      SDL_EventState(SDL_MOUSEMOTION, SDL_ENABLE);
      SDL_EventState(SDL_MOUSEBUTTONUP, SDL_ENABLE);
      SDL_EventState(SDL_MOUSEBUTTONDOWN, SDL_ENABLE);
      winTabProximity = false;
    }
    return true;
  case WT_PACKET:
    return processWTPacket(msg);
  case WM_DISPLAYCHANGE:
    // when resolution changes, Wintab seems to use updated resolution internally but LOGCONTEXT returned
    //  by WTGet isn't updated
    gpWTClose(hTab);
    initWinTab(msg->hwnd);
    break;
  default:
    break;
  } // switch
  return false; // propagate to next handler
}

#endif // Wintab

/* Use this code to debug problem with multiple screens:
      RECT pRect, dRect;
      GetPointerDeviceRects(pointerInfo[ii].sourceDevice, &pRect, &dRect);
      PLATFORM_LOG("Pointer device rect ltrb: %d %d %d %d; Display device rect: %d %d %d %d\n",
        pRect.left, pRect.top, pRect.right, pRect.bottom,
        dRect.left, dRect.top, dRect.right, dRect.bottom);
      PLATFORM_LOG("WMPointer touch at %d, %d; himetric %d, %d; clientRect origin: %f, %f\n",
        pointerInfo[ii].ptPixelLocation.x, pointerInfo[ii].ptPixelLocation.y,
        pointerInfo[ii].ptHimetricLocation.x, pointerInfo[ii].ptHimetricLocation.y,
        clientRect.left, clientRect.top);
*/

static constexpr USHORT SLIM_PEN_VID = 0x045E;
static constexpr USHORT SLIM_PEN_PID = 0x0C0F;
static constexpr BYTE WAVEFORM_LIST_REPORT_ID = 0x42;
static constexpr BYTE TRIGGER_REPORT_ID = 0x41;
static constexpr size_t TRIGGER_REPORT_LEN = 7;
static constexpr BYTE FIRST_WAVEFORM_ORDINAL = 3;

static constexpr UINT16 WF_NONE   = 0x1001;
static constexpr UINT16 WF_STOP   = 0x1002;
static constexpr UINT16 WF_CLICK  = 0x1003;
static constexpr UINT16 WF_INK    = 0x100B;
static constexpr UINT16 WF_PENCIL = 0x100C;
static constexpr UINT16 WF_MARKER = 0x100D;
static constexpr UINT16 WF_CHISEL = 0x100E;
static constexpr UINT16 WF_BRUSH  = 0x100F;
static constexpr UINT16 WF_ERASER = 0x1010;

static const DEVPROPKEY kDevPropInstanceId = {
  { 0x78c34fc8, 0x104a, 0x4aca, { 0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57 } }, 256
};
static const DEVPROPKEY kDevPropIsConnected = {
  { 0x83da6326, 0x97a6, 0x4088, { 0x94, 0x53, 0xa1, 0x92, 0x3f, 0x57, 0x3b, 0x29 } }, 15
};

static bool g_syncInited = false;
static CRITICAL_SECTION g_hapticCs;
static HANDLE g_hapticWakeEvent = NULL;
static HANDLE g_writerThread = NULL;
static bool g_shutdownRequested = false;
static bool g_connectRequested = false;

static bool g_hasDevice = false;
static bool g_lastWriteOk = true;
static std::map<UINT16, BYTE> g_ordinals;
static std::map<UINT16, UINT16> g_durations;
static std::deque<std::array<BYTE, TRIGGER_REPORT_LEN>> g_oneShots;
static bool g_hasPendingTexture = false;
static std::array<BYTE, TRIGGER_REPORT_LEN> g_pendingTexture = {};

static bool g_hapticsEnabled = true;
static double g_hapticIntensity = 0.75;
static WinPenHapticTool g_hapticTool = PEN_HAPTIC_INK;
static bool g_penEraserActive = false;

static bool g_penTipDown = false;
static bool g_hasLastPos = false;
static Dim g_lastLogicalX = 0;
static Dim g_lastLogicalY = 0;
static LONGLONG g_lastPosQpc = 0;
static double g_qpcFreq = 0.0;
static double g_travel = 0.0;
static double g_speed = 0.0;

static bool g_texturePlaying = false;
static UINT16 g_textureWaveform = 0;
static BYTE g_textureIntensity = 0;
static ULONGLONG g_textureSentAtMs = 0;
static ULONGLONG g_lastMoveTickMs = 0;

static ULONGLONG g_penSeenAtMs = 0;
static ULONGLONG g_wakeUntilMs = 0;
static ULONGLONG g_wakeProbeAtMs = 0;
static bool g_wakeProbed = false;
static ULONGLONG g_firstWriteFailAtMs = 0;

static void ensureHapticSyncInited()
{
  if(!g_syncInited) {
    InitializeCriticalSection(&g_hapticCs);
    LARGE_INTEGER freq;
    if(QueryPerformanceFrequency(&freq) && freq.QuadPart > 0)
      g_qpcFreq = double(freq.QuadPart);
    g_syncInited = true;
  }
}

static std::array<BYTE, TRIGGER_REPORT_LEN> buildTriggerReport(BYTE ordinal, BYTE intensity)
{
  std::array<BYTE, TRIGGER_REPORT_LEN> cmd = {};
  cmd[0] = TRIGGER_REPORT_ID;
  cmd[1] = 0x01;
  cmd[2] = intensity < 1 ? 1 : intensity;
  cmd[3] = ordinal;
  return cmd;
}

static bool parseWaveformTable(const BYTE* buf, size_t len,
    std::map<UINT16, BYTE>& ordinals, std::map<UINT16, UINT16>& durations)
{
  const size_t tableStart = 5;
  if(!buf || len < tableStart + 4)
    return false;
  size_t slots = (len - tableStart) / 4;
  if(slots < 1)
    return false;

  ordinals.clear();
  durations.clear();
  ordinals[WF_NONE] = 1;
  ordinals[WF_STOP] = 2;

  for(size_t i = 0; i < slots; ++i) {
    UINT16 duration = UINT16(buf[tableStart + i * 2] | (buf[tableStart + i * 2 + 1] << 8));
    size_t wfOff = tableStart + (slots + i) * 2;
    UINT16 wfId = UINT16(buf[wfOff] | (buf[wfOff + 1] << 8));
    if(wfId < WF_CLICK)
      continue;
    ordinals[wfId] = BYTE(FIRST_WAVEFORM_ORDINAL + i);
    durations[wfId] = duration;
  }
  return ordinals.count(WF_CLICK) > 0;
}

static std::wstring findBluetoothDevice(const std::wstring& hidPath)
{
  DEVPROPTYPE propType = 0;
  ULONG size = 0;
  CM_Get_Device_Interface_PropertyW(hidPath.c_str(), &kDevPropInstanceId, &propType, NULL, &size, 0);
  if(size < sizeof(WCHAR))
    return std::wstring();

  std::vector<BYTE> buf(size);
  if(CM_Get_Device_Interface_PropertyW(hidPath.c_str(), &kDevPropInstanceId, &propType, buf.data(), &size, 0) != CR_SUCCESS)
    return std::wstring();

  const WCHAR* idPtr = reinterpret_cast<const WCHAR*>(buf.data());
  size_t maxChars = size / sizeof(WCHAR);
  std::wstring instId(idPtr, wcsnlen(idPtr, maxChars));

  DEVINST node = 0;
  if(CM_Locate_DevNodeW(&node, const_cast<DEVINSTID_W>(instId.c_str()), 0) != CR_SUCCESS)
    return std::wstring();

  WCHAR name[512];
  for(int depth = 0; depth < 10; ++depth) {
    if(CM_Get_Device_IDW(node, name, 512, 0) != CR_SUCCESS)
      return std::wstring();
    name[511] = L'\0';
    if(_wcsnicmp(name, L"BTHLE\\DEV_", 10) == 0)
      return std::wstring(name);
    DEVINST parent = 0;
    if(CM_Get_Parent(&parent, node, 0) != CR_SUCCESS)
      return std::wstring();
    node = parent;
  }
  return std::wstring();
}

static int isBluetoothConnected(const std::wstring& btId)
{
  if(btId.empty())
    return -1;
  DEVINST node = 0;
  if(CM_Locate_DevNodeW(&node, const_cast<DEVINSTID_W>(btId.c_str()), 0) != CR_SUCCESS)
    return -1;
  DEVPROPTYPE propType = 0;
  BYTE val[4] = {};
  ULONG size = sizeof(val);
  if(CM_Get_DevNode_PropertyW(node, &kDevPropIsConnected, &propType, val, &size, 0) != CR_SUCCESS || size == 0)
    return -1;
  return val[0] != 0 ? 1 : 0;
}

struct HidWriterState {
  HANDLE handle = INVALID_HANDLE_VALUE;
  std::wstring devicePath;
  std::wstring btDeviceId;
};

static bool tryWriteHid(HidWriterState& st, const BYTE* cmd, DWORD len)
{
  for(int attempt = 0; attempt < 2; ++attempt) {
    if(st.handle == INVALID_HANDLE_VALUE) {
      if(st.devicePath.empty())
        return false;
      st.handle = CreateFileW(st.devicePath.c_str(),
          GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
          NULL, OPEN_EXISTING, 0, NULL);
      if(st.handle == INVALID_HANDLE_VALUE)
        return false;
    }

    DWORD written = 0;
    if(WriteFile(st.handle, cmd, len, &written, NULL) && written == len)
      return true;

    CloseHandle(st.handle);
    st.handle = INVALID_HANDLE_VALUE;
  }
  return false;
}

static bool connectHid(HidWriterState& st)
{
  if(st.handle != INVALID_HANDLE_VALUE) {
    CloseHandle(st.handle);
    st.handle = INVALID_HANDLE_VALUE;
  }

  GUID hidGuid;
  HidD_GetHidGuid(&hidGuid);
  HDEVINFO devInfo = SetupDiGetClassDevsW(&hidGuid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if(devInfo == INVALID_HANDLE_VALUE)
    return false;

  bool found = false;
  SP_DEVICE_INTERFACE_DATA ifData = {};
  ifData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
  for(DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, NULL, &hidGuid, i, &ifData); ++i) {
    DWORD needed = 0;
    SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, NULL, 0, &needed, NULL);
    if(needed < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
      continue;

    std::vector<BYTE> detailBuf(needed);
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail =
        reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuf.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if(!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, needed, NULL, NULL))
      continue;

    std::wstring path(detail->DevicePath);
    HANDLE h = CreateFileW(path.c_str(),
        GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);
    if(h == INVALID_HANDLE_VALUE)
      continue;

    HIDD_ATTRIBUTES attrs = {};
    attrs.Size = sizeof(HIDD_ATTRIBUTES);
    if(!HidD_GetAttributes(h, &attrs) ||
        attrs.VendorID != SLIM_PEN_VID || attrs.ProductID != SLIM_PEN_PID) {
      CloseHandle(h);
      continue;
    }

    PHIDP_PREPARSED_DATA ppd = NULL;
    if(!HidD_GetPreparsedData(h, &ppd)) {
      CloseHandle(h);
      continue;
    }
    HIDP_CAPS caps = {};
    NTSTATUS capsOk = HidP_GetCaps(ppd, &caps);
    HidD_FreePreparsedData(ppd);

    if(capsOk != HIDP_STATUS_SUCCESS ||
        caps.UsagePage != HID_USAGE_PAGE_DIGITIZER ||
        caps.OutputReportByteLength != TRIGGER_REPORT_LEN ||
        caps.FeatureReportByteLength < 9) {
      CloseHandle(h);
      continue;
    }

    std::wstring btId = findBluetoothDevice(path);
    if(isBluetoothConnected(btId) == 0) {
      CloseHandle(h);
      continue;
    }

    std::vector<BYTE> feat(caps.FeatureReportByteLength, 0);
    feat[0] = WAVEFORM_LIST_REPORT_ID;
    std::map<UINT16, BYTE> ordinals;
    std::map<UINT16, UINT16> durations;
    if(!HidD_GetFeature(h, feat.data(), ULONG(feat.size())) ||
        !parseWaveformTable(feat.data(), feat.size(), ordinals, durations)) {
      CloseHandle(h);
      continue;
    }

    st.handle = h;
    st.devicePath = path;
    st.btDeviceId = btId;

    EnterCriticalSection(&g_hapticCs);
    g_ordinals.swap(ordinals);
    g_durations.swap(durations);
    g_hasDevice = true;
    g_lastWriteOk = true;
    g_firstWriteFailAtMs = 0;
    LeaveCriticalSection(&g_hapticCs);

    found = true;
    break;
  }

  SetupDiDestroyDeviceInfoList(devInfo);
  return found;
}

static DWORD WINAPI hapticWriterThread(LPVOID)
{
  HidWriterState st;
  for(;;) {
    bool shuttingDown = false;
    bool needConnect = false;
    bool hasCmd = false;
    std::array<BYTE, TRIGGER_REPORT_LEN> cmd = {};
    DWORD waitMs = INFINITE;

    EnterCriticalSection(&g_hapticCs);
    shuttingDown = g_shutdownRequested;
    if(!shuttingDown) {
      if(g_texturePlaying && (GetTickCount64() - g_lastMoveTickMs > 120)) {
        g_texturePlaying = false;
        g_textureWaveform = 0;
        g_hasPendingTexture = false;
        g_oneShots.push_back(buildTriggerReport(2 /* WF_STOP */, 100));
      }
      if(g_connectRequested) {
        needConnect = true;
        g_connectRequested = false;
      }
      else if(!g_oneShots.empty()) {
        cmd = g_oneShots.front();
        g_oneShots.pop_front();
        hasCmd = true;
      }
      else if(g_hasPendingTexture) {
        cmd = g_pendingTexture;
        g_hasPendingTexture = false;
        hasCmd = true;
      }
      waitMs = g_texturePlaying ? 80 : INFINITE;
    }
    LeaveCriticalSection(&g_hapticCs);

    if(shuttingDown)
      break;
    if(needConnect) {
      connectHid(st);
      continue;
    }
    if(hasCmd) {
      bool ok = tryWriteHid(st, cmd.data(), DWORD(cmd.size()));
      EnterCriticalSection(&g_hapticCs);
      g_lastWriteOk = ok;
      if(ok)
        g_firstWriteFailAtMs = 0;
      else if(g_firstWriteFailAtMs == 0)
        g_firstWriteFailAtMs = GetTickCount64();
      LeaveCriticalSection(&g_hapticCs);
      continue;
    }
    WaitForSingleObject(g_hapticWakeEvent, waitMs);
  }

  if(st.handle != INVALID_HANDLE_VALUE) {
    CloseHandle(st.handle);
    st.handle = INVALID_HANDLE_VALUE;
  }
  return 0;
}

static void initPenHaptics()
{
  ensureHapticSyncInited();
  if(g_writerThread)
    return;
  if(!g_hapticWakeEvent)
    g_hapticWakeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
  EnterCriticalSection(&g_hapticCs);
  g_shutdownRequested = false;
  g_connectRequested = true;
  LeaveCriticalSection(&g_hapticCs);
  g_writerThread = CreateThread(NULL, 0, hapticWriterThread, NULL, 0, NULL);
}

static void shutdownPenHaptics()
{
  if(!g_syncInited)
    return;
  if(g_writerThread) {
    EnterCriticalSection(&g_hapticCs);
    g_shutdownRequested = true;
    g_oneShots.clear();
    g_hasPendingTexture = false;
    g_texturePlaying = false;
    LeaveCriticalSection(&g_hapticCs);
    if(g_hapticWakeEvent)
      SetEvent(g_hapticWakeEvent);
    WaitForSingleObject(g_writerThread, 1500);
    CloseHandle(g_writerThread);
    g_writerThread = NULL;
  }
  if(g_hapticWakeEvent) {
    CloseHandle(g_hapticWakeEvent);
    g_hapticWakeEvent = NULL;
  }
}

static void stopHapticTexture()
{
  ensureHapticSyncInited();
  if(!g_writerThread)
    return;
  bool wake = false;
  EnterCriticalSection(&g_hapticCs);
  if(g_texturePlaying || g_hasPendingTexture) {
    g_texturePlaying = false;
    g_textureWaveform = 0;
    g_hasPendingTexture = false;
    g_oneShots.push_back(buildTriggerReport(2 /* WF_STOP */, 100));
    wake = true;
  }
  LeaveCriticalSection(&g_hapticCs);
  if(wake && g_hapticWakeEvent)
    SetEvent(g_hapticWakeEvent);
}

static BYTE lookupBestOrdinalLocked(UINT16 primary, UINT16 secondary, UINT16 tertiary, UINT16* chosenWf)
{
  if(primary) {
    auto it = g_ordinals.find(primary);
    if(it != g_ordinals.end()) {
      if(chosenWf) *chosenWf = primary;
      return it->second;
    }
  }
  if(secondary) {
    auto it = g_ordinals.find(secondary);
    if(it != g_ordinals.end()) {
      if(chosenWf) *chosenWf = secondary;
      return it->second;
    }
  }
  if(tertiary) {
    auto it = g_ordinals.find(tertiary);
    if(it != g_ordinals.end()) {
      if(chosenWf) *chosenWf = tertiary;
      return it->second;
    }
  }
  return 0;
}

static void resolveToolWaveforms(UINT16& primary, UINT16& secondary, UINT16& tertiary)
{
  primary = 0;
  secondary = 0;
  tertiary = 0;
  if(g_penEraserActive) {
    primary = WF_ERASER;
    secondary = WF_INK;
    return;
  }
  switch(g_hapticTool) {
  case PEN_HAPTIC_NONE:   break;
  case PEN_HAPTIC_INK:    primary = WF_INK; break;
  case PEN_HAPTIC_PENCIL: primary = WF_PENCIL; secondary = WF_INK; break;
  case PEN_HAPTIC_MARKER: primary = WF_MARKER; secondary = WF_INK; break;
  case PEN_HAPTIC_BRUSH:  primary = WF_BRUSH; secondary = WF_INK; break;
  case PEN_HAPTIC_CHISEL: primary = WF_CHISEL; secondary = WF_MARKER; tertiary = WF_INK; break;
  case PEN_HAPTIC_ERASER: primary = WF_ERASER; secondary = WF_INK; break;
  }
}

static void onPenHapticSeen()
{
  ensureHapticSyncInited();
  if(!g_hapticsEnabled || !g_writerThread)
    return;

  ULONGLONG now = GetTickCount64();
  bool woke = (g_penSeenAtMs == 0) || (now - g_penSeenAtMs > 2000);
  g_penSeenAtMs = now;
  if(woke) {
    g_wakeUntilMs = now + 8000;
    g_wakeProbed = false;
    g_wakeProbeAtMs = now >= 400 ? now - 400 : 0;
  }

  if(now >= g_wakeUntilMs || (now - g_wakeProbeAtMs < 400))
    return;

  bool wake = false;
  EnterCriticalSection(&g_hapticCs);
  if(g_wakeProbed && g_hasDevice && g_lastWriteOk) {
    g_wakeUntilMs = 0;
    LeaveCriticalSection(&g_hapticCs);
    return;
  }
  g_wakeProbeAtMs = now;
  g_wakeProbed = true;
  if(!g_hasDevice || (g_firstWriteFailAtMs && (now - g_firstWriteFailAtMs > 3000))) {
    g_connectRequested = true;
    g_firstWriteFailAtMs = 0;
    wake = true;
  }
  else if(!g_lastWriteOk || g_oneShots.empty()) {
    g_oneShots.push_back(buildTriggerReport(2 /* WF_STOP */, 100));
    wake = true;
  }
  LeaveCriticalSection(&g_hapticCs);
  if(wake && g_hapticWakeEvent)
    SetEvent(g_hapticWakeEvent);
}

static void onPenHapticInput(PenEventType eventtype, Dim x, Dim y, double pressure, bool inContact, bool isEraser)
{
  onPenHapticSeen();
  g_penEraserActive = isEraser;

  if(eventtype == TabletPress) {
    stopHapticTexture();
    g_penTipDown = true;
    g_hasLastPos = false;
    g_travel = 0.0;
    g_speed = 0.0;
    return;
  }
  if(eventtype == TabletRelease) {
    g_penTipDown = false;
    g_hasLastPos = false;
    stopHapticTexture();
    return;
  }

  if(!inContact || !g_penTipDown) {
    if(g_penTipDown) {
      g_penTipDown = false;
      g_hasLastPos = false;
    }
    stopHapticTexture();
    return;
  }
  if(!g_hapticsEnabled || g_hapticIntensity <= 0.0 || !g_writerThread) {
    stopHapticTexture();
    return;
  }

  UINT16 primary = 0, secondary = 0, tertiary = 0;
  resolveToolWaveforms(primary, secondary, tertiary);
  if(!primary) {
    stopHapticTexture();
    return;
  }

  double scale = g_dpiScale > 0.0 ? g_dpiScale : 1.0;
  Dim lx = x / scale;
  Dim ly = y / scale;
  LARGE_INTEGER qpcNow;
  QueryPerformanceCounter(&qpcNow);

  if(!g_hasLastPos) {
    g_hasLastPos = true;
    g_lastLogicalX = lx;
    g_lastLogicalY = ly;
    g_lastPosQpc = qpcNow.QuadPart;
    return;
  }

  double dt = g_qpcFreq > 0.0 ? double(qpcNow.QuadPart - g_lastPosQpc) / g_qpcFreq : 0.016;
  if(dt < 0.004)
    return;

  double dx = double(lx - g_lastLogicalX);
  double dy = double(ly - g_lastLogicalY);
  double dist = std::sqrt(dx * dx + dy * dy);
  g_lastLogicalX = lx;
  g_lastLogicalY = ly;
  g_lastPosQpc = qpcNow.QuadPart;
  g_travel += dist;
  g_speed += 0.3 * (dist / dt - g_speed);

  if(g_travel < 6.0 || g_speed < 20.0) {
    stopHapticTexture();
    return;
  }

  bool alreadyPlaying = false;
  EnterCriticalSection(&g_hapticCs);
  alreadyPlaying = g_texturePlaying;
  LeaveCriticalSection(&g_hapticCs);
  if(!alreadyPlaying && g_speed < 40.0)
    return;

  double sp = g_speed / 800.0;
  if(sp < 0.0) sp = 0.0;
  if(sp > 1.0) sp = 1.0;
  double pr = pressure;
  if(pr < 0.0) pr = 0.0;
  if(pr > 1.0) pr = 1.0;

  double speedPart = std::sqrt(sp);
  double pressurePart = 0.5 + 0.5 * pr;
  double level = 0.5 + 0.5 * speedPart * pressurePart;
  int rawTarget = int(std::lround(255.0 * g_hapticIntensity * level));
  if(rawTarget < 1) rawTarget = 1;
  if(rawTarget > 255) rawTarget = 255;
  BYTE target = BYTE(rawTarget);

  ULONGLONG nowMs = GetTickCount64();
  bool wake = false;
  EnterCriticalSection(&g_hapticCs);
  UINT16 chosenWf = 0;
  BYTE ordinal = lookupBestOrdinalLocked(primary, secondary, tertiary, &chosenWf);
  if(ordinal != 0) {
    g_lastMoveTickMs = nowMs;
    UINT16 dur = 0;
    auto dit = g_durations.find(chosenWf);
    if(dit != g_durations.end())
      dur = dit->second;
    ULONGLONG keepAlive = dur > 0 ? ULONGLONG(dur * 3 / 4) : 1000;
    ULONGLONG elapsed = nowMs - g_textureSentAtMs;
    bool changed = (std::abs(int(target) - int(g_textureIntensity)) >= 20) && (elapsed >= 100);
    bool retry = !g_lastWriteOk && (elapsed >= 150);

    if(!g_texturePlaying || chosenWf != g_textureWaveform || changed || retry || elapsed >= keepAlive) {
      g_pendingTexture = buildTriggerReport(ordinal, target);
      g_hasPendingTexture = true;
      g_texturePlaying = true;
      g_textureWaveform = chosenWf;
      g_textureIntensity = target;
      g_textureSentAtMs = nowMs;
      wake = true;
    }
  }
  LeaveCriticalSection(&g_hapticCs);
  if(wake && g_hapticWakeEvent)
    SetEvent(g_hapticWakeEvent);
}

void winSetPenHapticsConfig(bool enabled, double intensity01)
{
  if(intensity01 < 0.0) intensity01 = 0.0;
  if(intensity01 > 1.0) intensity01 = 1.0;
  if(g_hapticsEnabled == enabled && g_hapticIntensity == intensity01)
    return;
  g_hapticsEnabled = enabled;
  g_hapticIntensity = intensity01;
  if(!enabled || intensity01 <= 0.0)
    stopHapticTexture();
}

void winSetPenHapticsTool(WinPenHapticTool tool)
{
  if(g_hapticTool == tool)
    return;
  g_hapticTool = tool;
  if(tool == PEN_HAPTIC_NONE)
    stopHapticTexture();
}

void winTriggerPenHapticClick()
{
  ensureHapticSyncInited();
  if(!g_hapticsEnabled || g_hapticIntensity <= 0.0 || !g_writerThread)
    return;

  int rawTarget = int(std::lround(255.0 * g_hapticIntensity));
  if(rawTarget < 1) rawTarget = 1;
  if(rawTarget > 255) rawTarget = 255;
  BYTE target = BYTE(rawTarget);

  bool wake = false;
  EnterCriticalSection(&g_hapticCs);
  auto it = g_ordinals.find(WF_CLICK);
  if(it != g_ordinals.end()) {
    g_hasPendingTexture = false;
    g_texturePlaying = false;
    g_textureWaveform = 0;
    g_oneShots.push_back(buildTriggerReport(it->second, target));
    wake = true;
  }
  LeaveCriticalSection(&g_hapticCs);
  if(wake && g_hapticWakeEvent)
    SetEvent(g_hapticWakeEvent);
}

static void processPenInfo(const POINTER_PEN_INFO& ppi, PenEventType eventtype)
{
  static UINT32 prevButtons = 0;

  if(winTabProximity) return;
  const POINT pix = ppi.pointerInfo.ptPixelLocation;
  const POINT him = ppi.pointerInfo.ptHimetricLocation;
  Dim x = pix.x;
  Dim y = pix.y;
  RECT pRect, dRect;  // pointer (Himetric) rect, display rect
  if(GetPointerDeviceRects(ppi.pointerInfo.sourceDevice, &pRect, &dRect)) {
    x = dRect.left + (dRect.right - dRect.left) * Dim(him.x - pRect.left)/(pRect.right - pRect.left);
    y = dRect.top + (dRect.bottom - dRect.top) * Dim(him.y - pRect.top)/(pRect.bottom - pRect.top);
  }
  // Confirmed that HIMETRIC is higher resolution than pixel location on Surface Pro: saw different HIMETRIC
  //  locations for the same pixel loc, including updates to HIMETRIC loc with no change in pixel loc
  //PLATFORM_LOG("Pix: %d %d; HIMETRIC: %d %d", pix.x, pix.y, him.x, him.y);
  //PLATFORM_LOG("Pointer flags: 0x%x; pen flags: 0x%x\n", ppi.pointerInfo.pointerFlags, ppi.penFlags);

  // if barrel button pressed when pen down, penFlags changes but pointerFlags doesn't (the first time)
  UINT32 buttons = ((ppi.pointerInfo.pointerFlags >> 4) & 0x1F) | (ppi.penFlags & PEN_FLAG_BARREL ? SDL_BUTTON_RMASK : 0);
  bool isEraser = ppi.penFlags & (PEN_FLAG_ERASER | PEN_FLAG_INVERTED);  // PEN_FLAG_INVERTED means eraser in proximity
  bool inContact = (ppi.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) != 0;
  onPenHapticInput(eventtype, x, y, ppi.pressure/1024.0, inContact, isEraser);
  notifyTabletEvent(eventtype, Point(x, y), ppi.pressure/1024.0, ppi.tiltX/90.0, ppi.tiltY/90.0, isEraser,
      buttons ^ prevButtons, buttons, int(ppi.pointerInfo.sourceDevice), ppi.pointerInfo.dwTime);
  prevButtons = buttons;
}

// ideally, we wouldn't process history unless mode is STROKE
static void processPenHistory(UINT32 ptrid)
{
  if(winTabProximity) return;
  UINT32 historycount = MAX_N_POINTERS;
  POINTER_PEN_INFO* ppi = &penPointerInfo[0];
  if(GetPointerPenInfoHistory(ptrid, &historycount, ppi)) {
    if(historycount > MAX_N_POINTERS) {
      // need more room ... we want to get all history at once since it's returned newest first!
      ppi = new POINTER_PEN_INFO[historycount];
      GetPointerPenInfoHistory(ptrid, &historycount, ppi);
    }
    // process items oldest to newest
    for(int ii = historycount - 1; ii >= 0; ii--)
      processPenInfo(ppi[ii], TabletMove);
    if(ppi != &penPointerInfo[0])
      delete[] ppi;
  }
}

static bool processPointerFrame(UINT32 ptrid, TouchPointState eventtype)
{
  UINT32 pointercount = MAX_N_POINTERS;
  std::vector<TouchPoint> pts;
  if(GetPointerFrameInfo(ptrid, &pointercount, &pointerInfo[0])) {
    for(unsigned int ii = 0; ii < pointercount; ii++) {
      if(pointerInfo[ii].pointerType == PT_PEN) {
        // for hovering pen
        if(GetPointerPenInfo(pointerInfo[ii].pointerId, &penPointerInfo[0]))
          processPenInfo(penPointerInfo[0], TabletMove);
        // propagate pen hover events to DefWindowProc so mouse cursor follows pen - see explanation below
        return false;
      }
      if(pointerInfo[ii].pointerType != PT_TOUCH)
        continue;
      TouchPoint pt;
      pt.id = pointerInfo[ii].pointerId;
      pt.state = pointerInfo[ii].pointerId == ptrid ? eventtype : TouchPointMoved;
      pt.screenPos = Point(pointerInfo[ii].ptPixelLocation.x, pointerInfo[ii].ptPixelLocation.y);
      pt.pressure = 1;
      pts.push_back(pt);
    }
    if(pts.empty())
      return false;
    //event.t = pointerInfo[0].performanceCount;
    notifyTouchEvent(eventtype, pts, pointerInfo[0].dwTime);
    return true;
  }
  else if(eventtype == TouchPointReleased) {
    // seems GetPointerFrameInfo/GetPointerInfo sometimes returns error for WM_POINTERUP ... didn't notice this before
    TouchPoint pt;
    pt.id = ptrid;
    pt.state = eventtype;
    pt.screenPos = Point(0, 0);
    pt.pressure = 0;
    pts.push_back(pt);
    notifyTouchEvent(eventtype, pts, pointerInfo[0].dwTime);  // reuse time from last event
    return true;
  }
  return false;
}

static void initWMPointer()
{
#ifdef WMPOINTER_NEEDED
  HINSTANCE user32 = LoadLibraryA("user32.dll");
  if(user32) {
    GetPointerInfo = (PtrGetPointerInfo)(GetProcAddress(user32, "GetPointerInfo"));
    GetPointerFrameInfo = (PtrGetPointerFrameInfo)(GetProcAddress(user32, "GetPointerFrameInfo"));
    GetPointerPenInfo = (PtrGetPointerPenInfo)(GetProcAddress(user32, "GetPointerPenInfo"));
    GetPointerPenInfoHistory = (PtrGetPointerPenInfoHistory)(GetProcAddress(user32, "GetPointerPenInfoHistory"));
    GetPointerDeviceRects = (PtrGetPointerDeviceRects)(GetProcAddress(user32, "GetPointerDeviceRects"));
    InjectTouchInput = (PtrInjectTouchInput)(GetProcAddress(user32, "InjectTouchInput"));
    InitializeTouchInjection = (PtrInitializeTouchInjection)(GetProcAddress(user32, "InitializeTouchInjection"));
  }
#endif
  // Attempt to get HIMETRIC to pixel conversion factor; on Surface Pro, result is close, but not quite
  // 1 HIMETRIC = 0.01 mm
  //QWidget* screen = QApplication::desktop()->screen(0);
  // this is equiv to GetDeviceCaps(HORZRES)/GetDeviceCaps(HORZSIZE)
  //HimetricToPix = (96.0 / 2540);  //screen->width()/qreal(100*screen->widthMM());
}

static bool winInputEvent(MSG* m) //, long* result)
{
  // we're assuming pointerIds are never 0, which seems to be the case, but probably isn't a good idea
  static UINT32 penPointerId = 0;

#ifdef WMPOINTER_NEEDED
  if(!GetPointerInfo)
    return false;
#endif
  switch(m->message) {
  // WM_POINTER:
  // WM_POINTERDOWN with type PT_PEN: ignore all other pointers, use GetPointerPenInfoHistory
  // otherwise, use GetPointerFrameInfo (discard history)
  case WM_POINTERENTER:
  {
    UINT32 ptrId = GET_POINTERID_WPARAM(m->wParam);
    if(GetPointerInfo(ptrId, &pointerInfo[0]) && pointerInfo[0].pointerType == PT_PEN) {
      if(GetPointerPenInfo(ptrId, &penPointerInfo[0]))
        g_penEraserActive = (penPointerInfo[0].penFlags & (PEN_FLAG_ERASER | PEN_FLAG_INVERTED)) != 0;
      onPenHapticSeen();
    }
    break;  // let DefWindowProc / SDL handle hover cursor
  }
  case WM_POINTERLEAVE:
  case WM_POINTERCAPTURECHANGED:
    stopHapticTexture();
    break;
  case WM_ACTIVATE:
    if(LOWORD(m->wParam) == WA_INACTIVE)
      stopHapticTexture();
    break;
  case WM_DESTROY:
    shutdownPenHaptics();
    break;
  case WM_POINTERDOWN:
    updateClientRect(m->hwnd);
    if(GetPointerInfo(GET_POINTERID_WPARAM(m->wParam), &pointerInfo[0])) {
      if(pointerInfo[0].pointerType == PT_PEN) {
        penPointerId = pointerInfo[0].pointerId;
        if(GetPointerPenInfo(penPointerId, &penPointerInfo[0]))
          processPenInfo(penPointerInfo[0], TabletPress);
        return true;
      }
      else
        return processPointerFrame(GET_POINTERID_WPARAM(m->wParam), TouchPointPressed);
    }
    break;
  case WM_POINTERUPDATE:
    updateClientRect(m->hwnd);
    if(penPointerId && penPointerId == GET_POINTERID_WPARAM(m->wParam)) {
      processPenHistory(penPointerId);
      return true;
    }
    else
      return processPointerFrame(GET_POINTERID_WPARAM(m->wParam), TouchPointMoved);
    break;
  case WM_POINTERUP:
    updateClientRect(m->hwnd);
    if(penPointerId && penPointerId == GET_POINTERID_WPARAM(m->wParam)) {
      if(GetPointerPenInfo(penPointerId, &penPointerInfo[0]))
        processPenInfo(penPointerInfo[0], TabletRelease);
      penPointerId = 0;
      return true;
    }
    else
      return processPointerFrame(GET_POINTERID_WPARAM(m->wParam), TouchPointReleased);

  case WM_MOUSEWHEEL:
  case WM_MOUSEHWHEEL:
  {
    // SDL doesn't update its internal mod state if window doesn't have focus, so get mods from Windows
    uint32_t mods = GetKeyState(VK_CONTROL) < 0 ? KMOD_CTRL : 0;  // high order bit set if key is down
    mods |= GetKeyState(VK_SHIFT) < 0 ? KMOD_SHIFT : 0;  // high order bit set if key is down
    SDL_Event event = { 0 };  // we'll leave windowID and which == 0
    event.type = SDL_MOUSEWHEEL;
    //event.wheel.timestamp = SDL_GetTicks();
    event.wheel.x = m->message == WM_MOUSEWHEEL ? 0 : GET_WHEEL_DELTA_WPARAM(m->wParam);
    event.wheel.y = m->message == WM_MOUSEWHEEL ? GET_WHEEL_DELTA_WPARAM(m->wParam) : 0;
    event.wheel.direction = SDL_MOUSEWHEEL_NORMAL | (mods << 16);
    SDL_PushEvent(&event);  //SDL_PeepEvents(&event, 1, SDL_ADDEVENT, 0, 0);
    break;
  }
  case WM_CLIPBOARDUPDATE:
  {
    SDL_Event event = { 0 };
    event.type = SDL_CLIPBOARDUPDATE;
    SDL_PushEvent(&event);
    break;
  }
  // Windows shutdown message; followed by WM_ENDSESSION
  case WM_QUERYENDSESSION:
  {
    SDL_Event event = { 0 };
    event.type = SDL_APP_WILLENTERBACKGROUND;
    SDL_PushEvent(&event);  // currently handled by event filter, so use PushEvent instead of PeepEvents
    break;
  }
  default:
    break;
  }
  return false;
}

static WNDPROC prevWndProc = NULL;

// We wrap SDL's window proc to prevent WM_POINTER from being forwared to DefWindowProc, which causes touch
//  feedback to be shown (e.g. on pen tap). However, in order for mouse cursor to track pen (or touch)
//  position, WM_POINTER messages must reach DefWindowProc (or SetCursorPos must be called manually), so we
//  do propagate pen hover events to DefWindowProc. The resulting mouse events are flagged as synthesized
//  from touch (unlike if we called SetCursorPos manually), so they'll be ignored in SvgGui::sdlEvent.
// Besides seeming like the right UX, it is necessary for mouse cursor to track pen because setting cursor
//  fails (at least via SDL) if the mouse cursor is outside the window (even if the pen cursor is inside).
// Win 8+ provides SetWindowFeedbackSetting that might also accomplish this, but swallowing WM_POINTER (except
//  pen hover) was how it worked in Qt version of Write ... by dumb luck!
// Note that neither SDL_SYSWMEVENT nor SDL_WindowsMessageHook provide a way to swallow events
// If we need this for multiple windows, look into setting for window class instead of per window - see
//  SDL_RegisterApp
LRESULT CALLBACK wrapSDLWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  MSG msg = { hwnd, uMsg, wParam, lParam, 0, {0, 0} };
  // window proc return value depends on message, but typically 0 indicates application has processed message
#ifdef USE_WINTAB
  if(hTab && winTabEvent(&msg))
    return 0;
#endif
  if(winInputEvent(&msg))
    return 0;  //DefWindowProc(hwnd, uMsg, wParam, lParam);

  return CallWindowProc(prevWndProc, hwnd, uMsg, wParam, lParam);
}

#include "SDL_syswm.h"

void initTouchInput(SDL_Window* sdlwin, bool useWintab)
{
  SDL_SysWMinfo wmInfo;
  SDL_VERSION(&wmInfo.version);
  if(!SDL_GetWindowWMInfo(sdlwin, &wmInfo))
    return;
  HWND hwnd = wmInfo.info.win.window;
  //sdlWindow = sdlwin;

  // setup Wintab and WM_POINTER support
  initWMPointer();
  initPenHaptics();
#ifdef USE_WINTAB
  hTab = NULL;
  if(useWintab)
    initWinTab(hwnd);
#endif

  //SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
  // wrap SDL's window proc to handle Wintab and WM_POINTER events
  prevWndProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)&wrapSDLWndProc);

#ifdef WMPOINTER_NEEDED
  if(!GetPointerInfo)
    return;  // don't disable SDL's touch handling if WM_POINTER not available
#endif

  // SDL divides mouse wheel step by WHEEL_DELTA (== 120) and sends as int - this is fine for an actual
  //  mouse wheel but throws away resolution for touchpad scroll, so we handle it ourselves
  SDL_EventState(SDL_MOUSEWHEEL, SDL_DISABLE);
  // disable SDL's touch handling since it will send finger events for pen input too
  // - technically not necessary if there are no WM_TOUCH events
  SDL_EventState(SDL_FINGERDOWN, SDL_DISABLE);
  SDL_EventState(SDL_FINGERMOTION, SDL_DISABLE);
  SDL_EventState(SDL_FINGERUP, SDL_DISABLE);
  // tell Windows not to send WM_TOUCH (SDL calls RegisterTouchWindow())
  // - doesn't seem to be necessary if we accept WM_POINTER, but can't hurt I guess
  UnregisterTouchWindow(hwnd);  // Win 7+

  // SDL_SetClipboardText is broken - it reads sequence number before calling CloseClipboard()
  AddClipboardFormatListener(hwnd);  // Win Vista+
  SDL_EventState(SDL_CLIPBOARDUPDATE, SDL_DISABLE);
}

#include <ShellScalingAPI.h>
typedef HRESULT(WINAPI *PtrSetProcessDpiAwareness)(PROCESS_DPI_AWARENESS);

void setDPIAware()
{
  HINSTANCE shcore = LoadLibraryA("shcore.dll");
  if(shcore) {
    PtrSetProcessDpiAwareness fn = (PtrSetProcessDpiAwareness)(GetProcAddress(shcore, "SetProcessDpiAwareness"));
    if(fn) {
      fn(PROCESS_PER_MONITOR_DPI_AWARE);
      return;
    }
  }
  // This is supposed to be set in application manifest; needs #include <ShellScalingAPI.h> and Shcore.lib
  //SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);  -- use LoadLibraryA and GetProcAddress here!
  SetProcessDPIAware();
}

// ref: https://github.com/FMJ-Software/Image-Eye/blob/master/Src/Clipboard.cpp
Image getClipboardImage()
{
  Image img(0, 0);
  //UINT fmt = 0; OpenClipboard(NULL); do { fmt = EnumClipboardFormats(fmt); } while(fmt); CloseClipboard();
  if(!IsClipboardFormatAvailable(CF_DIB) || !OpenClipboard(NULL))
    return img;

  HANDLE hClipboard = GetClipboardData(CF_DIB);
  //if(!hClipboard)
  //  hClipboard = GetClipboardData(CF_DIBV5);
  if(hClipboard && hClipboard != INVALID_HANDLE_VALUE) {
    void* dib = GlobalLock(hClipboard);
    if(dib) {
      BITMAPINFOHEADER* bminfo = (BITMAPINFOHEADER*)dib;
      //BITMAPFILEHEADER hdr;
      constexpr int bfHdrSize = 14;
      int iColors = 0;
      if(bminfo->biBitCount <= 8)
        iColors = bminfo->biClrUsed == 0 ? (1 << bminfo->biBitCount) : bminfo->biClrUsed;
      DWORD dwSize = bminfo->biSizeImage;
      if(dwSize == 0)
        dwSize = bminfo->biHeight * ((((bminfo->biWidth*bminfo->biBitCount + 7) / 8) + 3) & (~3));
      DWORD bfOffset = bfHdrSize + bminfo->biSize + iColors * sizeof(RGBQUAD);
      if(bminfo->biCompression == BI_BITFIELDS) bfOffset += 3 * sizeof(DWORD);
      DWORD bfSize = bfOffset + dwSize;

      unsigned char* buff = (unsigned char*)malloc(bfSize);
      *(WORD*)buff = 0x4D42;  // "BM"
      *(DWORD*)(buff + 2) = bfSize;
      *(DWORD*)(buff + 6) = 0;
      *(DWORD*)(buff + 10) = bfOffset;
      memcpy(buff + bfHdrSize, bminfo, bfSize - bfHdrSize);

      // we have to assume PNG for clipboard; since Image::encodePNG() checks for PNG header, not a problem
      //  that encoded format is Windows bitmap instead of PNG
      img = Image::decodeBuffer(buff, bfSize, Image::PNG);
      free(buff);
      GlobalUnlock(dib);
    }
  }
  CloseClipboard();
  return img;
}

std::string winOpenPdfDialog(SDL_Window* sdlwin, const char* initialDir)
{
  HWND hwnd = NULL;
  if(sdlwin) {
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if(SDL_GetWindowWMInfo(sdlwin, &wmInfo))
      hwnd = wmInfo.info.win.window;
  }

  wchar_t fileBuf[4096] = {};
  std::wstring initDirW;
  if(initialDir && initialDir[0]) {
    initDirW = utf8_to_wstr(initialDir);
    for(wchar_t& ch : initDirW) {
      if(ch == L'/') ch = L'\\';
    }
  }

  OPENFILENAMEW ofn = {};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = hwnd;
  ofn.lpstrFilter = L"PDF Files (*.pdf)\0*.pdf\0All Files (*.*)\0*.*\0";
  ofn.lpstrFile = fileBuf;
  ofn.nMaxFile = sizeof(fileBuf) / sizeof(fileBuf[0]);
  ofn.lpstrInitialDir = initDirW.empty() ? NULL : initDirW.c_str();
  ofn.lpstrTitle = L"Import PDF";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

  if(GetOpenFileNameW(&ofn))
    return wstr_to_utf8(fileBuf);
  return "";
}

int winRunCommand(const std::string& cmdLine, std::string* output)
{
  if(output)
    output->clear();

  int wlen = MultiByteToWideChar(CP_UTF8, 0, cmdLine.c_str(), -1, NULL, 0);
  if(wlen <= 0) {
    if(output) *output = "Invalid UTF-8 command line";
    return -1;
  }
  std::vector<wchar_t> cmdBuf(wlen);
  MultiByteToWideChar(CP_UTF8, 0, cmdLine.c_str(), -1, cmdBuf.data(), wlen);

  SECURITY_ATTRIBUTES sa = {};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE hRead = NULL, hWrite = NULL;
  if(!CreatePipe(&hRead, &hWrite, &sa, 0)) {
    if(output) *output = "CreatePipe failed";
    return -1;
  }
  SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = hWrite;
  si.hStdError = hWrite;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION pi = {};
  BOOL ok = CreateProcessW(NULL, cmdBuf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
  CloseHandle(hWrite);

  if(!ok) {
    DWORD err = GetLastError();
    CloseHandle(hRead);
    if(output) *output = fstring("Failed to launch command (error %lu)", (unsigned long)err);
    return -1;
  }

  char buf[4096];
  DWORD bytesRead = 0;
  while(ReadFile(hRead, buf, sizeof(buf), &bytesRead, NULL) && bytesRead > 0) {
    if(output)
      output->append(buf, bytesRead);
  }
  CloseHandle(hRead);

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exitCode = 1;
  GetExitCodeProcess(pi.hProcess, &exitCode);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return int(exitCode);
}

// On Win 8.1 VM, widthMM is just 96 * widthPx
/*float winGetRealDpi(SDL_Window* sdlWindow)
{
  SDL_SysWMinfo wmInfo;
  SDL_VERSION(&wmInfo.version);
  if(!SDL_GetWindowWMInfo(sdlWindow, &wmInfo))
    return 96;
  HDC dc = wmInfo.info.win.hdc;
  int widthMM = GetDeviceCaps(dc, HORZSIZE);
  int widthPx = GetDeviceCaps(dc, HORZRES);
  return (25.4*widthPx) / widthMM;
}*/


/*
// Qt created a dummy window to receive Wintab events - not sure what the advantage of this is; doesn't seem
//  necessary
static HWND createDummyWindow(WNDPROC wndProc)
{
  WNDCLASSEX wc;
  HWND hwnd;
  MSG Msg;
  HINSTANCE hInstance = static_cast<HINSTANCE>(GetModuleHandle(0));
  const wchar_t* winClassName = L"TabletDummyWindow";
  const wchar_t* windowName = L"TabletDummyWindow";

  //Step 1: Registering the Window Class
  wc.cbSize = sizeof(WNDCLASSEX);
  wc.style = 0;
  wc.lpfnWndProc = wndProc;  // wndProc ? wndProc : DefWindowProc
  wc.cbClsExtra = 0;
  wc.cbWndExtra = 0;
  wc.hInstance = hInstance;
  wc.hIcon = 0; // LoadIcon(NULL, IDI_APPLICATION);
  wc.hCursor = 0; // LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW+1);
  wc.lpszMenuName = NULL;
  wc.lpszClassName = winClassName;
  wc.hIconSm = LoadIcon(NULL, IDI_APPLICATION);

  if(!RegisterClassEx(&wc))
    return 0;

  return CreateWindowEx(0, winClassName, windowName, WS_OVERLAPPED, CW_USEDEFAULT, CW_USEDEFAULT,
      CW_USEDEFAULT, CW_USEDEFAULT, HWND_MESSAGE, NULL, hInstance, NULL);
}

LRESULT CALLBACK winTabWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  MSG msg = { hwnd, uMsg, wParam, lParam, 0, {0, 0} };
  if(ghWintab && winTabEvent(&msg))
    return 0;
  return DefWindowProc(hwnd, uMsg, wParam, lParam);
}
*/
// functions for direct injection of tablet and touch events (only used on Windows at the moment)
//#include "test/scribbletest.h"

// see http://code.msdn.microsoft.com/windowsdesktop/Touch-Injection-Sample-444d9bf7/
/* #ifdef SCRIBBLE_TEST
bool ScribbleInput::injectTouch(Dim x, Dim y, Dim p, int event)
{
  static bool touchInjectionInited = false;
  static POINTER_TOUCH_INFO contact;
  if(!GetPointerInfo)
    return false;

  if(!touchInjectionInited) {
    InitializeTouchInjection(10, TOUCH_FEEDBACK_NONE);
    memset(&contact, 0, sizeof(POINTER_TOUCH_INFO));
    contact.pointerInfo.pointerType = PT_TOUCH; // we're sending touch input
    contact.pointerInfo.pointerId = 0;          // contact 0
    contact.touchFlags = TOUCH_FLAG_NONE;
    contact.touchMask = TOUCH_MASK_CONTACTAREA | TOUCH_MASK_ORIENTATION | TOUCH_MASK_PRESSURE;
    contact.orientation = 90;
    // set the contact area depending on thickness
    //contact.rcContact.top = 480 - 2;
    //contact.rcContact.bottom = 480 + 2;
    //contact.rcContact.left = 640 - 2;
    //contact.rcContact.right = 640 + 2;
    touchInjectionInited = true;
  }

  contact.pointerInfo.ptPixelLocation.x = x;
  contact.pointerInfo.ptPixelLocation.y = y;
  //contact.pointerInfo.ptHimetricLocation.x = x/HimetricToPix;
  //contact.pointerInfo.ptHimetricLocation.y = y/HimetricToPix;
  contact.pressure = p * 1024;
  if(event == INPUTEVENT_PRESS)
    contact.pointerInfo.pointerFlags = POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
  else if(event == INPUTEVENT_RELEASE)
    contact.pointerInfo.pointerFlags = POINTER_FLAG_UP;
  else
    contact.pointerInfo.pointerFlags = POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT;
  if(InjectTouchInput(1, &contact) == 0)
    return false;
  // seems to be necessary for us to receive the touch event
  QApplication::processEvents();
  return true;
}
#endif */
