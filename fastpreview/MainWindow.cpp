#include "MainWindow.h"
#include <assert.h>
#include <strsafe.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <windowsx.h>
#include <shlwapi.h>
#include <math.h>
#include <sstream>
#include <memory>
#include "resource.h"
#include "console.h"
#include "Messages.h"
#include "Objects.h"
#include "Rect.h"
#include "procpriority.h"


/* timers */
#define IDT_RELOAD   1
#define IDT_AACCEPT  3

/* status */
#define IDC_STATUS 10

namespace {
  using stringtools::loadResourceString;

  const std::wstring s_switch = loadResourceString(IDS_SWITCH);
  const std::wstring s_switch_free = loadResourceString(IDS_SWITCH_FREE);
  const std::wstring s_switch_best = loadResourceString(IDS_SWITCH_BEST);
  const std::wstring s_opening = loadResourceString(IDS_OPENING);
  const std::wstring s_quitting = loadResourceString(IDS_QUITTING);
  const std::wstring s_deleting = loadResourceString(IDS_DELETING);
  const std::wstring s_explorermenu = loadResourceString(IDS_EXPLORERMENU);
  const std::wstring s_loading = loadResourceString(IDS_LOADING);
  const std::wstring s_resizing = loadResourceString(IDS_RESIZING);
  const std::wstring s_err_load = loadResourceString(IDS_ERR_LOAD);
  const std::wstring s_err_unsupported = loadResourceString(IDS_ERR_UNSUPPORTED);

  class MenuMethod
  {
  private:
    typedef std::map<FREE_IMAGE_FILTER, DWORD> MenuMap;
    const MenuMap menuMap_;

    static MenuMap makeMenuMap()
    {
      MenuMap menuMap;
      menuMap[FILTER_BOX] = ID_RESAMPLEMETHOD_BOX;
      menuMap[FILTER_BILINEAR] = ID_RESAMPLEMETHOD_BILINEAR;
      menuMap[FILTER_BICUBIC] = ID_RESAMPLEMETHOD_BICUBIC;
      menuMap[FILTER_BSPLINE] = ID_RESAMPLEMETHOD_BSPLINE;
      menuMap[FILTER_CATMULLROM] = ID_RESAMPLEMETHOD_CATMULL;
      menuMap[FILTER_LANCZOS3] = ID_RESAMPLEMETHOD_LANCZOS3;
      return menuMap;
    }

  public:
    MenuMethod() : menuMap_(makeMenuMap())
    {}
    void operator ()(HMENU hSub, FREE_IMAGE_FILTER m) const
    {
      MENUITEMINFO mi;
      mi.cbSize = sizeof(MENUITEMINFO);
      mi.fMask = MIIM_STATE;
      for (const auto& i : menuMap_) {
        mi.fState = i.first == m ? MFS_CHECKED : MFS_UNCHECKED;
        SetMenuItemInfo(
          hSub,
          i.second,
          FALSE,
          &mi
          );
      }
    }

    bool valid(DWORD f) const
    {
      return menuMap_.find((FREE_IMAGE_FILTER)f) != menuMap_.end();
    }

  };

  class __declspec(novtable) AutoToggle
  {
  private:
    bool &ref_;

  public:
    explicit AutoToggle(bool& ref, bool initial) : ref_(ref)
    {
      ref_ = initial;
    }
    ~AutoToggle()
    {
      ref_ = !ref_;
    }
  };
}


static LRESULT CALLBACK MainWindowProc(
  HWND hwnd_, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  MainWindow *Owner = reinterpret_cast<MainWindow*>(
    GetProp(hwnd_, _T("ownerWndProc")));

  switch (uMsg) {
  case WM_CREATE:
    SetProp(
      hwnd_,
      _T("ownerWndProc"),
      (HANDLE)reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams
      );
    break;
  }

  return Owner->WindowProc(hwnd_, uMsg, wParam, lParam);
}

