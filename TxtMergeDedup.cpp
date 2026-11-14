// TxtMergeDedup.cpp — 文本合并去重工具 (Win32 GUI)
// 多个 txt 合并为一个文件并按行去重,哈希分片外部去重,内存占用恒定,支持 10GB+ 数据。
// 方案: 无序输出,阶段1哈希分片 → 阶段2分片内去重 → 顺序拼接。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_set>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' publicKeyToken='6595b64144ccf1df' language='*' processorArchitecture='*'\"")

namespace fs = std::filesystem;

// ================= 常量 =================
static const size_t   READ_BUF_SIZE   = 8u << 20;   // 8MB 读取缓冲
static const size_t   OUT_BUF_SIZE    = 8u << 20;   // 8MB 输出缓冲
static const size_t   SHARD_BUF_SIZE  = 256u << 10; // 每分片 256KB 写缓冲
static const uint64_t SHARD_TARGET    = 64ull << 20;// 每分片目标 64MB
static const uint32_t MAX_SHARDS      = 256;
static const uint64_t PROGRESS_STEP   = 32ull << 20;

// 控件 ID
enum {
    IDC_DIR = 101, IDC_BROWSE, IDC_OUT, IDC_RECURSE, IDC_SKIPEMPTY,
    IDC_TRIM, IDC_START, IDC_CANCEL, IDC_PROGRESS, IDC_STATUS, IDC_LOG
};
static const UINT WM_APP_PROGRESS = WM_APP + 1; // wParam = 千分比 0..1000
static const UINT WM_APP_STATUS   = WM_APP + 2; // lParam = wchar_t* (堆分配)
static const UINT WM_APP_LOG      = WM_APP + 3; // lParam = wchar_t* (堆分配)
static const UINT WM_APP_DONE     = WM_APP + 4; // wParam = Stats* (堆分配)

static std::atomic<bool> g_cancel{ false };

// ================= 哈希 =================
// FNV-1a 64 + Murmur3 finalizer,质量足够做去重指纹
static inline uint64_t HashBytes(const char* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) { h ^= (unsigned char)p[i]; h *= 1099511628211ULL; }
    h ^= h >> 33; h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return h;
}

// ================= 带缓冲写入器 =================
struct BufWriter {
    FILE* f = nullptr;
    std::vector<char> buf;
    size_t used = 0;
    bool ok = true;
    void init(FILE* fp, size_t cap) { f = fp; buf.resize(cap); used = 0; ok = (fp != nullptr); }
    void write(const char* p, size_t n) {
        if (!ok) return;
        if (n >= buf.size()) {
            flush();
            if (fwrite(p, 1, n, f) != n) ok = false;
            return;
        }
        if (used + n > buf.size()) flush();
        memcpy(buf.data() + used, p, n);
        used += n;
    }
    void flush() {
        if (used && f) { if (fwrite(buf.data(), 1, used, f) != used) ok = false; used = 0; }
    }
    void close() { flush(); if (f) { fclose(f); f = nullptr; } }
};

// ================= 流式逐行读取 =================
// fn(const char* p, size_t len) — 行内容不含换行,\r 已剥离,自动跳过 UTF-8 BOM
// onChunk(size_t bytes)         — 每次 fread 后回调(用于进度)
template <class Fn, class Prog>
static bool ForEachLine(FILE* f, Fn&& fn, Prog&& onChunk) {
    std::vector<char> buf(READ_BUF_SIZE);
    std::string carry;
    bool first = true;
    size_t r;
    while ((r = fread(buf.data(), 1, buf.size(), f)) > 0) {
        onChunk(r);
        const char* p = buf.data();
        size_t n = r;
        if (first) {
            if (n >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) { p += 3; n -= 3; }
            first = false;
        }
        size_t start = 0;
        for (size_t i = 0; i < n; ++i) {
            if (p[i] == '\n') {
                carry.append(p + start, i - start);
                size_t len = carry.size();
                while (len && carry[len - 1] == '\r') --len;
                fn(carry.data(), len);
                carry.clear();
                start = i + 1;
            }
        }
        carry.append(p + start, n - start);
        if (ferror(f)) return false;
    }
    if (ferror(f)) return false;
    if (!carry.empty()) {
        size_t len = carry.size();
        while (len && carry[len - 1] == '\r') --len;
        fn(carry.data(), len);
    }
    return true;
}

