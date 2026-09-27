// OB Taskmgr: a minimal task manager. One NtQuerySystemInformation call fetches all processes;
// a virtual ListView paints only the visible rows.
#define _UNICODE
#include <windows.h>
#include <winternl.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <shellapi.h>
#include <winioctl.h>
#include <pdh.h>
#include <stdio.h>
#include <stdlib.h>

// ---- UI text: every user-visible string lives here ----
#define TXT_TITLE         L"OB Taskmgr"
#define TXT_SEARCH_HINT   L"Search name or PID"
#define TXT_END_TASK      L"End task"
#define TXT_COL_NAME      L"Name"
#define TXT_COL_PID       L"PID"
#define TXT_COL_CPU       L"CPU"
#define TXT_COL_MEM       L"Memory"
#define TXT_STATUS        L"Processes %d    CPU %.0f%%    Memory %lu%%  (%.1f / %.1f GB)"
#define TXT_CPU_TEMP      L"CPU %d°C"
#define TXT_DISK_TEMP     L"Disk %d°C"
#define TXT_CONFIRM_END   L"End %ls (PID %lu)?\nAny unsaved data will be lost."
#define TXT_END_FAILED    L"Failed to end process (error %lu)%ls"
#define TXT_ACCESS_DENIED L"\nAccess denied. Try running OB Taskmgr as administrator."
#define TXT_OPEN_LOCATION L"Open file location"
#define TXT_COPY_NAME     L"Copy name"
#define TXT_RESTORE       L"Restore"
#define TXT_EXIT          L"Exit"

// Highlight thresholds: CPU as % of the whole machine (same as the CPU column), memory as private working set
#define CPU_WARN 10.0
#define CPU_HIGH 30.0
#define MEM_WARN (1ULL << 30) // 1 GB
#define MEM_HIGH (4ULL << 30) // 4 GB

enum { ID_SEARCH = 1, ID_KILL, ID_LIST };
#define WM_TRAY (WM_APP + 1)

typedef struct { // mingw headers lack STORAGE_TEMPERATURE_DATA_DESCRIPTOR; defined here per the Windows SDK
    WORD Index;
    SHORT Temperature, OverThreshold, UnderThreshold;
    BOOLEAN OverThresholdChangable, UnderThresholdChangable, EventGenerated;
    BYTE Reserved0;
    DWORD Reserved1;
} TEMP_INFO;
typedef struct {
    DWORD Version, Size;
    SHORT CriticalTemperature, WarningTemperature;
    WORD InfoCount;
    BYTE Reserved0[2];
    DWORD Reserved1[2];
    TEMP_INFO Info[1];
} TEMP_DESC;

typedef struct { // first part of SYSTEM_PROCESS_INFORMATION (includes undocumented fields)
    ULONG NextEntryOffset, NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount, NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime, UserTime, KernelTime;
    UNICODE_STRING ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId;
} NTPROC;

typedef struct {
    WCHAR name[128];
    DWORD pid;
    LONGLONG create, time; // creation time / total CPU time (100 ns units)
    double cpu;            // %
    ULONGLONG mem;         // private working set, same metric as Task Manager's "Memory" column
} Proc;

typedef struct { Proc *p; int n, cap; } Snap;

static HWND hWnd, hList, hHeader, hSearch, hKill, hStatus;
static HFONT boldFont;
static int rightW; // width of the status bar's temperature part
static Snap snaps[2]; static int si;  // snaps[si] is the current sample, the other is the previous one
static Proc *shown; static int *view, nView, viewCap;
static BYTE *buf; static ULONG bufSize;
static ULONGLONG lastTotal, lastIdle;
static int sortCol = 3, sortDesc = 1, dpi = 96, ctlH, btnW;
static WCHAR query[128];
static NOTIFYICONDATAW nid;
static UINT wmTaskbarCreated, wmShowMe;
static PDH_HQUERY pq;
static PDH_HCOUNTER pc;
static int diskT[16], nDisk, tick;

