// unvbn-export.exe: the export window, the Windows counterpart of the macOS app's export panel
// (App/ExportView.swift). Drop or choose folders (or launch it from Explorer's "Export with unVBN..." on
// folders); every sequence found is listed and ticked; Export converts the ticked ones with the same
// settings, one after the other. A second launch hands its folders to the window already open.
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

#include "export/exporter.h"
#include "resource.h"
#include "vbn_decode.h"

using Microsoft::WRL::ComPtr;
using namespace vbnx;

namespace {

constexpr UINT WM_APP_SCANNED = WM_APP + 1;  // lParam: Scan*, owned by the receiver
constexpr UINT WM_APP_EXPORT = WM_APP + 2;   // progress waiting in App::pending
constexpr UINT WM_APP_DONE = WM_APP + 3;     // lParam: BatchResult*, owned by the receiver
constexpr wchar_t kClass[] = L"unVBN.ExportWindow";
constexpr ULONG_PTR kCopyDataPaths = 0x564E4250;  // 'VNBP': NUL-separated UTF-16 paths
const double kFrameRates[] = {23.976, 24, 25, 29.97, 30, 50, 59.94, 60};
const Format kFormats[] = {Format::png, Format::mp4, Format::webm};

std::wstring wide(const std::string& s) { return fromUtf8(s).wstring(); }
std::string narrow(const std::wstring& s) { return utf8(fs::path(s)); }

struct Row {
    Sequence sequence;
    std::string size;  // "1920×1080 DXT5", from the first frame's header
    size_t missing = 0;
};

struct Scan {
    std::vector<Row> rows;
    std::vector<std::string> problems;
    bool append = false;
};

std::string sizeOf(const fs::path& frame) {
    std::ifstream in(frame, std::ios::binary);
    uint8_t bytes[vbn::kHeaderSize];
    vbn::Header h;
    std::string error;
    if (!in.read(reinterpret_cast<char*>(bytes), sizeof bytes) || !vbn::parseHeader(bytes, sizeof bytes, h, error)) return "?";
    return std::to_string(h.width) + "\xC3\x97" + std::to_string(h.height) + (h.dxt5 ? " DXT5" : " RGBA");
}

struct App {
    HINSTANCE instance = nullptr;
    HWND hwnd = nullptr;
    HWND list = nullptr;
    std::vector<Row> rows;
    bool populating = false;  // the list is being refilled: ignore its change notifications

    // Folder scanning (off the UI thread: folders may be on a network share).
    std::thread scanner;
    bool scanning = false;
    std::vector<std::pair<std::vector<fs::path>, bool>> queuedLoads;

    // Export.
    std::thread worker;
    std::atomic<bool> cancel{false};
    bool running = false;
    size_t jobCount = 0;
    std::optional<size_t> batchIndex;
    std::mutex lock;  // guards `pending`
    struct {
        std::optional<Progress> overall;
        std::optional<size_t> started;
        bool posted = false;
    } pending;
    std::vector<fs::path> lastOutputs;