MainWindow::MainWindow(HINSTANCE aInstance, const std::wstring &aFile)
  : hinst_(aInstance),
  file_(aFile),
  fileAttr_(aFile),
  clientWidth_(300), clientHeight_(300),
  aspect_(1.0f),
  newAspect_(1.0f),
  best_(true),
  wheeling_(false),
  devcontext_(nullptr),
  inTransformation_(false),
  keyCtrl_(FALSE),
  keyShift_(FALSE),
  mbtnDown_(FALSE),
  loaded_image_width(0),
  loaded_image_height(0),
  display_image_width(0),
  display_image_height(0),

  resize_method(FILTER_LANCZOS3),
  reg_(HKEY_CURRENT_USER, L"Software\\MaierSoft\\FastPreview")
{
  InitCommonControls();

  reg_.create();

  // setup window class and register
  WNDCLASS wc;
  wc.style = CS_OWNDC | CS_DBLCLKS;
  wc.lpfnWndProc = MainWindowProc;
  wc.cbClsExtra = 0;
  wc.cbWndExtra = 0;
  wc.hInstance = hinst_;
  wc.hIcon = LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDI_APP));
  //wc.hIcon = GetIcon(file);
  wc.hCursor = nullptr; //LoadCursor(nullptr, MAKEINTRESOURCE(IDC_ARROW));
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  wc.lpszMenuName = nullptr;
  wc.lpszClassName = _T("FPWND");
  RegisterClass(&wc);

  size_t pos = file_.rfind('\\') + 1;

  // Create the window!
  auto title =
    stringtools::formatResourceString(IDS_MAINTITLE, file_.substr(pos).c_str());
  hwnd_ = CreateWindowEx(
    WS_EX_APPWINDOW,
    _T("FPWND"),
    title.c_str(),
    WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_THICKFRAME,
    CW_USEDEFAULT,
    CW_USEDEFAULT,
    CW_USEDEFAULT,
    CW_USEDEFAULT,
    nullptr,
    nullptr,
    hinst_,
    (LPVOID)this
    );
  if (!hwnd_) {
    throw WindowsException();
  }

  mbtnPos_.x = mbtnPos_.y = 0;

  hctx_ = LoadMenu(
    hinst_,
    MAKEINTRESOURCE(IDR_MAINCTX)
    );
  hstatus_ = CreateWindowEx(
    WS_EX_STATICEDGE,
    STATUSCLASSNAME,
    nullptr,
    WS_CHILD | WS_BORDER,
    0, 0, 0, 0,
    hwnd_,
    nullptr, //(HMENU)IDC_STATUS,
    hinst_,
    nullptr
    );
  if (!hstatus_ || hstatus_ == INVALID_HANDLE_VALUE) {
    throw WindowsException();
  }
  
  CreateDC();
  resize_method = GetResampleMethod();
  SendMessage(hstatus_, SB_SIMPLE, TRUE, 0);
  
}

MainWindow::~MainWindow(void)
{
  KillTimer(hwnd_, IDT_RELOAD);

  DestroyWindow(hwnd_);
  UnregisterClass(_T("FPWND"), hinst_);
  DestroyMenu(hctx_);
  DeleteDC(devcontext_);
}

LRESULT MainWindow::WindowProc(
  HWND hwnd_, UINT msg, WPARAM wparam, LPARAM lparam)
{
#define ONHANDLER(message, handler) \
  case message: return handler(msg, wparam, lparam)

  switch (msg) {
  case WM_CREATE:	{
    SetTimer(
      hwnd_,
      IDT_RELOAD,
      2000,
      0
      );
    SetCursor(
      LoadCursor(
      nullptr,
      IDC_ARROW
      )
      );

    HMENU hSys = GetSystemMenu(hwnd_, FALSE);
    InsertMenu(hSys, SC_CLOSE, MF_STRING | MF_BYCOMMAND, WM_USER, _T("&About"));
    InsertMenu(hSys, SC_CLOSE, MF_SEPARATOR | MF_BYCOMMAND, 0, nullptr);

    return 0;
  }

    ONHANDLER(WM_CLOSE, OnClose);

    ONHANDLER(WM_SHOWWINDOW, OnShowWindow);

    ONHANDLER(WM_PAINT, OnPaint);

    ONHANDLER(WM_KEYUP, OnKey);
    ONHANDLER(WM_KEYDOWN, OnKey);

    ONHANDLER(WM_CHAR, OnChar);

    
    

    ONHANDLER(WM_LBUTTONDOWN, OnLButton);
    ONHANDLER(WM_LBUTTONUP, OnLButton);
    ONHANDLER(WM_NCMOUSEMOVE, OnLButton);
    ONHANDLER(WM_LBUTTONDBLCLK, OnLButton);

    ONHANDLER(WM_MOUSEMOVE, OnMouseMove);

    ONHANDLER(WM_VSCROLL, OnScroll);
    ONHANDLER(WM_HSCROLL, OnScroll);

    ONHANDLER(WM_CONTEXTMENU, OnContextMenu);

    ONHANDLER(WM_COMMAND, OnCommand);
    ONHANDLER(WM_SYSCOMMAND, OnSysCommand);

    ONHANDLER(WM_TIMER, OnTimer);

    ONHANDLER(WM_MOVING, OnMoving);

    ONHANDLER(WM_SIZE, OnSize);
    ONHANDLER(WM_SIZING, OnSize);

    ONHANDLER(WM_WATCH, OnWatch);

  default:
    return ::DefWindowProc(hwnd_, msg, wparam, lparam);
  }
#undef ONHANDLER
}