// ================= 参数与结果 =================
struct Params {
    HWND hwnd;
    std::wstring srcDir;
    std::wstring outFile;
    bool recurse, skipEmpty, trimWS;
};
struct Stats {
    bool success = false;
    bool cancelled = false;
    uint64_t totalLines = 0, uniqueLines = 0;
    double seconds = 0;
    std::wstring error;
};

// ================= 工具 =================
static std::wstring LowerCopy(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}
static std::wstring DirOf(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L".";
    return path.substr(0, pos);
}
static std::wstring FmtU64(uint64_t v) {
    wchar_t buf[32]; wchar_t out[40]; int oi = 0;
    swprintf(buf, 32, L"%I64u", v);
    int n = (int)wcslen(buf);
    for (int i = 0; i < n; ++i) {
        out[oi++] = buf[i];
        int rem = n - 1 - i;
        if (rem > 0 && rem % 3 == 0) out[oi++] = L',';
    }
    out[oi] = 0;
    return out;
}

static void PostLog(HWND h, const std::wstring& s) {
    auto* p = new std::wstring(s);
    PostMessageW(h, WM_APP_LOG, 0, (LPARAM)p);
}
static void PostStatus(HWND h, const std::wstring& s) {
    auto* p = new std::wstring(s);
    PostMessageW(h, WM_APP_STATUS, 0, (LPARAM)p);
}

// ================= 核心处理(工作线程) =================
static bool EnumerateFiles(const std::wstring& dir, bool recurse,
                           const std::wstring& outFile, const std::wstring& tmpDir,
                           std::vector<std::wstring>& files, uint64_t& totalBytes,
                           std::wstring& err) {
    std::wstring outLower = LowerCopy(outFile);
    std::wstring tmpLower = LowerCopy(tmpDir);
    try {
        auto addFile = [&](const fs::path& p) {
            std::error_code ec;
            if (!fs::is_regular_file(p, ec) || ec) return;
            std::wstring ext = p.extension().wstring();
            if (_wcsicmp(ext.c_str(), L".txt") != 0) return;
            std::wstring low = LowerCopy(p.wstring());
            if (low == outLower) return;                    // 排除输出文件自身
            if (low.compare(0, tmpLower.size(), tmpLower) == 0) return; // 排除临时目录
            files.push_back(p.wstring());
            totalBytes += (uint64_t)fs::file_size(p, ec);
        };
        std::error_code skipErr;
        if (recurse) {
            for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied);
                 it != fs::recursive_directory_iterator(); it.increment(skipErr)) {
                try { if (it->is_directory(skipErr) || skipErr) { skipErr.clear(); continue; } } catch (...) { continue; }
                addFile(it->path());
            }
        } else {
            for (auto it = fs::directory_iterator(dir, fs::directory_options::skip_permission_denied);
                 it != fs::directory_iterator(); it.increment(skipErr)) {
                addFile(it->path());
            }
        }
    } catch (const std::exception& e) {
        wchar_t w[512];
        size_t n = 0;
        mbstowcs_s(&n, w, e.what(), _TRUNCATE);
        err = std::wstring(L"枚举文件失败: ") + w;
        return false;
    }
    return true;
}

// 输出回调集合(GUI 线程/控制台模式各自实现)
struct Sink {
    std::function<void(const std::wstring&)> log;
    std::function<void(const std::wstring&)> status;
    std::function<void(int)> progress; // 0..1000
};