    COLORREF background = RGB(0, 0, 0);
    COLORREF customColors[16]{};
    std::string error;
    ComPtr<ITaskbarList3> taskbar;
} app;

HWND item(int id) { return GetDlgItem(app.hwnd, id); }
void setText(int id, const std::string& s) { SetWindowTextW(item(id), wide(s).c_str()); }
void show(int id, bool visible) { ShowWindow(item(id), visible ? SW_SHOW : SW_HIDE); }

Format selectedFormat() {
    const LRESULT i = SendMessageW(item(IDC_FORMAT), CB_GETCURSEL, 0, 0);
    return kFormats[i >= 0 && i < 3 ? i : 2];
}

double selectedFps() {
    const LRESULT i = SendMessageW(item(IDC_FPS), CB_GETCURSEL, 0, 0);
    return kFrameRates[i >= 0 && i < LRESULT(std::size(kFrameRates)) ? i : 5];
}

std::vector<size_t> checkedRows() {
    std::vector<size_t> out;
    for (size_t i = 0; i < app.rows.size(); ++i)
        if (ListView_GetCheckState(app.list, int(i))) out.push_back(i);
    return out;
}

/// Converts dialog units to pixels for this window (and its current DPI).
int dlu(int x) {
    RECT r{0, 0, x, 0};
    MapDialogRect(app.hwnd, &r);
    return r.right;
}

// MARK: state -> controls

void updateControls() {
    const bool idle = !app.running;
    const Format f = selectedFormat();
    for (int id : {IDC_CHOOSE, IDC_LIST, IDC_FORMAT, IDC_FORMAT_LABEL})
        EnableWindow(item(id), idle);
    EnableWindow(item(IDC_FPS), idle && f != Format::png);
    EnableWindow(item(IDC_FPS_LABEL), idle && f != Format::png);
    for (int id : {IDC_BG_LABEL, IDC_BG_SWATCH, IDC_BG_CHOOSE}) EnableWindow(item(id), idle && f == Format::mp4);
    InvalidateRect(item(IDC_BG_SWATCH), nullptr, TRUE);

    const auto checked = checkedRows();
    setText(IDOK, app.running ? (app.cancel ? "Cancelling\xE2\x80\xA6" : "Cancel")
                              : (checked.size() > 1 ? "Export All\xE2\x80\xA6" : "Export\xE2\x80\xA6"));
    EnableWindow(item(IDOK), app.running ? !app.cancel.load() : !checked.empty() && !app.scanning);

    show(IDC_PROGRESS, app.running);
    show(IDC_PERCENT, app.running);
    show(IDC_ERROR, !app.running);
    show(IDC_REVEAL, !app.running && !app.lastOutputs.empty());
    setText(IDC_ERROR, app.error);

    size_t frames = 0, missing = 0;
    for (size_t i : checked) {
        frames += app.rows[i].sequence.frames.size();
        missing += app.rows[i].missing;
    }
    std::string summary;
    if (app.rows.empty()) {
        summary = "";
    } else if (checked.empty()) {
        summary = "Tick the sequences to export.";
    } else {
        if (checked.size() > 1) summary = std::to_string(checked.size()) + " sequences, ";
        summary += std::to_string(frames) + " frames";
        if (missing) summary += " (" + std::to_string(missing) + " missing, skipped)";
        if (checked.size() > 1) summary += ", exported one after the other";
    }
    setText(IDC_FRAMES, summary);
    setText(IDC_DROPHINT, app.rows.empty() ? "Drop .vbn sequence folders or a frame here" : "Drop other sequences here");
}

void fillList() {
    app.populating = true;
    ListView_DeleteAllItems(app.list);
    for (size_t i = 0; i < app.rows.size(); ++i) {
        const Row& r = app.rows[i];
        std::wstring name = wide(r.sequence.name), frames = std::to_wstring(r.sequence.frames.size()),
                     size = wide(r.size), folder = r.sequence.directory.wstring();
        if (r.missing) frames += L" (" + std::to_wstring(r.missing) + L" missing)";
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = int(i);
        it.pszText = name.data();
        ListView_InsertItem(app.list, &it);
        ListView_SetItemText(app.list, int(i), 1, frames.data());
        ListView_SetItemText(app.list, int(i), 2, size.data());
        ListView_SetItemText(app.list, int(i), 3, folder.data());
        ListView_SetCheckState(app.list, int(i), TRUE);
    }
    app.populating = false;
    updateControls();
    // Enter now exports (the focused button would otherwise win over the default one).
    if (!app.rows.empty()) SendMessageW(app.hwnd, WM_NEXTDLGCTL, WPARAM(item(IDOK)), TRUE);
}

// MARK: loading

void startScan(std::vector<fs::path> paths, bool append) {
    if (app.running) return;
    if (app.scanning) {
        app.queuedLoads.push_back({std::move(paths), append});
        return;
    }
    app.scanning = true;
    setText(IDC_STATUS, "Looking for sequences\xE2\x80\xA6");
    updateControls();
    HWND hwnd = app.hwnd;
    app.scanner = std::thread([hwnd, paths = std::move(paths), append] {
        auto scan = std::make_unique<Scan>();
        scan->append = append;
        for (const auto& p : paths) {
            try {
                for (auto& seq : findSequences(p)) {
                    Row row{std::move(seq), {}, 0};
                    row.size = sizeOf(row.sequence.frames[0].path);
                    row.missing = row.sequence.missingNumbers().size();
                    scan->rows.push_back(std::move(row));
                }
            } catch (const std::exception& e) {
                scan->problems.push_back(e.what());
            }
        }
        PostMessageW(hwnd, WM_APP_SCANNED, 0, reinterpret_cast<LPARAM>(scan.release()));
    });
}

void onScanned(std::unique_ptr<Scan> scan) {
    if (app.scanner.joinable()) app.scanner.join();
    app.scanning = false;
    setText(IDC_STATUS, "");
    app.lastOutputs.clear();
    std::string problems;
    for (const auto& p : scan->problems) problems += (problems.empty() ? "" : "\n") + p;
    if (!scan->append) app.error.clear();
    if (!problems.empty()) app.error = app.error.empty() ? problems : app.error + "\n" + problems;
    if (!scan->rows.empty()) {
        if (!scan->append) app.rows.clear();
        for (auto& row : scan->rows) {
            const bool known = std::any_of(app.rows.begin(), app.rows.end(), [&](const Row& r) {
                return r.sequence.directory == row.sequence.directory && r.sequence.name == row.sequence.name;
            });
            if (!known) app.rows.push_back(std::move(row));
        }
        fillList();
    } else {
        updateControls();
    }
    if (!app.queuedLoads.empty()) {
        auto next = std::move(app.queuedLoads.front());
        app.queuedLoads.erase(app.queuedLoads.begin());
        startScan(std::move(next.first), next.second);
    }
}

// MARK: dialogs

/// Shows a dialog; false when the user cancelled. Any other failure is reported in the window.
bool showDialog(IFileDialog* dialog) {
    const HRESULT hr = dialog->Show(app.hwnd);
    if (SUCCEEDED(hr)) return true;
    if (hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        char text[64];
        std::snprintf(text, sizeof text, "The file dialog failed (0x%08lX).", static_cast<unsigned long>(hr));
        app.error = text;
        updateControls();
    }
    return false;
}

std::optional<fs::path> resultPath(IFileDialog* dialog) {
    ComPtr<IShellItem> result;
    PWSTR path = nullptr;
    if (FAILED(dialog->GetResult(&result)) || FAILED(result->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
    fs::path p(path);
    CoTaskMemFree(path);
    return p;
}

void setFolder(IFileDialog* dialog, const fs::path& folder) {
    ComPtr<IShellItem> item;
    if (SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&item)))) dialog->SetFolder(item.Get());
}