#define HANDLERIMPL(handler) \
  LRESULT __fastcall MainWindow::handler(UINT msg, WPARAM wparam, LPARAM lparam)
#define HANDLERPASS \
  return ::DefWindowProc(hwnd_, msg, wparam, lparam)

HANDLERIMPL(OnClose)
{
  PostQuitMessage(0);
  return 0;
}

HANDLERIMPL(OnShowWindow)
{
  CenterWindow();
  HANDLERPASS;
}

HANDLERIMPL(OnPaint)
{
  
  PAINTSTRUCT ps;
  BeginPaint(hwnd_, &ps);
  
  if (ps.rcPaint.right - ps.rcPaint.left && ps.rcPaint.bottom - ps.rcPaint.top) {
    BitBlt(
      ps.hdc,
      ps.rcPaint.left,
      ps.rcPaint.top,
      ps.rcPaint.right - ps.rcPaint.left,
      ps.rcPaint.bottom - ps.rcPaint.top,
      devcontext_,
	  sp_.x + ps.rcPaint.left,
	  sp_.y + ps.rcPaint.top,
      SRCCOPY
      );
  }

  EndPaint(hwnd_, &ps);

  SendMessage(hstatus_, msg, wparam, lparam);
  return 0;
}

HANDLERIMPL(OnKey)
{
  if (msg == WM_KEYUP) {
    switch (wparam) {
    case VK_SHIFT:
      keyShift_ = FALSE;
      break;

    case VK_CONTROL:
      keyCtrl_ = FALSE;
      break;

    default:
      HANDLERPASS;
    }
    return 0;
  }

  switch (wparam) {
  case VK_SHIFT:
    keyShift_ = TRUE;
    break;

  case VK_CONTROL:
    keyCtrl_ = TRUE;
    break;
  }

  if (keyShift_ && keyCtrl_) {
    HANDLERPASS;
  }
  else if (keyShift_) {
    switch (wparam) {
    case VK_UP:
    case VK_DOWN:
      Transform(FIJPEG_OP_FLIP_V);
      break;

    case VK_LEFT:
    case VK_RIGHT:
      Transform(FIJPEG_OP_FLIP_H);
      break;
    default:
      HANDLERPASS;
    }
    return 0;
  }

  else if (keyCtrl_) {
    switch (wparam) {
    case VK_UP:
    case VK_DOWN:
      Transform(FIJPEG_OP_ROTATE_180);
      break;
    case VK_LEFT:
      Transform(FIJPEG_OP_ROTATE_270);
      break;
    case VK_RIGHT:
      Transform(FIJPEG_OP_ROTATE_90);
      break;

    default:
      HANDLERPASS;
    }
    return 0;
  }

  switch (wparam) {
  case VK_ESCAPE:
    PostQuitMessage(0);
    break;

  case VK_UP:
  case VK_LEFT:
    PostMessage(
      hwnd_,
      wparam == VK_UP ? WM_VSCROLL : WM_HSCROLL,
      MAKEWPARAM(SB_LINEUP, 0),
      0
      );
    break;

  case VK_DOWN:
  case VK_RIGHT:
    PostMessage(
      hwnd_,
      wparam == VK_DOWN ? WM_VSCROLL : WM_HSCROLL,
      MAKEWPARAM(SB_LINEDOWN, 0),
      0
      );
    break;

  case VK_F5:
    LoadFile();
    break;

  case VK_RETURN:
    Switch();
    break;

  default:
    HANDLERPASS;
  }

  return 0;
}