static void RunCore(const Params* P, Stats* st, const Sink& sink) {
    auto t0 = std::chrono::steady_clock::now();

    // ---- 枚举文件 ----
    std::wstring tmpDir = DirOf(P->outFile) + L"\\__dedup_tmp__";
    std::vector<std::wstring> files;
    uint64_t totalBytes = 0;
    sink.status(L"正在扫描源目录...");
    if (!EnumerateFiles(P->srcDir, P->recurse, P->outFile, tmpDir, files, totalBytes, st->error)) {
        return;
    }
    if (files.empty()) {
        st->error = L"源目录下未找到任何 .txt 文件";
        return;
    }
    sink.log(L"找到 " + FmtU64(files.size()) + L" 个 txt 文件, 总计 " + FmtU64(totalBytes) + L" 字节");

    // ---- 创建临时目录与分片 ----
    if (!CreateDirectoryW(tmpDir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        st->error = L"无法创建临时目录: " + tmpDir;
        return;
    }
    uint32_t shardCount = (uint32_t)((totalBytes + SHARD_TARGET - 1) / SHARD_TARGET);
    if (shardCount < 1) shardCount = 1;
    if (shardCount > MAX_SHARDS) shardCount = MAX_SHARDS;

    std::vector<BufWriter> shards(shardCount);
    std::vector<std::wstring> shardPaths(shardCount);
    for (uint32_t i = 0; i < shardCount; ++i) {
        wchar_t name[64];
        swprintf(name, 64, L"\\s%03u.tmp", i);
        shardPaths[i] = tmpDir + name;
        FILE* f = _wfopen(shardPaths[i].c_str(), L"wb");
        shards[i].init(f, SHARD_BUF_SIZE);
    }

    // ---- 阶段 1: 流式读取全部文件 → 哈希分片 ----
    uint64_t p1bytes = 0, totalLines = 0;
    uint64_t denom = totalBytes * 2; // 阶段1+阶段2 各占一半
    auto postP = [&](uint64_t doneBytes) {
        uint64_t d = doneBytes;
        if (d > denom) d = denom;
        if (denom) sink.progress((int)(d * 1000 / denom));
    };
    bool ioError = false;
    int fileIdx = 0;
    for (const auto& fp : files) {
        if (g_cancel) break;
        ++fileIdx;
        std::wstring name = fs::path(fp).filename().wstring();
        sink.status(L"阶段1/2 分片中 [" + std::to_wstring(fileIdx) + L"/" + std::to_wstring(files.size()) + L"] " + name);
        FILE* f = _wfopen(fp.c_str(), L"rb");
        if (!f) { sink.log(L"[警告] 无法打开: " + name); continue; }
        uint64_t lastPost = p1bytes;
        bool okFile = ForEachLine(f,
            [&](const char* p, size_t len) {
                if (P->trimWS) { while (len && (p[len - 1] == ' ' || p[len - 1] == '\t')) --len; }
                if (P->skipEmpty && len == 0) return;
                uint64_t hsh = HashBytes(p, len);
                uint32_t s = (uint32_t)(hsh % shardCount);
                shards[s].write(p, len);
                shards[s].write("\n", 1);
                ++totalLines;
            },
            [&](size_t r) {
                p1bytes += r;
                if (p1bytes - lastPost >= PROGRESS_STEP) { lastPost = p1bytes; postP(p1bytes); }
            });
        fclose(f);
        if (!okFile) { sink.log(L"[警告] 读取错误(文件可能被占用): " + name); }
        for (uint32_t i = 0; i < shardCount; ++i)
            if (!shards[i].ok) { ioError = true; break; }
        if (ioError) break;
        postP(p1bytes);
    }

    for (uint32_t i = 0; i < shardCount; ++i) shards[i].close();

    if (g_cancel) { st->cancelled = true; }
    else if (ioError) { st->error = L"写入临时分片失败, 请检查磁盘空间"; }

    // ---- 阶段 2: 逐分片去重 → 输出 ----
    uint64_t uniqueLines = 0;
    if (!st->cancelled && st->error.empty()) {
        FILE* of = _wfopen(P->outFile.c_str(), L"wb");
        if (!of) {
            st->error = L"无法创建输出文件: " + P->outFile;
        } else {
            BufWriter out; out.init(of, OUT_BUF_SIZE);
            uint64_t p2bytes = 0;
            for (uint32_t i = 0; i < shardCount && !g_cancel && out.ok; ++i) {
                if (i % 16 == 0)
                    sink.status(L"阶段2/2 去重中 [" + std::to_wstring(i + 1) + L"/" + std::to_wstring(shardCount) + L"]");
                FILE* sf = _wfopen(shardPaths[i].c_str(), L"rb");
                if (!sf) { st->error = L"无法读取临时分片 " + std::to_wstring(i); break; }
                std::unordered_set<uint64_t> seen;
                seen.reserve(1u << 20);
                uint64_t lastPost2 = p2bytes;
                ForEachLine(sf,
                    [&](const char* p, size_t len) {
                        uint64_t hsh = HashBytes(p, len);
                        if (seen.insert(hsh).second) {   // 分片内首次出现才写出
                            out.write(p, len);
                            out.write("\n", 1);
                            ++uniqueLines;
                        }
                    },
                    [&](size_t r) {
                        p2bytes += r;
                        if (p2bytes - lastPost2 >= PROGRESS_STEP) { lastPost2 = p2bytes; postP(p1bytes + p2bytes); }
                    });
                fclose(sf);
                DeleteFileW(shardPaths[i].c_str());
            }
            if (!out.ok) st->error = L"写入输出文件失败, 请检查磁盘空间";
            out.close();
            if (g_cancel && st->error.empty()) st->cancelled = true;
            postP(denom);
        }
    }

    // 清理临时目录
    for (uint32_t i = 0; i < shardCount; ++i) DeleteFileW(shardPaths[i].c_str());
    RemoveDirectoryW(tmpDir.c_str());

    auto t1 = std::chrono::steady_clock::now();
    st->seconds = std::chrono::duration<double>(t1 - t0).count();
    st->totalLines = totalLines;
    st->uniqueLines = uniqueLines;
}

static DWORD WINAPI WorkerThread(LPVOID lp) {
    std::unique_ptr<Params> P((Params*)lp);
    HWND h = P->hwnd;
    Sink sink;
    sink.log    = [h](const std::wstring& s) { PostLog(h, s); };
    sink.status = [h](const std::wstring& s) { PostStatus(h, s); };
    sink.progress = [h](int pct) { PostMessageW(h, WM_APP_PROGRESS, (WPARAM)pct, 0); };
    Stats* st = new Stats();
    RunCore(P.get(), st, sink);
    PostMessageW(h, WM_APP_DONE, 0, (LPARAM)st);
    return 0;
}

// ================= GUI =================
static HWND g_hwnd, hDir, hBrowse, hOut, hRecurse, hSkipEmpty, hTrim, hStart, hCancel, hProg, hStatus, hLog;

static void AppendLog(const wchar_t* s) {
    int len = GetWindowTextLengthW(hLog);
    SendMessageW(hLog, EM_SETSEL, len, len);
    SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)s);
}