std::optional<fs::path> pickFolder(const fs::path& start, const std::wstring& title, const std::wstring& okLabel) {
    ComPtr<IFileOpenDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return std::nullopt;
    FILEOPENDIALOGOPTIONS o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(title.c_str());
    d->SetOkButtonLabel(okLabel.c_str());
    if (!start.empty()) setFolder(d.Get(), start);
    if (!showDialog(d.Get())) return std::nullopt;
    return resultPath(d.Get());
}

void chooseFolders() {
    ComPtr<IFileOpenDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    FILEOPENDIALOGOPTIONS o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM);
    d->SetTitle(L"Choose .vbn sequence folders");
    if (!showDialog(d.Get())) return;
    ComPtr<IShellItemArray> items;
    DWORD count = 0;
    if (FAILED(d->GetResults(&items)) || FAILED(items->GetCount(&count))) return;
    std::vector<fs::path> paths;
    for (DWORD i = 0; i < count; ++i) {
        ComPtr<IShellItem> it;
        PWSTR path = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            paths.emplace_back(path);
            CoTaskMemFree(path);
        }
    }
    if (!paths.empty()) startScan(std::move(paths), false);
}

/// Where one sequence goes: a file for video, a parent folder (holding NAME_png) for PNG.
std::optional<fs::path> askDestination(const Sequence& seq, Format f) {
    const fs::path suggested = defaultOutput(f, seq);
    const char* ext = formatExtension(f);
    if (!ext) {
        auto folder = pickFolder(suggested.parent_path(),
                                 L"The PNG frames go into a new folder “" + suggested.filename().wstring() + L"”",
                                 L"Export Here");
        if (!folder) return std::nullopt;
        return *folder / suggested.filename();
    }
    ComPtr<IFileSaveDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return std::nullopt;
    const std::wstring wext = wide(ext), pattern = L"*." + wext, label = wide(formatName(f));
    const COMDLG_FILTERSPEC filter{label.c_str(), pattern.c_str()};
    d->SetFileTypes(1, &filter);
    d->SetDefaultExtension(wext.c_str());
    d->SetFileName(suggested.filename().c_str());
    setFolder(d.Get(), suggested.parent_path());
    FILEOPENDIALOGOPTIONS o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM);
    if (!showDialog(d.Get())) return std::nullopt;
    return resultPath(d.Get());
}