static int S(int x) { return MulDiv(x, dpi, 96); }
static ULONGLONG U(FILETIME f) { return (ULONGLONG)f.dwHighDateTime << 32 | f.dwLowDateTime; }

// Status bar parts: [processes/CPU/memory][temperatures, right-aligned][size grip spacer].
// The grip gets its own part, otherwise it covers right-aligned text in the last part.
static void LayoutStatus(void) {
    RECT r;
    GetClientRect(hStatus, &r);
    int grip = GetSystemMetrics(SM_CXVSCROLL), parts[3] = { r.right - grip - rightW, r.right - grip, -1 };
    SendMessageW(hStatus, SB_SETPARTS, 3, (LPARAM)parts);
    SendMessageW(hStatus, SB_SETTEXTW, 2 | SBT_NOBORDERS, (LPARAM)L"");
}

static int Match(const Proc *p) {
    WCHAR s[12];
    if (!query[0]) return 1;
    _snwprintf(s, 12, L"%lu", p->pid);
    return StrStrIW(p->name, query) || !wcscmp(s, query);
}

#define CMP(a, b) (((a) > (b)) - ((a) < (b)))
static int Cmp(const void *a, const void *b) {
    const Proc *x = &snaps[si].p[*(const int *)a], *y = &snaps[si].p[*(const int *)b];
    int r = sortCol == 0 ? _wcsicmp(x->name, y->name)
          : sortCol == 1 ? CMP(x->pid, y->pid)
          : sortCol == 2 ? CMP(x->cpu, y->cpu)
          :                CMP(x->mem, y->mem);
    if (!r) r = CMP(x->pid, y->pid);
    return sortDesc ? -r : r;
}

static void CellText(const Proc *p, int col, WCHAR *s, int n) {
    switch (col) {
    case 0: lstrcpynW(s, p->name, n); break;
    case 1: _snwprintf(s, n, L"%lu", p->pid); break;
    case 2: _snwprintf(s, n, L"%.1f%%", p->cpu); break;
    default: _snwprintf(s, n, L"%.1f MB", p->mem / 1048576.0); break;
    }
}

static int Level(const Proc *p) { // 0 normal, 1 warning (yellow), 2 critical (red)
    if (p->cpu >= CPU_HIGH || p->mem >= MEM_HIGH) return 2;
    return p->cpu >= CPU_WARN || p->mem >= MEM_WARN;
}

static int RowSame(const Proc *a, const Proc *b) {
    WCHAR x[128], y[128];
    if (Level(a) != Level(b)) return 0; // repaint when a threshold is crossed even if the text is unchanged
    for (int col = 0; col < 4; col++) {
        CellText(a, col, x, 128);
        CellText(b, col, y, 128);
        if (wcscmp(x, y)) return 0;
    }
    return 1;
}