static std::wstring GetEditText(HWND c) {
    int len = GetWindowTextLengthW(c);
    std::wstring s(len + 1, L'\0');
    len = GetWindowTextW(c, &s[0], len + 1);
    s.resize(len);
    return s;
}

static std::wstring BrowseFolder(HWND parent) {
    std::wstring result;
    IFileDialog* fd = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fd)))) {
        DWORD opts = 0;
        fd->GetOptions(&opts);
        fd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (SUCCEEDED(fd->Show(parent))) {
            IShellItem* si = nullptr;
            if (SUCCEEDED(fd->GetResult(&si))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(si->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                    result = p;
                    CoTaskMemFree(p);
                }
                si->Release();
            }
        }
        fd->Release();
    }
    return result;
}

static void SetRunningUI(bool running) {
    EnableWindow(hDir, !running);   EnableWindow(hBrowse, !running);
    EnableWindow(hOut, !running);
    EnableWindow(hRecurse, !running); EnableWindow(hSkipEmpty, !running); EnableWindow(hTrim, !running);
    EnableWindow(hStart, !running); EnableWindow(hCancel, running);
}

static void StartJob() {
    std::wstring dir = GetEditText(hDir);
    std::wstring out = GetEditText(hOut);
    // 去除两侧引号/空白
    auto trim = [](std::wstring& s) {
        size_t a = s.find_first_not_of(L" \"\t");
        size_t b = s.find_last_not_of(L" \"\t");
        s = (a == std::wstring::npos) ? L"" : s.substr(a, b - a + 1);
    };
    trim(dir); trim(out);

    DWORD attr = GetFileAttributesW(dir.c_str());
    if (dir.empty() || attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        MessageBoxW(g_hwnd, L"源目录不存在, 请检查路径。", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    if (out.empty()) {
        MessageBoxW(g_hwnd, L"请填写输出文件路径。", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    attr = GetFileAttributesW(out.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        MessageBoxW(g_hwnd, L"输出路径是一个目录, 请指定一个 .txt 文件名。", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }
    std::wstring outDir = DirOf(out);
    attr = GetFileAttributesW(outDir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        MessageBoxW(g_hwnd, (L"输出文件所在目录不存在: " + outDir).c_str(), L"提示", MB_OK | MB_ICONWARNING);
        return;
    }

    auto* P = new Params();
    P->hwnd = g_hwnd;
    P->srcDir = dir;
    P->outFile = out;
    P->recurse  = SendMessageW(hRecurse,  BM_GETCHECK, 0, 0) == BST_CHECKED;
    P->skipEmpty = SendMessageW(hSkipEmpty, BM_GETCHECK, 0, 0) == BST_CHECKED;
    P->trimWS   = SendMessageW(hTrim,    BM_GETCHECK, 0, 0) == BST_CHECKED;

    g_cancel = false;
    SetRunningUI(true);
    SendMessageW(hProg, PBM_SETPOS, 0, 0);
    AppendLog(L"\r\n----------------------------------------\r\n");
    AppendLog((L"[开始] " + dir + L"  ->  " + out + L"\r\n").c_str());
    CloseHandle(CreateThread(nullptr, 0, WorkerThread, P, 0, nullptr));
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
            HWND c = CreateWindowW(cls, text, WS_CHILD | WS_VISIBLE | style,
                                   x, y, w, h, hwnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
            SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
            return c;
        };
        mk(L"STATIC", L"源目录:", 0, 15, 18, 60, 20, 0);
        hDir    = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 80, 15, 420, 24, IDC_DIR);
        hBrowse = mk(L"BUTTON", L"浏览...", BS_PUSHBUTTON, 510, 15, 90, 26, IDC_BROWSE);
        mk(L"STATIC", L"输出文件:", 0, 15, 53, 60, 20, 0);
        hOut    = mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 80, 50, 420, 24, IDC_OUT);
        hRecurse  = mk(L"BUTTON", L"包含子目录",   BS_AUTOCHECKBOX, 80, 88, 130, 22, IDC_RECURSE);
        hSkipEmpty = mk(L"BUTTON", L"丢弃空行",   BS_AUTOCHECKBOX, 220, 88, 130, 22, IDC_SKIPEMPTY);
        hTrim     = mk(L"BUTTON", L"去除行尾空白", BS_AUTOCHECKBOX, 360, 88, 140, 22, IDC_TRIM);
        SendMessageW(hRecurse, BM_SETCHECK, BST_CHECKED, 0);
        hStart  = mk(L"BUTTON", L"开始合并去重", BS_PUSHBUTTON, 80, 118, 140, 30, IDC_START);
        hCancel = mk(L"BUTTON", L"取消", BS_PUSHBUTTON, 230, 118, 120, 30, IDC_CANCEL);
        EnableWindow(hCancel, FALSE);
        hProg   = mk(PROGRESS_CLASSW, L"", 0, 15, 160, 585, 22, IDC_PROGRESS);
        SendMessageW(hProg, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));
        hStatus = mk(L"STATIC", L"将 txt 文件或文件夹拖入本窗口即可填入路径", 0, 15, 190, 585, 20, IDC_STATUS);
        hLog    = mk(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 15, 215, 585, 200, IDC_LOG);
        DragAcceptFiles(hwnd, TRUE);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BROWSE: {
            std::wstring d = BrowseFolder(hwnd);
            if (!d.empty()) {
                SetWindowTextW(hDir, d.c_str());
                SetWindowTextW(hOut, (d + L"\\merged_dedup.txt").c_str());
            }
            return 0;
        }
        case IDC_START: StartJob(); return 0;
        case IDC_CANCEL:
            g_cancel = true;
            EnableWindow(hCancel, FALSE);
            AppendLog(L"[取消中] 等待当前文件处理完...\r\n");
            return 0;
        }
        return 0;
    case WM_DROPFILES: {
        wchar_t path[MAX_PATH * 2] = { 0 };
        if (DragQueryFileW((HDROP)wp, 0, path, MAX_PATH * 2)) {
            DWORD attr = GetFileAttributesW(path);
            if (attr & FILE_ATTRIBUTE_DIRECTORY) {
                SetWindowTextW(hDir, path);
                SetWindowTextW(hOut, (std::wstring(path) + L"\\merged_dedup.txt").c_str());
            } else {
                // 拖入的是文件 → 用其所在目录
                std::wstring d = DirOf(path);
                SetWindowTextW(hDir, d.c_str());
                SetWindowTextW(hOut, (d + L"\\merged_dedup.txt").c_str());
            }
        }
        DragFinish((HDROP)wp);
        return 0;
    }
    case WM_APP_PROGRESS:
        SendMessageW(hProg, PBM_SETPOS, (int)wp, 0);
        return 0;
    case WM_APP_STATUS: {
        auto* s = (std::wstring*)lp;
        SetWindowTextW(hStatus, s->c_str());
        delete s;
        return 0;
    }
    case WM_APP_LOG: {
        auto* s = (std::wstring*)lp;
        AppendLog((s->c_str()));
        delete s;
        return 0;
    }
    case WM_APP_DONE: {
        auto* s = (Stats*)lp;  // 注意: Stats 通过 lParam 传递(与 PostMessage 对应)
        SetRunningUI(false);
        if (s->cancelled) {
            AppendLog(L"[已取消]\r\n");
            SetWindowTextW(hStatus, L"已取消 (临时文件已清理)");
        } else if (!s->error.empty()) {
            AppendLog((L"[错误] " + s->error + L"\r\n").c_str());
            SetWindowTextW(hStatus, L"处理失败");
            MessageBoxW(g_hwnd, s->error.c_str(), L"错误", MB_OK | MB_ICONERROR);
        } else {
            uint64_t removed = s->totalLines > s->uniqueLines ? s->totalLines - s->uniqueLines : 0;
            wchar_t buf[256];
            swprintf(buf, 256, L"[完成] 共 %I64u 行, 去重后 %I64u 行, 移除重复 %I64u 行, 耗时 %.1f 秒\r\n",
                     s->totalLines, s->uniqueLines, removed, s->seconds);
            AppendLog(buf);
            SetWindowTextW(hStatus, L"完成");
            SendMessageW(hProg, PBM_SETPOS, 1000, 0);
        }
        delete s;
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// 控制台输出(UTF-8 字节写入, 重定向/管道均正常)
static void PrintUtf8(const std::wstring& s) {
    if (s.empty()) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string u((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &u[0], n, nullptr, nullptr);
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), u.data(), (DWORD)u.size(), &written, nullptr);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // ---- 命令行模式: TxtMergeDedup.exe <源目录> <输出文件> [/skipempty] [/trim] [/norecurse] ----
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 3) {
        AttachConsole(ATTACH_PARENT_PROCESS);
        Params P{};
        P.srcDir = argv[1];
        P.outFile = argv[2];
        P.recurse = true;
        for (int i = 3; i < argc; ++i) {
            if (!_wcsicmp(argv[i], L"/skipempty") || !_wcsicmp(argv[i], L"-skipempty")) P.skipEmpty = true;
            else if (!_wcsicmp(argv[i], L"/trim") || !_wcsicmp(argv[i], L"-trim")) P.trimWS = true;
            else if (!_wcsicmp(argv[i], L"/norecurse") || !_wcsicmp(argv[i], L"-norecurse")) P.recurse = false;
        }
        Stats st;
        Sink sink;
        sink.log    = [](const std::wstring& s) { PrintUtf8(s + L"\n"); };
        sink.status = [](const std::wstring& s) { PrintUtf8(L"\r" + s); };
        sink.progress = [](int pct) { PrintUtf8(L"\rProgress: " + std::to_wstring(pct / 10) + L"." + std::to_wstring(pct % 10) + L"%      "); };
        RunCore(&P, &st, sink);
        PrintUtf8(L"\n");
        if (!st.error.empty()) { PrintUtf8(L"[ERROR] " + st.error + L"\n"); LocalFree(argv); return 1; }
        if (st.cancelled)      { PrintUtf8(L"[CANCELLED]\n"); LocalFree(argv); return 1; }
        uint64_t removed = st.totalLines > st.uniqueLines ? st.totalLines - st.uniqueLines : 0;
        wchar_t buf[256];
        swprintf(buf, 256, L"[DONE] total=%I64u unique=%I64u removed=%I64u time=%.1fs\n",
                 st.totalLines, st.uniqueLines, removed, st.seconds);
        PrintUtf8(buf);
        LocalFree(argv);
        return 0;
    }

    // ---- GUI 模式 ----
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"TxtMergeDedupWnd";
    RegisterClassW(&wc);

    RECT rc = { 0, 0, 615, 430 };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX), FALSE);
    g_hwnd = CreateWindowW(wc.lpszClassName, L"文本合并去重工具",
                           WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX),
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           rc.right - rc.left, rc.bottom - rc.top,
                           nullptr, nullptr, hInst, nullptr);
    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