/// One folder for the whole batch. Outputs that already exist are replaced only after asking.
std::optional<std::vector<fs::path>> askBatchDestination(const std::vector<Sequence>& seqs, Format f) {
    const std::wstring n = std::to_wstring(seqs.size());
    const std::wstring title = f == Format::png ? L"Each of the " + n + L" sequences goes into its own NAME_png folder"
                                                : L"Each of the " + n + L" sequences becomes its own ." + wide(formatExtension(f)) + L" file";
    auto folder = pickFolder(seqs.front().directory.parent_path(), title, L"Export All Here");
    if (!folder) return std::nullopt;
    auto outputs = batchOutputs(f, seqs, *folder);
    std::vector<fs::path> existing;
    for (const auto& o : outputs) {
        std::error_code ec;
        if (fs::exists(o, ec)) existing.push_back(o);
    }
    if (!existing.empty()) {
        const std::wstring message = existing.size() == 1
                                         ? L"“" + existing[0].filename().wstring() + L"” already exists."
                                         : std::to_wstring(existing.size()) + L" of the exports already exist in this folder.";
        const TASKDIALOG_BUTTON buttons[] = {{IDOK, L"Replace"}, {IDCANCEL, L"Cancel"}};
        TASKDIALOGCONFIG c{sizeof c};
        c.hwndParent = app.hwnd;
        c.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
        c.pszWindowTitle = L"unVBN Export";
        c.pszMainIcon = TD_WARNING_ICON;
        c.pszMainInstruction = message.c_str();
        c.pszContent = L"Replacing them overwrites their contents.";
        c.cButtons = 2;
        c.pButtons = buttons;
        c.nDefaultButton = IDCANCEL;
        int pressed = IDCANCEL;
        if (FAILED(TaskDialogIndirect(&c, &pressed, nullptr, nullptr)) || pressed != IDOK) return std::nullopt;
    }
    return outputs;
}

// MARK: export

std::string progressLabel(const Progress& p) {
    std::string s;
    if (app.batchIndex) s = "Sequence " + std::to_string(*app.batchIndex + 1) + " / " + std::to_string(app.jobCount) + " \xC2\xB7 ";
    s += "Rendering frame " + std::to_string(p.done) + " / " + std::to_string(p.total);
    if (auto fps = p.framesPerSecond()) {
        char buf[32];
        std::snprintf(buf, sizeof buf, " \xC2\xB7 %.0f fps", *fps);
        s += buf;
    }
    if (auto left = p.remaining()) s += " \xC2\xB7 " + (*left < 1 ? std::string("almost done") : Progress::formatDuration(*left) + " left");
    return s;
}

void showProgress(const Progress& p) {
    SendMessageW(item(IDC_PROGRESS), PBM_SETPOS, WPARAM(p.fraction() * 1000), 0);
    setText(IDC_STATUS, progressLabel(p));
    setText(IDC_PERCENT, std::to_string(p.percent()) + " %");
    if (app.taskbar) app.taskbar->SetProgressValue(app.hwnd, ULONGLONG(p.done), ULONGLONG(std::max(p.total, 1)));
}

void startExport() {
    const auto checked = checkedRows();
    if (checked.empty() || app.running || app.scanning) return;
    const Format f = selectedFormat();
    std::vector<Sequence> seqs;
    for (size_t i : checked) seqs.push_back(app.rows[i].sequence);

    std::vector<Job> jobs;
    if (seqs.size() == 1) {
        auto out = askDestination(seqs[0], f);
        if (!out) return;
        jobs.push_back({seqs[0], *out});
    } else {
        auto outs = askBatchDestination(seqs, f);
        if (!outs) return;
        for (size_t i = 0; i < seqs.size(); ++i) jobs.push_back({seqs[i], (*outs)[i]});
    }
    Options options;
    options.format = f;
    options.fps = selectedFps();
    options.background = {GetRValue(app.background), GetGValue(app.background), GetBValue(app.background)};

    app.running = true;
    app.cancel = false;
    app.jobCount = jobs.size();
    app.batchIndex.reset();
    app.lastOutputs.clear();
    app.error.clear();
    int total = 0;
    for (const auto& j : jobs) total += int(j.sequence.frames.size());
    {
        std::lock_guard<std::mutex> g(app.lock);
        app.pending = {};
    }
    SendMessageW(item(IDC_PROGRESS), PBM_SETRANGE32, 0, 1000);
    showProgress(Progress{0, total, 0});
    if (app.taskbar) app.taskbar->SetProgressState(app.hwnd, TBPF_NORMAL);
    updateControls();

    HWND hwnd = app.hwnd;
    const bool batch = jobs.size() > 1;
    app.worker = std::thread([hwnd, jobs = std::move(jobs), options, batch] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const auto start = std::chrono::steady_clock::now();
        auto post = [hwnd] {
            if (!app.pending.posted) {
                app.pending.posted = true;
                PostMessageW(hwnd, WM_APP_EXPORT, 0, 0);
            }
        };
        auto result = std::make_unique<BatchResult>(runBatch(jobs, options, [&](const BatchEvent& e) {
            std::lock_guard<std::mutex> g(app.lock);
            if (e.type == BatchEvent::Type::started && batch) {
                app.pending.started = e.index;
                post();
            } else if (e.type == BatchEvent::Type::progress) {
                app.pending.overall = e.overall;
                post();
            }
        }, [] { return app.cancel.load(); }));
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        // Failures are reported by job index; the UI thread maps them to names.
        PostMessageW(hwnd, WM_APP_DONE, WPARAM(seconds * 1000), reinterpret_cast<LPARAM>(result.release()));
        CoUninitialize();
    });
}