static void ApplyView(void) {
    Snap *c = &snaps[si];
    Proc *oldShown = shown;
    int oldN = nView, old[256];
    int top = ListView_GetTopIndex(hList), rows = min(ListView_GetCountPerPage(hList) + 1, 256);
    for (int i = 0; i < rows && top + i < oldN; i++) old[i] = view[top + i];
    int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    DWORD selPid = sel >= 0 && sel < nView ? shown[view[sel]].pid : 0;

    if (viewCap < c->n) view = realloc(view, (viewCap = c->cap) * sizeof(int));
    nView = 0;
    for (int i = 0; i < c->n; i++)
        if (Match(&c->p[i])) view[nView++] = i;
    shown = c->p;
    qsort(view, nView, sizeof(int), Cmp);
    ListView_SetItemCountEx(hList, nView, LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);

    int now = -1;
    for (int i = 0; selPid && i < nView; i++)
        if (shown[view[i]].pid == selPid) { now = i; break; }
    if (ListView_GetNextItem(hList, -1, LVNI_SELECTED) != now) {
        ListView_SetItemState(hList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        if (now >= 0) ListView_SetItemState(hList, now, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }

    // Repainting is the main cost (~1 ms per ListView row): redraw only visible rows whose text changed,
    // one row at a time, otherwise the bounding box of several invalid rows turns into one big repaint
    UpdateWindow(hList); // first flush what SetItemCountEx (rows added/removed at the end) and selection changes invalidated
    if (ListView_GetTopIndex(hList) != top) InvalidateRect(hList, NULL, FALSE);
    else for (int i = 0; i < rows && top + i < min(oldN, nView); i++)
        if (!RowSame(&oldShown[old[i]], &shown[view[top + i]])) {
            ListView_RedrawItems(hList, top + i, top + i);
            UpdateWindow(hList);
        }
}

// Disk temperature: documented storage IOCTL, works without admin. Skips HDDs so polling doesn't wake sleeping disks.
static void ReadDiskTemps(void) {
    nDisk = 0;
    for (int i = 0; i < 16; i++) {
        WCHAR path[32];
        _snwprintf(path, 32, L"\\\\.\\PhysicalDrive%d", i);
        HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        STORAGE_PROPERTY_QUERY q = { StorageDeviceSeekPenaltyProperty, PropertyStandardQuery };
        DEVICE_SEEK_PENALTY_DESCRIPTOR seek;
        BYTE out[512];
        DWORD n;
        if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &seek, sizeof seek, &n, NULL) || !seek.IncursSeekPenalty) {
            q.PropertyId = StorageDeviceTemperatureProperty;
            if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, out, sizeof out, &n, NULL) && ((TEMP_DESC *)out)->InfoCount)
                diskT[nDisk++] = ((TEMP_DESC *)out)->Info[0].Temperature;
        }
        CloseHandle(h);
    }
}

// CPU temperature: ACPI thermal zone (from BIOS/EC, usually near the CPU on laptops); the hottest zone wins, 0 if none
static int CpuTemp(void) {
    static BYTE out[4096];
    PDH_FMT_COUNTERVALUE_ITEM_W *it = (PDH_FMT_COUNTERVALUE_ITEM_W *)out;
    DWORD size = sizeof out, n = 0;
    double t = 0;
    if (!pc || PdhCollectQueryData(pq) || PdhGetFormattedCounterArrayW(pc, PDH_FMT_DOUBLE, &size, &n, it)) return 0;
    for (DWORD i = 0; i < n; i++) t = max(t, it[i].FmtValue.doubleValue - 273.15); // Kelvin
    return (int)(t + 0.5);
}