HANDLERIMPL(OnChar)
{
  switch (toupper((int)wparam)) {
  case 'A':
    Switch();
    break;
  case '-':
	  best_ = false;
	  AdjustSize(0);
	  break;
  case '_':
	  best_ = false;
	  AdjustSize(0);
	  break;
  case '+':
	  best_ = false;
	  AdjustSize(1);
	  break;
  case '=':
	  best_ = false;
	  AdjustSize(1);
	  break;

  case '#':
  case '0':
	  !best_;
      aspect_ = 1.0f;
      DoDC();
    break;

  case 'R':
    LoadFile();
    break;

  default:
    HANDLERPASS;
  }

  return 0;
}

HANDLERIMPL(OnLButton)
{
  switch (msg) {
  case WM_LBUTTONDOWN: {
    
    mbtnDown_ = TRUE;

    mbtnPos_.x = GET_X_LPARAM(lparam);
    mbtnPos_.y = GET_Y_LPARAM(lparam);
    SetCursor(
      LoadCursor(
      nullptr,
      IDC_SIZEALL
      )
      );
    return 0;
  }

  case WM_NCMOUSEMOVE:
  case WM_LBUTTONUP: {
    
    mbtnDown_ = FALSE;
    SetCursor(
      LoadCursor(
      nullptr,
      IDC_ARROW
      )
      );
    if (msg == WM_NCMOUSEMOVE) {
      break;
    }
    return 0;
  }

  } // switch

  HANDLERPASS;
}

HANDLERIMPL(OnMouseMove)
{
  if (mbtnDown_) {
    

    Perform(
      WM_HSCROLL,
      MAKEWPARAM(9, (mbtnPos_.x - GET_X_LPARAM(lparam)) * 1.4f),
      0
      );
    Perform(
      WM_VSCROLL,
      MAKEWPARAM(9, (mbtnPos_.y - GET_Y_LPARAM(lparam)) * 1.4f),
      0
      );
    mbtnPos_.x = GET_X_LPARAM(lparam);
    mbtnPos_.y = GET_Y_LPARAM(lparam);
  }
  HANDLERPASS;
}

HANDLERIMPL(OnScroll)
{
  const bool vscroll = (msg == WM_VSCROLL);
  int SB = vscroll ? SB_VERT : SB_HORZ;
  

  SCROLLINFO si;
  si.cbSize = sizeof(SCROLLINFO);
  si.fMask = SIF_PAGE | SIF_RANGE;
  if (!GetScrollInfo(hwnd_, SB, &si)) {
    return 0;
  }

  if ((signed)si.nPage > si.nMax) {
    return 0;
  }

  LONG v = vscroll ? sp_.y : sp_.x;

  switch (LOWORD(wparam)) {
  case SB_LINEDOWN:
    v += 10;
    break;

  case SB_LINEUP:
    v -= 10;
    break;

  case SB_PAGEDOWN:
    v += 100;
    break;

  case SB_PAGEUP:
    v -= 100;
    break;

  case SB_THUMBPOSITION:
  case SB_THUMBTRACK:
    v = (short)HIWORD(wparam);
    break;

  case 9:
    v += GET_WHEEL_DELTA_WPARAM(wparam);
    break;
  }

  
  v = max(si.nMin, v);
  v = min(si.nMax - (signed)si.nPage, v);
  
  SetScrollPos(hwnd_, SB, v, TRUE);
  InvalidateRect(hwnd_, nullptr, FALSE);
  vscroll ? sp_.y = v : sp_.x = v;

  return 0;
}