void onExportDone(std::unique_ptr<BatchResult> result, double seconds) {
    if (app.worker.joinable()) app.worker.join();
    app.running = false;
    app.batchIndex.reset();
    app.lastOutputs = result->exported;
    // The job list was built from the ticked rows, in order.
    const auto checked = checkedRows();
    std::string errors;
    for (const auto& [index, message] : result->failures) {
        const std::string name = index < checked.size() ? app.rows[checked[index]].sequence.name : "";
        errors += (errors.empty() ? "" : "\n") + (app.jobCount > 1 ? name + ": " + message : message);
    }
    app.error = errors;
    char time[32];
    std::snprintf(time, sizeof time, "%.1f s", seconds);
    const size_t exported = result->exported.size(), count = app.jobCount;
    std::string status;
    if (count == 1) {
        if (exported) status = "Exported " + utf8(result->exported[0].filename()) + " in " + time + ".";
    } else if (result->cancelled) {
        status = "Cancelled after " + std::to_string(exported) + " of " + std::to_string(count) + " sequences.";
    } else {
        status = exported == count ? "Exported " + std::to_string(count) + " sequences in " + time + "."
                                   : "Exported " + std::to_string(exported) + " of " + std::to_string(count) + " sequences in " + time + ".";
    }
    setText(IDC_STATUS, status);
    if (app.taskbar) app.taskbar->SetProgressState(app.hwnd, TBPF_NOPROGRESS);
    updateControls();
    if (!result->cancelled && exported && GetForegroundWindow() != app.hwnd) {
        FLASHWINFO fi{sizeof fi, app.hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
        FlashWindowEx(&fi);
    }
}

/// Opens Explorer on each folder holding outputs, with the outputs selected.
void revealOutputs() {
    std::map<fs::path, std::vector<fs::path>> byFolder;
    for (const auto& p : app.lastOutputs) byFolder[p.parent_path()].push_back(p);
    for (const auto& [folder, files] : byFolder) {
        PIDLIST_ABSOLUTE folderId = nullptr;
        if (FAILED(SHParseDisplayName(folder.c_str(), nullptr, &folderId, 0, nullptr))) continue;
        std::vector<PIDLIST_ABSOLUTE> ids;
        for (const auto& f : files) {
            PIDLIST_ABSOLUTE id = nullptr;
            if (SUCCEEDED(SHParseDisplayName(f.c_str(), nullptr, &id, 0, nullptr))) ids.push_back(id);
        }
        // Absolute item IDs are accepted here as well as child IDs.
        const std::vector<LPCITEMIDLIST> items(ids.begin(), ids.end());
        SHOpenFolderAndSelectItems(folderId, UINT(items.size()), (PCUITEMID_CHILD_ARRAY)items.data(), 0);
        for (auto id : ids) CoTaskMemFree(id);
        CoTaskMemFree(folderId);
    }
}

void chooseBackground() {
    CHOOSECOLORW c{sizeof c};
    c.hwndOwner = app.hwnd;
    c.rgbResult = app.background;
    c.lpCustColors = app.customColors;
    c.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (ChooseColorW(&c)) {
        app.background = c.rgbResult;
        InvalidateRect(item(IDC_BG_SWATCH), nullptr, TRUE);
    }
}

void drawSwatch(const DRAWITEMSTRUCT& d) {
    const bool enabled = !(d.itemState & ODS_DISABLED);
    FillRect(d.hDC, &d.rcItem, GetSysColorBrush(COLOR_BTNFACE));
    RECT r = d.rcItem;
    InflateRect(&r, -2, -2);
    HBRUSH fill = CreateSolidBrush(enabled ? app.background : GetSysColor(COLOR_BTNFACE));
    FillRect(d.hDC, &r, fill);
    DeleteObject(fill);
    FrameRect(d.hDC, &r, GetSysColorBrush(enabled ? COLOR_WINDOWTEXT : COLOR_GRAYTEXT));
    if (d.itemState & ODS_FOCUS) {
        RECT f = d.rcItem;
        DrawFocusRect(d.hDC, &f);
    }
}

std::vector<fs::path> droppedPaths(HDROP drop) {
    std::vector<fs::path> paths;
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        std::wstring p(DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');
        p.resize(DragQueryFileW(drop, i, p.data(), UINT(p.size())));
        paths.emplace_back(p);
    }
    DragFinish(drop);
    return paths;
}

void initDialog(HWND hwnd) {
    app.hwnd = hwnd;
    app.list = item(IDC_LIST);
    HICON icon = LoadIconW(app.instance, MAKEINTRESOURCEW(IDI_APP));
    SendMessageW(hwnd, WM_SETICON, ICON_BIG, LPARAM(icon));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL,
                 LPARAM(LoadImageW(app.instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0)));

    ListView_SetExtendedListViewStyle(app.list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const struct { const wchar_t* title; int width; int format; } columns[] = {
        {L"Sequence", 92, LVCFMT_LEFT}, {L"Frames", 52, LVCFMT_RIGHT}, {L"Size", 58, LVCFMT_LEFT}, {L"Folder", 92, LVCFMT_LEFT}};
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.pszText = const_cast<wchar_t*>(columns[i].title);
        c.cx = dlu(columns[i].width);
        c.fmt = columns[i].format;
        ListView_InsertColumn(app.list, i, &c);
    }
    for (Format f : kFormats) SendMessageW(item(IDC_FORMAT), CB_ADDSTRING, 0, LPARAM(wide(formatName(f)).c_str()));
    SendMessageW(item(IDC_FORMAT), CB_SETCURSEL, 2, 0);  // WebM, as on the Mac
    for (double fps : kFrameRates) SendMessageW(item(IDC_FPS), CB_ADDSTRING, 0, LPARAM(wide(fpsString(fps) + " fps").c_str()));
    SendMessageW(item(IDC_FPS), CB_SETCURSEL, 5, 0);  // 50 fps

    if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&app.taskbar))) &&
        FAILED(app.taskbar->HrInit()))
        app.taskbar.Reset();
    updateControls();
}