static void Refresh(void) {
    ULONG need = 0;
    NTSTATUS st;
    while ((st = NtQuerySystemInformation(SystemProcessInformation, buf, bufSize, &need)) == (NTSTATUS)0xC0000004L)
        buf = realloc(buf, bufSize = need + 64 * 1024); // STATUS_INFO_LENGTH_MISMATCH
    if (st < 0) return;

    FILETIME fi, fk, fu;
    GetSystemTimes(&fi, &fk, &fu);
    ULONGLONG total = U(fk) + U(fu), idle = U(fi); // kernel time includes idle
    double dt = lastTotal ? (double)(total - lastTotal) : 0;
    double sysCpu = dt > 0 ? (dt - (double)(idle - lastIdle)) * 100 / dt : 0;
    lastTotal = total, lastIdle = idle;

    Snap *o = &snaps[si], *c = &snaps[si ^= 1];
    c->n = 0;
    for (NTPROC *e = (NTPROC *)buf;; e = (NTPROC *)((BYTE *)e + e->NextEntryOffset)) {
        DWORD pid = (DWORD)(ULONG_PTR)e->UniqueProcessId;
        if (pid) { // skip System Idle Process
            if (c->n == c->cap) c->p = realloc(c->p, (c->cap += 256) * sizeof(Proc));
            Proc *p = &c->p[c->n++];
            int len = min(e->ImageName.Length / 2, 127);
            if (len) memcpy(p->name, e->ImageName.Buffer, len * 2);
            p->name[len] = 0;
            p->pid = pid;
            p->create = e->CreateTime.QuadPart;
            p->time = e->UserTime.QuadPart + e->KernelTime.QuadPart;
            p->mem = e->WorkingSetPrivateSize.QuadPart;
            p->cpu = 0;
            // O(n²) match against the previous sample: <1 ms for a few hundred processes; use a hash for tens of thousands
            for (int i = 0; dt > 0 && i < o->n; i++)
                if (o->p[i].pid == pid && o->p[i].create == p->create) {
                    p->cpu = (p->time - o->p[i].time) * 100.0 / dt;
                    break;
                }
        }
        if (!e->NextEntryOffset) break;
    }

    MEMORYSTATUSEX ms = { sizeof ms };
    GlobalMemoryStatusEx(&ms);
    if (tick++ % 10 == 0) ReadDiskTemps(); // disk temps change slowly: read every 10 s (each read is a ~10 ms device round trip)
    WCHAR s[128], t[256];
    _snwprintf(s, 128, TXT_STATUS, c->n, sysCpu, ms.dwMemoryLoad,
               (ms.ullTotalPhys - ms.ullAvailPhys) / 1073741824.0, ms.ullTotalPhys / 1073741824.0);
    SendMessageW(hStatus, SB_SETTEXTW, 0, (LPARAM)s);
    // Temperature part (two \t = right-aligned); unavailable readings are omitted
    int ct = CpuTemp(), k = _snwprintf(t, 16, L"\t\t");
    if (ct) k += _snwprintf(t + k, 32, TXT_CPU_TEMP, ct);
    for (int i = 0; i < nDisk; i++) {
        if (!i && ct) k += _snwprintf(t + k, 8, L"    ");
        k += _snwprintf(t + k, 32, i ? L" · %d°C" : TXT_DISK_TEMP, diskT[i]);
    }
    SIZE sz = { 0 };
    HDC dc = GetDC(hStatus);
    HGDIOBJ of = SelectObject(dc, (HFONT)SendMessageW(hStatus, WM_GETFONT, 0, 0));
    GetTextExtentPoint32W(dc, t + 2, k - 2, &sz);
    SelectObject(dc, of);
    ReleaseDC(hStatus, dc);
    int w = k > 2 ? sz.cx + S(16) : 0;
    if (w != rightW) {
        rightW = w;
        LayoutStatus();
    }
    SendMessageW(hStatus, SB_SETTEXTW, 1, (LPARAM)t);
    ApplyView();
}

static void SetSortArrows(void) {
    HWND h = ListView_GetHeader(hList);
    for (int i = 0; i < 4; i++) {
        HDITEMW it = { HDI_FORMAT };
        Header_GetItem(h, i, &it);
        it.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == sortCol) it.fmt |= sortDesc ? HDF_SORTDOWN : HDF_SORTUP;
        Header_SetItem(h, i, &it);
    }
}

static void KillSelected(void) {
    int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    if (sel < 0) return;
    DWORD pid = shown[view[sel]].pid; // the timer keeps refreshing while the MessageBox is open, so copy it first
    WCHAR msg[256];
    _snwprintf(msg, 256, TXT_CONFIRM_END, shown[view[sel]].name, pid);
    if (MessageBoxW(hWnd, msg, TXT_TITLE, MB_OKCANCEL | MB_ICONWARNING) != IDOK) return;
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h || !TerminateProcess(h, 1)) {
        DWORD err = GetLastError();
        _snwprintf(msg, 256, TXT_END_FAILED, err, err == ERROR_ACCESS_DENIED ? TXT_ACCESS_DENIED : L"");
        MessageBoxW(hWnd, msg, TXT_TITLE, MB_ICONERROR);
    }
    if (h) CloseHandle(h);
    Refresh();
}

static void CopyText(const WCHAR *s) {
    int n = (lstrlenW(s) + 1) * 2;
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!g) return;
    memcpy(GlobalLock(g), s, n);
    GlobalUnlock(g);
    if (OpenClipboard(hWnd)) {
        EmptyClipboard();
        if (SetClipboardData(CF_UNICODETEXT, g)) g = NULL; // the clipboard owns it now
        CloseClipboard();
    }
    if (g) GlobalFree(g);
}