HANDLERIMPL(OnContextMenu)
{
  MENUITEMINFO mi;
  mi.cbSize = sizeof(MENUITEMINFO);

  const std::wstring mt = stringtools::formatResourceString(
    IDS_SWITCH, (best_ ? s_switch_free : s_switch_best).c_str());
  mi.fMask = MIIM_STRING | MIIM_STATE;
  mi.dwTypeData = const_cast<wchar_t*>(mt.c_str());
  mi.cch = static_cast<UINT>(mt.length());
  mi.fState = img_.isValid() ? MFS_ENABLED : MFS_DISABLED;

  SetMenuItemInfo(
    GetSubMenu(hctx_, 0),
    ID_MCTX_SWITCH,
    FALSE,
    &mi
    );

  mi.fMask = MIIM_STATE;
  mi.fState = img_.isValid() &&
    img_.getOriginalInformation().getFormat() == FIF_JPEG ? MFS_ENABLED : MFS_DISABLED;

  HMENU hSub = GetSubMenu(hctx_, 0);
  unsigned subCount = GetMenuItemCount(hSub);
  SetMenuItemInfo(
    hSub,
    subCount - 4,
    TRUE,
    &mi
    );

  MenuMethod menuMethod;
  menuMethod(hSub, GetResampleMethod());

  TrackPopupMenu(
    hSub,
    TPM_LEFTALIGN | TPM_LEFTBUTTON | TPM_RIGHTBUTTON,
    GET_X_LPARAM(lparam),
    GET_Y_LPARAM(lparam),
    0,
    hwnd_,
    nullptr
    );

  HANDLERPASS;
}

HANDLERIMPL(OnCommand)
{
  switch (LOWORD(wparam)) {
  case ID_MCTX_OPEN: {
    SetStatus(s_opening.c_str());
    SHELLEXECUTEINFO shi;
    ZeroMemory(&shi, sizeof(shi));
    shi.cbSize = sizeof(shi);
    shi.fMask = SEE_MASK_NOASYNC | SEE_MASK_DOENVSUBST | SEE_MASK_HMONITOR |
      SEE_MASK_NOZONECHECKS;
    shi.lpFile = file_.c_str();
    shi.hMonitor = ::MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    shi.hwnd = hwnd_;
    shi.nShow = SW_SHOWDEFAULT;
    ShellExecuteEx(&shi);
    SetStatus();
    break;
  }

  case ID_MCTX_OPENANOTHER:
    BrowseNew();
    break;

  case ID_MCTX_SHOWPROPERTIES:
    PostMessage(hwnd_, WM_LBUTTONDBLCLK, 0, 0);
    break;

  case ID_MCTX_EXIT:
    SetStatus(s_quitting.c_str());
    PostQuitMessage(0);
    break;

  case ID_MCTX_EXPLORERMENU:
    SetStatus(s_explorermenu.c_str());
    try {
      ExplorerMenu(hwnd_, file_);
    }
    catch (Exception
#ifdef _DEBUG
      &E
#endif
      ) {
      
    }
    SetStatus();
    break;

  case ID_MCTX_SWITCH:
    Switch();
    break;

  case ID_RESAMPLEMETHOD_BOX:
    SetResampleMethod(FILTER_BOX);
    break;

  case ID_RESAMPLEMETHOD_BILINEAR:
    SetResampleMethod(FILTER_BILINEAR);
    break;

  case ID_RESAMPLEMETHOD_BICUBIC:
    SetResampleMethod(FILTER_BICUBIC);
    break;

  case ID_RESAMPLEMETHOD_BSPLINE:
    SetResampleMethod(FILTER_BSPLINE);
    break;

  case ID_RESAMPLEMETHOD_CATMULL:
    SetResampleMethod(FILTER_CATMULLROM);
    break;

  case ID_RESAMPLEMETHOD_LANCZOS3:
    SetResampleMethod(FILTER_LANCZOS3);
    break;

  case ID_LOSSLESSTRANSFORM_LEFT:
    Transform(FIJPEG_OP_ROTATE_270);
    break;

  case ID_LOSSLESSTRANSFORM_RIGHT:
    Transform(FIJPEG_OP_ROTATE_90);
    break;

  case ID_LOSSLESSTRANSFORM_181:
    Transform(FIJPEG_OP_ROTATE_180);
    break;

  case ID_LOSSLESSTRANSFORM_VERTICALFLIP:
    Transform(FIJPEG_OP_FLIP_V);
    break;

  case ID_LOSSLESSTRANSFORM_HORIZONTALFLIP:
    Transform(FIJPEG_OP_FLIP_H);
    break;

  }

  return 0;
}

HANDLERIMPL(OnSysCommand)
{
  if (wparam == WM_USER) {
    MessageBox(
      hwnd_,
      _T("FastPreview 4.0\n\n© 2006-2014 by Nils Maier\n\nSee the License.* files for more licensing information."),
      _T("About"),
      MB_ICONINFORMATION
      );
    return 0;
  }

  HANDLERPASS;
}