INT_PTR CALLBACK dialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_INITDIALOG:
        initDialog(hwnd);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDOK:
            if (app.running) {
                app.cancel = true;
                updateControls();
            } else {
                startExport();
            }
            return TRUE;
        case IDCANCEL:  // Esc: cancels a running export, never closes the window
            if (app.running) {
                app.cancel = true;
                updateControls();
            }
            return TRUE;
        case IDC_CHOOSE: chooseFolders(); return TRUE;
        case IDC_BG_SWATCH:
        case IDC_BG_CHOOSE: chooseBackground(); return TRUE;
        case IDC_REVEAL: revealOutputs(); return TRUE;
        case IDC_FORMAT:
            if (HIWORD(wParam) == CBN_SELCHANGE) updateControls();
            return TRUE;
        }
        break;
    case WM_NOTIFY: {
        const auto* n = reinterpret_cast<const NMHDR*>(lParam);
        if (n->idFrom == IDC_LIST && n->code == LVN_ITEMCHANGED && !app.populating) {
            const auto* lv = reinterpret_cast<const NMLISTVIEW*>(lParam);
            if ((lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK)) updateControls();
        }
        break;
    }
    case WM_DRAWITEM:
        if (wParam == IDC_BG_SWATCH) {
            drawSwatch(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        }
        break;
    case WM_CTLCOLORSTATIC:
        if (reinterpret_cast<HWND>(lParam) == item(IDC_ERROR)) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, RGB(196, 43, 28));
            SetBkMode(dc, TRANSPARENT);
            return INT_PTR(GetSysColorBrush(COLOR_BTNFACE));
        }
        break;
    case WM_DROPFILES:
        if (!app.running) startScan(droppedPaths(reinterpret_cast<HDROP>(wParam)), false);
        else DragFinish(reinterpret_cast<HDROP>(wParam));
        return TRUE;
    case WM_COPYDATA: {
        const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        if (cds->dwData != kCopyDataPaths) break;
        std::vector<fs::path> paths;
        const auto* text = static_cast<const wchar_t*>(cds->lpData);
        const size_t length = cds->cbData / sizeof(wchar_t);
        for (size_t i = 0; i < length;) {
            const size_t end = std::find(text + i, text + length, L'\0') - text;
            if (end > i) paths.emplace_back(std::wstring(text + i, end - i));
            i = end + 1;
        }
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        // Folders sent by other launches (Explorer starts one per selected folder) join the list.
        if (!paths.empty() && !app.running) startScan(std::move(paths), true);
        SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, TRUE);
        return TRUE;
    }
    case WM_APP_SCANNED:
        onScanned(std::unique_ptr<Scan>(reinterpret_cast<Scan*>(lParam)));
        return TRUE;
    case WM_APP_EXPORT: {
        std::optional<Progress> overall;
        std::optional<size_t> started;
        {
            std::lock_guard<std::mutex> g(app.lock);
            overall = app.pending.overall;
            started = app.pending.started;
            app.pending.posted = false;
        }
        if (!app.running) return TRUE;
        if (started) app.batchIndex = started;
        if (overall) showProgress(*overall);
        return TRUE;
    }
    case WM_APP_DONE:
        onExportDone(std::unique_ptr<BatchResult>(reinterpret_cast<BatchResult*>(lParam)), double(wParam) / 1000);
        return TRUE;
    case WM_CLOSE:
        if (app.running) {
            app.cancel = true;
            if (app.worker.joinable()) app.worker.join();
        }
        if (app.scanner.joinable()) app.scanner.join();
        DestroyWindow(hwnd);
        return TRUE;
    case WM_DESTROY:
        PostQuitMessage(0);
        return TRUE;
    }
    return FALSE;
}