// Right-click (or Shift+F10 / menu key) on a row: Copy name / Open file location / End task
static void ListMenu(LPARAM lp) {
    int sel = ListView_GetNextItem(hList, -1, LVNI_SELECTED);
    if (sel < 0) return;
    POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
    if (lp == -1) { // keyboard: open below the selected row
        RECT r;
        ListView_GetItemRect(hList, sel, &r, LVIR_LABEL);
        pt.x = r.left, pt.y = r.bottom;
        ClientToScreen(hList, &pt);
    }
    WCHAR name[128], path[MAX_PATH], args[MAX_PATH + 16];
    DWORD n = MAX_PATH, pid = shown[view[sel]].pid; // copy: the timer keeps refreshing while the menu is open
    lstrcpynW(name, shown[view[sel]].name, 128);
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    BOOL hasPath = h && QueryFullProcessImageNameW(h, 0, path, &n); // fails for System, Registry, etc.
    if (h) CloseHandle(h);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 3, TXT_COPY_NAME);
    AppendMenuW(m, MF_STRING | (hasPath ? 0 : MF_GRAYED), 2, TXT_OPEN_LOCATION);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 1, TXT_END_TASK);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(m);
    if (cmd == 1) KillSelected();
    else if (cmd == 2) {
        _snwprintf(args, MAX_PATH + 16, L"/select,\"%ls\"", path);
        ShellExecuteW(hWnd, NULL, L"explorer.exe", args, NULL, SW_SHOWNORMAL);
    } else if (cmd == 3) CopyText(name);
}

static void Restore(void) {
    int hidden = IsIconic(hWnd);
    ShowWindow(hWnd, hidden ? SW_RESTORE : SW_SHOW); // SW_RESTORE on a shown window would un-maximize it
    SetForegroundWindow(hWnd);
    if (hidden) Refresh(); // nothing was refreshed while hidden
}

static void TrayMenu(void) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, TXT_RESTORE);
    AppendMenuW(m, MF_STRING, 2, TXT_EXIT);
    SetMenuDefaultItem(m, 1, FALSE);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hWnd); // otherwise the menu won't close when clicking outside it
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
    PostMessageW(hWnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (cmd == 1) Restore();
    else if (cmd == 2) DestroyWindow(hWnd);
}