HANDLERIMPL(OnWatch)
{
  
  if (inTransformation_) {
    HANDLERPASS;
  }

  switch (wparam) {
  case FILE_ACTION_REMOVED:
  
    PostQuitMessage(0);
    break;
  default:
    try {
      FileAttr(file_).getSize();
    }
    catch (Exception) {
      
      PostQuitMessage(0);
    }
    
    LoadFile();
    break;
  }
  return 0;
}

HANDLERIMPL(OnTimer)
{
  switch (wparam) {
  case IDT_RELOAD:
  {
    
    if (inTransformation_) {
      break;
    }
    try {
      if (FileAttr(file_) != fileAttr_) {
        LoadFile();
      }
    }
    catch (Exception) {
      PostQuitMessage(0);
    }
  }
    break;
    case IDT_AACCEPT:
	{
		KillTimer(hwnd_, IDT_AACCEPT);
			aspect_ = newAspect_;
			DoDC();
		}
		break;
}
  return 0;
}
HANDLERIMPL(OnMoving)
{
  WorkArea area(hwnd_);
  Rect *rc = reinterpret_cast<Rect*>(lparam);
  auto dx = (rc->left < area.left) ?
    (area.left - rc->left) :
    ((rc->right > area.right) ? (area.right - rc->right) : 0);
  auto dy = (rc->top < area.top) ?
    (area.top - rc->top) :
    ((rc->bottom > area.bottom) ? (area.bottom - rc->bottom) : 0);
  OffsetRect(rc, dx, dy);

  return TRUE;
}

HANDLERIMPL(OnSize)
{
  if (msg == WM_SIZE) {
    SendMessage(hstatus_, msg, wparam, lparam);
    HANDLERPASS;
  }

  RECT r;
  GetWindowRect(hwnd_, &r);
  CopyRect(reinterpret_cast<LPRECT>(lparam), &r);
  return TRUE;
}

void MainWindow::Show()
{
  LoadFile();
  ShowWindow(hwnd_, SW_SHOWNORMAL);

  try {
    watcher_.reset(new WatcherThread(file_, hwnd_));
  }
  catch (Exception &E) {
    E.show(hwnd_);
  }
}

void MainWindow::PreAdjustWindow()
{
  WINDOWINFO wi;
  ZeroMemory(&wi, sizeof(WINDOWINFO));
  GetWindowInfo(
    hwnd_,
    &wi
    );
  Rect wr(wi.rcWindow), cr(wi.rcClient);
 

  WorkArea area(hwnd_);

  // get max displayable image size

  unsigned
    max_width = area.width() - (wr.width() - cr.width()),
    max_height = area.height() - (wr.height() - cr.height());

  // if in best fit mode figure out the largest size that will fit the screen

  if (best_ && img_.isValid()) {
    bestAspect_ = 1.0f;
    if (loaded_image_width > max_width) {
      bestAspect_ = (float)max_width / (float)loaded_image_width;
    }
    if (loaded_image_height > max_height) {
      bestAspect_ = (float)max_height / (float)loaded_image_height;
    }
  }
  
  // figure out if the image needs scroll bars

  const bool hs = display_image_width > max_width, vs = display_image_height > max_height;

  SCROLLINFO si = {
    sizeof(si),
    SIF_PAGE | SIF_RANGE,
    0,
    0,
    0,
    0,
    0
  };
  if (hs) {
    si.nPage = max_width;
    si.nMax = display_image_width + (vs ? GetSystemMetrics(SM_CXVSCROLL) : 0);
    
  }
  SetScrollInfo(
    hwnd_,
    SB_HORZ,
    &si,
    TRUE
    );

  if (vs) {
    si.nPage = max_height;
    si.nMax = display_image_height + (hs ? GetSystemMetrics(SM_CYHSCROLL) : 0);
  }

  SetScrollInfo(
    hwnd_,
    SB_VERT,
    &si,
    TRUE
    );

  wheeling_ = hs || vs;

  if (vs) {
    ShowScrollBar(hwnd_, SB_VERT, TRUE);
  }
  if (hs) {
    ShowScrollBar(hwnd_, SB_HORZ, TRUE);
  }


  clientHeight_ = min(max_height, display_image_height);
  clientWidth_ = min(max_width, display_image_width);
  if (hs) {
	  clientHeight_ = min((clientHeight_ + GetSystemMetrics(SM_CYHSCROLL)), max_height);
  }
  else {
	  clientHeight_ = min(clientHeight_, max_height);
  }
  if (vs) {
	  clientWidth_ = min((clientWidth_ + GetSystemMetrics(SM_CXVSCROLL)), max_width);
  }
  else {
	  clientWidth_ = min(clientWidth_, max_width);
  }

  Rect rw(clientWidth_, clientHeight_);
  AdjustWindowRectEx(
    &rw,
    wi.dwStyle,
    FALSE,
    wi.dwExStyle
    );
    
  WorkArea wa(hwnd_);
  rw.centerIn(wa);
  
  InvalidateRect(hwnd_, NULL, FALSE);

  MoveWindow(
	  hwnd_,
	  rw.left,
	  rw.top,
	  rw.width(),
	  rw.height(),
	  TRUE
	  );

  InvalidateRect(hwnd_, NULL, FALSE);
  

}