#ifdef SELFTEST
    // 自检: 模拟真实流程向 GUI 线程投递 WM_APP_* 消息(Stats/字符串走 lParam, 与工作线程一致)
    // 旧版本此处 (Stats*)wp 取到 0 造成空指针崩溃
    auto* stT = new Stats();
    stT->success = true; stT->totalLines = 100; stT->uniqueLines = 80;
    PostMessageW(g_hwnd, WM_APP_LOG, 0, (LPARAM)(new std::wstring(L"[SELFTEST] log ok\n")));
    PostMessageW(g_hwnd, WM_APP_STATUS, 0, (LPARAM)(new std::wstring(L"[SELFTEST] status ok")));
    PostMessageW(g_hwnd, WM_APP_PROGRESS, (WPARAM)500, 0);
    PostMessageW(g_hwnd, WM_APP_DONE, 0, (LPARAM)stT);
    SetTimer(g_hwnd, 1, 1000, nullptr); // 1 秒后退出(让消息都处理完)
    MSG msgT;
    while (GetMessageW(&msgT, nullptr, 0, 0)) {
        if (msgT.message == WM_TIMER && msgT.hwnd == g_hwnd) { KillTimer(g_hwnd, 1); break; }
        TranslateMessage(&msgT);
        DispatchMessageW(&msgT);
    }
    DestroyWindow(g_hwnd);
    CoUninitialize();
    return 0;
#endif

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return (int)msg.wParam;
}