std::vector<fs::path> commandLinePaths() {
    std::vector<fs::path> paths;
    int argc = 0;
    if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; ++i)
            if (*argv[i]) paths.emplace_back(argv[i]);  // an empty argument would mean the current folder
        LocalFree(argv);
    }
    return paths;
}

/// Hands the paths to an export window that is already open; false if there is none.
bool forwardToRunningInstance(const std::vector<fs::path>& paths, bool wait) {
    for (int attempt = 0; attempt < (wait ? 100 : 1); ++attempt) {
        if (HWND existing = FindWindowW(kClass, nullptr)) {
            std::wstring data;
            for (const auto& p : paths) data += fs::absolute(p).wstring() + L'\0';
            COPYDATASTRUCT cds{kCopyDataPaths, DWORD(data.size() * sizeof(wchar_t)), data.data()};
            DWORD pid = 0;
            GetWindowThreadProcessId(existing, &pid);
            AllowSetForegroundWindow(pid);
            DWORD_PTR result = 0;
            SendMessageTimeoutW(existing, WM_COPYDATA, 0, LPARAM(&cds), SMTO_ABORTIFHUNG, 5000, &result);
            return true;
        }
        Sleep(50);
    }
    return false;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    const auto paths = commandLinePaths();
    // Explorer starts one process per selected folder: the first opens the window, the others join it.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\unVBN.Export");
    const bool another = mutex && GetLastError() == ERROR_ALREADY_EXISTS;
    if (another && forwardToRunningInstance(paths, true)) return 0;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    app.instance = instance;

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = DefDlgProcW;
    wc.cbWndExtra = DLGWINDOWEXTRA;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    HWND hwnd = CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_MAIN), nullptr, dialogProc, 0);
    if (!hwnd) return 1;
    // Lets drops from Explorer reach an elevated window too (harmless otherwise).
    ChangeWindowMessageFilterEx(hwnd, WM_DROPFILES, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(hwnd, 0x0049 /* WM_COPYGLOBALDATA */, MSGFLT_ALLOW, nullptr);
    ShowWindow(hwnd, showCommand);
    if (!paths.empty()) startScan(paths, false);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return 0;
}