void MainWindow::LoadFile()
{
  if (inTransformation_) {
    return;
  }
  SetStatus(s_loading.c_str());
  FreeFile();

  fileAttr_ = FileAttr(file_);

  img_.clear();
  bool load;
  {
    Priority prio(HIGH_PRIORITY_CLASS);
    load = img_.load(file_.c_str());
	display_image_height = loaded_image_height = img_.getHeight();
	display_image_width = loaded_image_width = img_.getWidth();
	sp_.x = sp_.y = 0;
  }

  if (!load) {
    sp_.x = sp_.y = 0;
    clientHeight_ = clientWidth_ = 400;

    ShowScrollBar(hwnd_, SB_BOTH, FALSE);
    PreAdjustWindow();

    CreateDC();

    if (devcontext_ == INVALID_HANDLE_VALUE) {
      throw WindowsException();
    }
    SelectObject(
      devcontext_,
      GetStockObject(DEFAULT_GUI_FONT)
      );

    RECT  rc = {0, 0, (LONG)Width(), (LONG)Height()};
    FillRect(
      devcontext_,
      &rc,
      (HBRUSH)(COLOR_BTNFACE + 1)
      );
    if (!DrawText(
      devcontext_,
      s_err_load.c_str(),
      -1,
      &rc,
      DT_SINGLELINE | DT_CENTER | DT_VCENTER
      )) {
      throw WindowsException();
    }

    CenterWindow();
    SetTitle();
    InvalidateRect(hwnd_, nullptr, FALSE);
  }
  else {
    if (img_.getFormat() == FIF_BMP && img_.getBitsPerPixel() == 32) {
      img_.convertTo24Bits();
    }
    DoDC();
  }
  SetStatus();
}

void MainWindow::DoDC()
{
	if (!best_ && aspect_ > 0.95 && aspect_ < 1.05) {
		const UINT w = display_image_width = loaded_image_width;
		const UINT h = display_image_height = loaded_image_height;
		img_.draw(devcontext_, Rect(0, 0, loaded_image_width, loaded_image_height));
	}
	else {
   
	  const UINT w = display_image_width = loaded_image_width * aspect_;
	  const UINT h = display_image_height = loaded_image_height * aspect_;
    
	  // enlarge device context if required
	  
	  if (w > GetDeviceCaps(devcontext_, HORZRES) || h > GetDeviceCaps(devcontext_, VERTRES))
	  {
		  HBITMAP hbm = CreateCompatibleBitmap(devcontext_, w, h);
		  SelectObject(devcontext_, hbm);
		  DeleteObject(hbm);
	  }
	  
	// rescale and draw - eliminated one trip through FreeImage by cacheing resize_method
	// if the bitmap is huge this gets really silly

	try {
      FreeImage::WinImage r(img_);
      r.rescale(w, h, resize_method);
      r.draw(devcontext_, Rect(0, 0, w, h));
    }
    catch (std::exception& ex) {
      SetStatus(stringtools::convert(ex.what()));
    }
	  
  }
    
  PreAdjustWindow();
  
}

void MainWindow::FreeFile()
{
  img_.clear();
}