// The header sends NM_CUSTOMDRAW only to its parent ListView, which doesn't forward it, so subclass the ListView
static LRESULT CALLBACK ListProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref) {
    if (msg == WM_NOTIFY && ((NMHDR *)lp)->hwndFrom == hHeader && ((NMHDR *)lp)->code == NM_CUSTOMDRAW) {
        NMCUSTOMDRAW *cd = (NMCUSTOMDRAW *)lp; // header: bold text + bottom separator line
        if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
        if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
            SelectObject(cd->hdc, boldFont);
            return CDRF_NEWFONT;
        }
        if (cd->dwDrawStage == CDDS_POSTPAINT) {
            RECT r;
            GetClientRect(hHeader, &r);
            r.top = r.bottom - 1; // hairline at any DPI
            FillRect(cd->hdc, &r, GetSysColorBrush(COLOR_BTNSHADOW));
        }
        return CDRF_DODEFAULT;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == wmTaskbarCreated) { // re-add the tray icon after Explorer restarts
        Shell_NotifyIconW(NIM_ADD, &nid);
        return 0;
    }
    if (msg == wmShowMe) { // a second launch asked us to come to the front
        Restore();
        return 0;
    }
    switch (msg) {
    case WM_CREATE: {
        NONCLIENTMETRICSW nm = { sizeof nm };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof nm, &nm, 0);
        HFONT f = CreateFontIndirectW(&nm.lfMessageFont);
        HDC dc = GetDC(h); // size the search box and button from font metrics so they follow DPI and the system font
        HGDIOBJ of = SelectObject(dc, f);
        TEXTMETRICW tm;
        SIZE sz;
        GetTextMetricsW(dc, &tm);
        GetTextExtentPoint32W(dc, TXT_END_TASK, lstrlenW(TXT_END_TASK), &sz);
        SelectObject(dc, of);
        ReleaseDC(h, dc);
        ctlH = tm.tmHeight + S(6);
        btnW = sz.cx + S(24);
        hSearch = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                  0, 0, 0, 0, h, (HMENU)ID_SEARCH, NULL, NULL);
        SendMessageW(hSearch, EM_SETCUEBANNER, TRUE, (LPARAM)TXT_SEARCH_HINT);
        hKill = CreateWindowW(L"BUTTON", TXT_END_TASK, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              0, 0, 0, 0, h, (HMENU)ID_KILL, NULL, NULL);
        hList = CreateWindowW(WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                              0, 0, 0, 0, h, (HMENU)ID_LIST, NULL, NULL);
        ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        SetWindowTheme(hList, L"Explorer", NULL);
        hStatus = CreateWindowW(STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, h, NULL, NULL, NULL);
        HWND ctl[] = { hSearch, hKill, hList, hStatus };
        for (int i = 0; i < 4; i++) SendMessageW(ctl[i], WM_SETFONT, (WPARAM)f, FALSE);
        hHeader = ListView_GetHeader(hList);
        SetWindowSubclass(hList, ListProc, 0, 0);
        LOGFONTW lf = nm.lfMessageFont;
        lf.lfWeight = FW_BOLD;
        boldFont = CreateFontIndirectW(&lf);
        static const WCHAR *cols[] = { TXT_COL_NAME, TXT_COL_PID, TXT_COL_CPU, TXT_COL_MEM };
        static const int w[] = { 280, 80, 80, 110 };
        for (int i = 0; i < 4; i++) {
            LVCOLUMNW col = { LVCF_TEXT | LVCF_WIDTH | LVCF_FMT, i ? LVCFMT_RIGHT : LVCFMT_LEFT, S(w[i]), (LPWSTR)cols[i] };
            ListView_InsertColumn(hList, i, &col);
        }
        SetSortArrows();
        if (!PdhOpenQueryW(NULL, 0, &pq) && PdhAddEnglishCounterW(pq, L"\\Thermal Zone Information(*)\\Temperature", 0, &pc))
            pc = NULL; // no thermal zone: hide CPU temperature
        nid.cbSize = sizeof nid;
        nid.hWnd = h;
        nid.uID = 1;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_TRAY;
        nid.hIcon = LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON,
                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        lstrcpynW(nid.szTip, TXT_TITLE, 128);
        // When running as admin, UIPI blocks tray messages from Explorer (normal integrity); allow them
        ChangeWindowMessageFilterEx(h, WM_TRAY, MSGFLT_ALLOW, NULL);
        ChangeWindowMessageFilterEx(h, wmTaskbarCreated, MSGFLT_ALLOW, NULL);
        ChangeWindowMessageFilterEx(h, wmShowMe, MSGFLT_ALLOW, NULL); // a non-admin second launch must reach an admin instance
        Shell_NotifyIconW(NIM_ADD, &nid); // the tray icon stays until exit
        Refresh();
        SetTimer(h, 1, 1000, NULL);
        return 0;
    }
    case WM_SIZE: {
        if (wp == SIZE_MINIMIZED) { // minimize to tray
            ShowWindow(h, SW_HIDE);
            return 0;
        }
        RECT rs;
        SendMessageW(hStatus, WM_SIZE, 0, 0);
        LayoutStatus();
        GetWindowRect(hStatus, &rs);
        int W = LOWORD(lp), H = HIWORD(lp), m = S(8), top = m * 2 + ctlH;
        MoveWindow(hSearch, m, m, S(260), ctlH, TRUE);
        MoveWindow(hKill, W - m - btnW, m, btnW, ctlH, TRUE);
        MoveWindow(hList, 0, top, W, H - top - (rs.bottom - rs.top), TRUE);
        return 0;
    }
    case WM_TRAY:
        if (lp == WM_LBUTTONUP) Restore();
        else if (lp == WM_RBUTTONUP) TrayMenu();
        return 0;
    case WM_TIMER:
        if (!IsIconic(h)) Refresh(); // no refresh while minimized: zero cost
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_SEARCH && HIWORD(wp) == EN_CHANGE) {
            GetWindowTextW(hSearch, query, 128);
            ApplyView();
        } else if (LOWORD(wp) == ID_KILL) KillSelected();
        return 0;
    case WM_CONTEXTMENU: // the ListView forwards right-clicks here; wp is the header for header right-clicks
        if ((HWND)wp != hList) break; // title bar right-click still gets the system menu
        ListMenu(lp);
        return 0;
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        if (n->hwndFrom != hList) break;
        if (n->code == NM_CUSTOMDRAW) { // highlight whole rows with high usage
            NMLVCUSTOMDRAW *cd = (NMLVCUSTOMDRAW *)lp;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT && cd->nmcd.dwItemSpec < (DWORD_PTR)nView) {
                static const COLORREF bg[] = { 0, RGB(255, 241, 184), RGB(255, 205, 205) }; // yellow: warning, red: critical
                int lv = Level(&shown[view[cd->nmcd.dwItemSpec]]);
                if (lv) {
                    cd->clrTextBk = bg[lv];
                    return CDRF_NEWFONT;
                }
            }
            return CDRF_DODEFAULT;
        }
        if (n->code == LVN_GETDISPINFOW) {
            LVITEMW *it = &((NMLVDISPINFOW *)lp)->item;
            if (!(it->mask & LVIF_TEXT) || it->iItem >= nView) return 0;
            CellText(&shown[view[it->iItem]], it->iSubItem, it->pszText, it->cchTextMax);
        } else if (n->code == LVN_COLUMNCLICK) {
            int col = ((NMLISTVIEW *)lp)->iSubItem;
            sortDesc = col == sortCol ? !sortDesc : col >= 2; // numeric columns default to descending
            sortCol = col;
            SetSortArrows();
            ApplyView();
        } else if (n->code == LVN_KEYDOWN && ((NMLVKEYDOWN *)lp)->wVKey == VK_DELETE) {
            KillSelected();
        }
        return 0;
    }
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cmd, int show) {
    // Single instance per session, however it's launched (double-click, shortcut, run as admin, renamed copy):
    // a second launch wakes the running window and exits. CreateMutex fails with ACCESS_DENIED when the
    // running copy is elevated and this one isn't, which also means "already running".
    wmShowMe = RegisterWindowMessageW(L"OBTaskmgr.ShowMe");
    HANDLE mtx = CreateMutexW(NULL, FALSE, L"Local\\OBTaskmgr.SingleInstance"); // held until the process exits
    if (!mtx || GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND w = FindWindowW(L"OBTaskmgr", NULL); // also finds the window while it's hidden in the tray
        if (w) {
            DWORD pid;
            GetWindowThreadProcessId(w, &pid);
            AllowSetForegroundWindow(pid); // we were just launched by the user, so we may hand over foreground rights
            PostMessageW(w, wmShowMe, 0, 0);
        }
        return 0;
    }
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);
    HDC dc = GetDC(NULL);
    dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(NULL, dc);
    wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSEXW wc = { sizeof wc, 0, WndProc, 0, 0, hi, LoadIconW(hi, MAKEINTRESOURCEW(1)), LoadCursorW(NULL, IDC_ARROW),
                       (HBRUSH)(COLOR_WINDOW + 1), NULL, L"OBTaskmgr",
                       LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0) };
    RegisterClassExW(&wc);
    hWnd = CreateWindowW(L"OBTaskmgr", TXT_TITLE, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                         CW_USEDEFAULT, CW_USEDEFAULT, S(640), S(680), NULL, NULL, hi, NULL);
    ShowWindow(hWnd, show);
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