void MainWindow::CreateDC()
{
	if (devcontext_ == nullptr) {
		devcontext_ = CreateCompatibleDC(nullptr);
		
		HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
		MONITORINFO info;
		info.cbSize = sizeof(MONITORINFO);
		GetMonitorInfo(monitor, &info);
		int monitor_width = info.rcMonitor.right - info.rcMonitor.left;
		int monitor_height = info.rcMonitor.bottom - info.rcMonitor.top;
					
		HBITMAP mbmp = CreateBitmap(3*monitor_width, 3*monitor_height, 1, 32, NULL);
		SelectObject(devcontext_, mbmp);
		DeleteObject(mbmp);
	}
 }

void MainWindow::ProcessPaint() const
{
  for (MSG msg; PeekMessage(&msg, hwnd_, 0, 0, PM_REMOVE | PM_QS_PAINT);) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
}

void MainWindow::Switch()
{
    best_ = false;

}

void MainWindow::AdjustSize(int direction)
{
	int test = 1;
	if (best_) {
		Switch();
	}
	
	if (direction == 1) {
		newAspect_ = min(5.0f, newAspect_ + 0.25f);
	}
	if (direction == 0){
		newAspect_ = max(0.25f, newAspect_ - 0.25f);
		}
 
  SetTitle();
  KillTimer(hwnd_, IDT_AACCEPT);
  SetTimer(hwnd_, IDT_AACCEPT, 100, nullptr);

}

void MainWindow::SetTitle()
{
  std::wstringstream ss;
  ss << stringtools::formatResourceString(
    IDS_MAINTITLE, file_.substr(file_.rfind('\\') + 1).c_str());
  Perform(
    WM_SETTEXT,
    0,
    (LPARAM)ss.str().c_str()
    );
}

void MainWindow::SetStatus(const std::wstring &aText)
{

}

void MainWindow::CenterWindow() const
{
  Rect rw;
  GetWindowRect(
    hwnd_,
    &rw
    );
  WorkArea wa(hwnd_);
  rw.centerIn(wa);
  MoveWindow(
    hwnd_,
    rw.left,
    rw.top,
    rw.width(),
    rw.height(),
    TRUE
    );
}

FREE_IMAGE_FILTER MainWindow::GetResampleMethod() const
{
  MenuMethod menuMethod;
  FREE_IMAGE_FILTER rv = FILTER_LANCZOS3;
  uint32_t f = 0;
  if (reg_.get(L"appFIResampleMethod", f)) {
    if (menuMethod.valid(f)) {
      rv = (FREE_IMAGE_FILTER)f;
    }
  }
  return rv;
}

void MainWindow::SetResampleMethod(FREE_IMAGE_FILTER m)
{
  reg_.set(L"appFIResampleMethod", (uint32_t)m);
  DoDC();
}

void MainWindow::Transform(FREE_IMAGE_JPEG_OPERATION aTrans)
{
  if (!img_.isValid() || img_.getOriginalInformation().getFormat() != FIF_JPEG) {
    throw Exception("Not a JPEG!");
  }

  try {
    AutoToggle a(inTransformation_, true);
    Thread::Suspender wts(watcher_.get());

    if (!FreeImage_JPEGTransform(
      stringtools::WStringToString(file_).c_str(),
      stringtools::WStringToString(file_).c_str(),
      aTrans
      )) {
      std::wstringstream ss;
      ss << L"Cannot transform " << file_;
      throw Exception(ss.str());
    }
    LoadFile();
  }
  catch (Exception &ex) {
    ex.show(hwnd_, L"Error transforming file.");
  }
}

void MainWindow::BrowseNew()
{
  if (!openDlg->Execute(hwnd_, file_)) {
    return;
  }
  watcher_.reset();
  file_ = openDlg->getFileName();
  watcher_.reset(new WatcherThread(file_, hwnd_));

  LoadFile();
}

HICON MainWindow::GetIcon(const std::wstring& File)
{
  SHFILEINFO shfi;
  ZeroMemory(&shfi, sizeof(SHFILEINFO));
  SHGetFileInfo(
    File.c_str(),
    0,
    &shfi,
    sizeof(SHFILEINFO),
    SHGFI_ICON | SHGFI_SMALLICON
    );
  return shfi.hIcon ?
    shfi.hIcon :
    LoadIcon((HINSTANCE)GetCurrentProcess(), MAKEINTRESOURCE(IDI_APP));
}
