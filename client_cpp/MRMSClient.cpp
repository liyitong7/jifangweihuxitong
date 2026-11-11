// =====================================================================
//  MRMSClient.cpp —— 机房维护系统 客户端（单文件 / 无界面 / 零外部依赖）
// =====================================================================
//  目标系统 : Windows 7 RTM(无SP) ~ Windows 11（x86 / x64）
//  功能     :
//    1) 每次启动把自己写入开机启动项（计划任务 + 注册表 Run，自愈式）
//    2) 向服务端注册并轮询（IP 由服务端从连接中识别）
//    3) 远程执行 cmd 命令并回传输出（输出自动转 UTF-8）
//    4) 屏幕采集（GDI+ JPEG）与鼠标键盘注入（SendInput）
//    5) 远程重启 / 关机
//  编译（MinGW-w64，推荐，产出无任何运行库依赖）:
//    g++ MRMSClient.cpp -O2 -std=c++17 -municode -mwindows -static \
//        -static-libgcc -static-libstdc++ -o MRMSClient.exe \
//        -lgdiplus -lwinhttp -lshell32 -lole32 -ladvapi32
//  编译（MSVC，务必用 /MT 静态链接避免运行库依赖，/utf-8 保证中文正常）:
//    cl /EHsc /O2 /MT /utf-8 /DUNICODE /D_UNICODE MRMSClient.cpp ^
//       gdiplus.lib winhttp.lib shell32.lib ole32.lib advapi32.lib
//  用法     :
//    MRMSClient.exe            正常运行（默认，后台常驻）
//    MRMSClient.exe /install   仅安装自启动后退出（部署用）
//    MRMSClient.exe /uninstall 移除自启动与安装文件
// =====================================================================

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_NONSTDC_NO_WARNINGS
#include <windows.h>
#include <winhttp.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>   // CommandLineToArgvW
#include <wincrypt.h>   // CryptGenRandom
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cctype>
#include <wchar.h>

#pragma comment(lib, "gdiplus.lib")   // MSVC 用；MinGW 忽略
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

// JPEG 编码质量参数 GUID（EncoderQuality，直接写死避免头文件差异）
static const CLSID kEncoderQualityGuid =
    {0x1d5be4b5, 0xfa4a, 0x452d, {0x9c, 0xdd, 0x5d, 0xb3, 0x51, 0x05, 0xe7, 0xeb}};

#define MRMS_TASK_NAME   L"MRMS_Client"
#define MRMS_WATCH_NAME  L"MRMS_Client_Watch"
#define MRMS_RUN_VAL     L"MRMS_Client"
#define MRMS_MUTEX       L"Global\\MRMS_Client_SingleInstance"
#define MRMS_VER         "1.0"

// =====================================================================
//  全局配置
// =====================================================================
static std::wstring g_server   = L"http://10.100.4.111:3309"; // 服务端地址
static std::string  g_token    = "MRCS-2026-DEFAULT-TOKEN";    // 接入令牌
static std::wstring g_host;          // 主机名
static std::wstring g_user;          // 当前用户
static std::string  g_os;            // 系统版本描述
static std::string  g_arch = "x86";
static std::string  g_priv = "0";    // "1"=管理员
static std::wstring g_cid;           // 客户端唯一 ID（主机名+随机后缀）
static int  g_sw = 0, g_sh = 0;      // 虚拟屏幕尺寸
static double g_interval = 5.0;      // 当前轮询间隔（秒）
static bool g_log_on = true;
static std::wstring g_log_path;
static std::wstring g_exe_path;      // 当前 exe 完整路径
static std::wstring g_install_dir;   // C:\ProgramData\MRMS
static std::wstring g_install_path;  // 安装副本完整路径

// =====================================================================
//  基础工具：字符串 / base64 / url 编码 / 日志
// =====================================================================
typedef std::wstring wstr;

static std::string w2u8(const wstr &s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0, NULL, NULL);
    std::string r((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &r[0], n, NULL, NULL);
    return r;
}
static wstr u82w(const std::string &s) {
    if (s.empty()) return wstr();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), NULL, 0);
    wstr r((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &r[0], n);
    return r;
}

static const char *B64T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string b64enc(const std::string &in) {
    std::string out;
    size_t i = 0;
    while (i + 2 < in.size()) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i+1] << 8) | (unsigned char)in[i+2];
        out += B64T[(v >> 18) & 63]; out += B64T[(v >> 12) & 63];
        out += B64T[(v >> 6) & 63];  out += B64T[v & 63];
        i += 3;
    }
    if (i + 1 == in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        out += B64T[(v >> 18) & 63]; out += B64T[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        unsigned v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i+1] << 8);
        out += B64T[(v >> 18) & 63]; out += B64T[(v >> 12) & 63];
        out += B64T[(v >> 6) & 63];  out += '=';
    }
    return out;
}
static int b64v(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static std::string b64dec(const std::string &in) {
    std::string out;
    int buf = 0, bits = 0;
    for (char c : in) {
        int v = b64v(c);
        if (v < 0) continue;               // 忽略 \r \n = 等
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((buf >> bits) & 0xFF);
        }
    }
    return out;
}

static std::string urlencode(const std::string &s) {
    static const char *HEX = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%'; out += HEX[c >> 4]; out += HEX[c & 15];
        }
    }
    return out;
}

static void split(const std::string &s, char sep, std::vector<std::string> &out) {
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
}

static void logf(const char *fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    OutputDebugStringA(buf);
    if (!g_log_on || g_log_path.empty()) return;
    FILE *f = _wfopen(g_log_path.c_str(), L"ab");
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%04u-%02u-%02u %02u:%02u:%02u] %s\r\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, buf);
    fclose(f);
    // 简单轮转：超过 512KB 清空
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(g_log_path.c_str(), GetFileExInfoStandard, &fa) &&
        fa.nFileSizeHigh == 0 && fa.nFileSizeLow > 512 * 1024) {
        f = _wfopen(g_log_path.c_str(), L"wb");
        if (f) fclose(f);
    }
}

// =====================================================================
//  系统信息
// =====================================================================
typedef LONG (WINAPI *RtlGetVersionPtr)(void *);
static void detect_os() {
    struct { ULONG dwOSVersionInfoSize; ULONG dwMajorVersion; ULONG dwMinorVersion;
             ULONG dwBuildNumber; ULONG dwPlatformId; WCHAR szCSDVersion[128]; } vi;
    memset(&vi, 0, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    RtlGetVersionPtr p = (RtlGetVersionPtr)(void *)
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (!p || p(&vi) != 0) { g_os = "Windows(未知版本)"; return; }
    char sp[64] = {0};
    if (vi.szCSDVersion[0]) {
        WideCharToMultiByte(CP_UTF8, 0, vi.szCSDVersion, -1, sp, sizeof(sp), NULL, NULL);
    }
    char buf[160];
    const char *name = "Windows";
    if (vi.dwMajorVersion == 10) name = vi.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10";
    else if (vi.dwMajorVersion == 6 && vi.dwMinorVersion == 3) name = "Windows 8.1";
    else if (vi.dwMajorVersion == 6 && vi.dwMinorVersion == 2) name = "Windows 8";
    else if (vi.dwMajorVersion == 6 && vi.dwMinorVersion == 1) name = "Windows 7";
    else if (vi.dwMajorVersion == 6 && vi.dwMinorVersion == 0) name = "Windows Vista";
    _snprintf(buf, sizeof(buf), "%s %s(build %lu)%s", name,
              *sp ? sp : "", vi.dwBuildNumber,
              (vi.dwMajorVersion == 6 && vi.dwMinorVersion == 1 && !*sp) ? " [无SP]" : "");
    g_os = buf;
}

static bool is_elevated() {
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    DWORD elev = 0, ret = 0;
    BOOL ok = GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &ret);
    CloseHandle(tok);
    return ok && elev != 0;
}

// =====================================================================
//  配置读取（exe 同目录 client_config.json，其次安装目录，最后默认值）
// =====================================================================
static bool json_field(const std::string &s, const char *key, std::string &out) {
    std::string pat = "\"";
    pat += key;
    pat += "\"";
    size_t p = s.find(pat);
    if (p == std::string::npos) return false;
    p = s.find(':', p + pat.size());
    if (p == std::string::npos) return false;
    p++;
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) p++;
    if (p >= s.size()) return false;
    if (s[p] == '"') {
        size_t q = s.find('"', p + 1);
        if (q == std::string::npos) return false;
        out = s.substr(p + 1, q - p - 1);
        return true;
    }
    size_t q = p;
    while (q < s.size() && s[q] != ',' && s[q] != '}' && s[q] != '\n') q++;
    out = s.substr(p, q - p);
    return true;
}

static std::string read_file_u8(const wstr &path) {
    FILE *f = _wfopen(path.c_str(), L"rb");
    if (!f) return std::string();
    std::string data;
    char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    return data;
}

static void load_config() {
    // exe 所在目录
    wstr dir;
    {
        wchar_t p[MAX_PATH * 2] = {0};
        GetModuleFileNameW(NULL, p, MAX_PATH * 2);
        g_exe_path = p;
        size_t k = g_exe_path.find_last_of(L"\\/");
        dir = (k == std::string::npos) ? L"." : g_exe_path.substr(0, k);
    }
    // 安装目录 %ProgramData%\MRMS
    wchar_t pd[MAX_PATH] = {0};
    GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
    g_install_dir = (pd[0] ? wstr(pd) : wstr(L"C:\\ProgramData")) + L"\\MRMS";
    g_install_path = g_install_dir + L"\\MRMSClient.exe";
    CreateDirectoryW(g_install_dir.c_str(), NULL);

    std::string cfg = read_file_u8(dir + L"\\client_config.json");
    if (cfg.empty()) cfg = read_file_u8(g_install_dir + L"\\client_config.json");
    std::string srv, tok, lg;
    if (!cfg.empty()) {
        if (json_field(cfg, "server", srv) && !srv.empty()) g_server = u82w(srv);
        if (json_field(cfg, "token", tok) && !tok.empty()) g_token = tok;
        if (json_field(cfg, "log", lg)) g_log_on = (lg != "false" && lg != "0");
    }
    g_log_path = g_install_dir + L"\\mrms_client.log";
}

// =====================================================================
//  HTTP（WinHTTP，纯 HTTP）
// =====================================================================
static wstr g_http_host;
static INTERNET_PORT g_http_port = 80;

static void parse_server() {
    wstr s = g_server;
    if (s.rfind(L"http://", 0) == 0) s = s.substr(7);
    else if (s.rfind(L"https://", 0) == 0) s = s.substr(8);
    size_t k = s.find(L'/');
    if (k != std::string::npos) s = s.substr(0, k);
    size_t c = s.find(L':');
    if (c != std::string::npos) {
        g_http_port = (INTERNET_PORT)_wtoi(s.substr(c + 1).c_str());
        s = s.substr(0, c);
    }
    if (s.empty()) s = L"127.0.0.1";
    g_http_host = s;
    if (!g_http_port) g_http_port = 80;
}

// 发送 POST，成功返回 true 并填充 status / body
static bool http_post(const wstr &path, const char *contentType,
                      const std::string &body, const wstr &extraHeaders,
                      int &status, std::string &resp) {
    status = 0; resp.clear();
    HINTERNET ses = WinHttpOpen(L"MRMSClient/" MRMS_VER,
                                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    WinHttpSetTimeouts(ses, 5000, 5000, 10000, 15000);
    HINTERNET con = WinHttpConnect(ses, g_http_host.c_str(), g_http_port, 0);
    bool ok = false;
    if (con) {
        HINTERNET req = WinHttpOpenRequest(con, L"POST", path.c_str(), NULL,
                                           WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (req) {
            wstr hdrs = L"Content-Type: " + u82w(contentType) + L"\r\n" + extraHeaders;
            if (WinHttpSendRequest(req, hdrs.c_str(), (DWORD)-1,
                                   (LPVOID)(body.empty() ? "" : body.data()),
                                   (DWORD)body.size(), (DWORD)body.size(), 0) &&
                WinHttpReceiveResponse(req, NULL)) {
                DWORD code = 0, sz = sizeof(code);
                if (WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                        WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX))
                    status = (int)code;
                DWORD n = 0;
                char buf[16384];
                while (WinHttpReadData(req, buf, sizeof(buf), &n) && n > 0)
                    resp.append(buf, n);
                ok = true;
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(con);
    }
    WinHttpCloseHandle(ses);
    return ok;
}

static void form_add(std::string &form, const char *k, const std::string &v) {
    if (!form.empty()) form += "&";
    form += k; form += "="; form += urlencode(v);
}

// =====================================================================
//  自启动安装（每次启动调用，自愈式）
// =====================================================================
static bool reg_set_run(HKEY root, const wstr &val, const wstr &cmdline) {
    HKEY k = NULL;
    LONG r = RegCreateKeyExW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                             0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL);
    if (r != ERROR_SUCCESS || !k) return false;
    r = RegSetValueExW(k, val.c_str(), 0, REG_SZ, (const BYTE *)cmdline.c_str(),
                       (DWORD)((cmdline.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}
static void reg_del_run(HKEY root, const wstr &val) {
    HKEY k = NULL;
    if (RegOpenKeyExW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, val.c_str());
        RegCloseKey(k);
    }
}

static int run_hidden(const wstr &cmdline) {
    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    std::vector<wchar_t> cl(cmdline.begin(), cmdline.end()); cl.push_back(0);
    if (!CreateProcessW(NULL, cl.data(), NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}

static bool task_exists() {
    return run_hidden(L"schtasks /Query /TN " MRMS_TASK_NAME) == 0;
}

static void install_startup() {
    CreateDirectoryW(g_install_dir.c_str(), NULL);

    // 复制自身到安装目录（已在安装目录则跳过）
    wstr run = g_exe_path;
    if (_wcsicmp(g_exe_path.c_str(), g_install_path.c_str()) != 0) {
        if (CopyFileW(g_exe_path.c_str(), g_install_path.c_str(), FALSE))
            run = g_install_path;
    }
    // 顺带复制配置文件（如果 exe 旁边有）
    wstr cfgSrc = g_exe_path.substr(0, g_exe_path.find_last_of(L"\\") + 1) + L"client_config.json";
    wstr cfgDst = g_install_dir + L"\\client_config.json";
    if (_wcsicmp(cfgSrc.c_str(), cfgDst.c_str()) != 0)
        CopyFileW(cfgSrc.c_str(), cfgDst.c_str(), FALSE);

    wstr quoted = L"\"" + run + L"\"";
    bool elev = is_elevated();

    if (elev) {
        // 优先计划任务（登录触发 + 每小时守护，均为最高权限）
        int r1 = run_hidden(L"schtasks /Create /F /TN " MRMS_TASK_NAME L" /TR \""
                            + run + L"\" /SC ONLOGON /RL HIGHEST");
        int r2 = run_hidden(L"schtasks /Create /F /TN " MRMS_WATCH_NAME L" /TR \""
                            + run + L"\" /SC HOURLY /RL HIGHEST");
        if (r1 == 0 && r2 == 0) {
            reg_del_run(HKEY_LOCAL_MACHINE, MRMS_RUN_VAL);  // 避免双实例
            reg_del_run(HKEY_CURRENT_USER, MRMS_RUN_VAL);
        } else {
            // 计划任务失败 → 退回注册表 HKLM Run
            reg_set_run(HKEY_LOCAL_MACHINE, MRMS_RUN_VAL, quoted);
        }
        logf("install(elevated): task=%d watch=%d path=%ls", r1, r2, run.c_str());
    } else {
        // 普通权限：写 HKCU Run
        reg_set_run(HKEY_CURRENT_USER, MRMS_RUN_VAL, quoted);
        logf("install(user): run=%ls", run.c_str());
    }
}

static void uninstall() {
    run_hidden(L"schtasks /Delete /F /TN " MRMS_TASK_NAME);
    run_hidden(L"schtasks /Delete /F /TN " MRMS_WATCH_NAME);
    reg_del_run(HKEY_LOCAL_MACHINE, MRMS_RUN_VAL);
    reg_del_run(HKEY_CURRENT_USER, MRMS_RUN_VAL);
    // 延迟到重启后删除正在使用的文件
    MoveFileExW(g_install_path.c_str(), NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    wstr cfgDst = g_install_dir + L"\\client_config.json";
    MoveFileExW(cfgDst.c_str(), NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    MoveFileExW(g_log_path.c_str(), NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    logf("uninstalled (files removed after reboot)");
    OutputDebugStringA("MRMS: uninstalled");
}

// =====================================================================
//  客户端唯一 ID（防克隆镜像重名：首次运行生成随机后缀）
// =====================================================================
static void make_cid() {
    wchar_t hst[64] = {0}; DWORD n = 63;
    GetComputerNameW(hst, &n);
    std::string clean;
    for (wchar_t c : wstr(hst)) {
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
            (c >= L'0' && c <= L'9') || c == L'-' || c == L'_')
            clean += (char)c;
    }
    if (clean.empty()) clean = "PC";

    wstr idFile = g_install_dir + L"\\client.id";
    std::string id = read_file_u8(idFile);
    bool fresh = false;
    // 去掉可能的换行
    while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' ')) id.pop_back();
    if (id.size() != 8) {
        unsigned rnd = (unsigned)GetTickCount() ^ GetCurrentProcessId();
        HCRYPTPROV hc = 0;
        if (CryptAcquireContextW(&hc, NULL, NULL, PROV_RSA_FULL,
                                 CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
            CryptGenRandom(hc, 4, (BYTE *)&rnd);
            CryptReleaseContext(hc, 0);
        }
        char buf[16];
        _snprintf(buf, sizeof(buf), "%08x", rnd);
        id = buf;
        FILE *f = _wfopen(idFile.c_str(), L"wb");
        if (f) { fwrite(id.data(), 1, id.size(), f); fclose(f); fresh = true; }
    }
    g_host = hst;
    g_cid = u82w(clean + "-" + id);
    if (fresh) logf("new client id generated");
}

// =====================================================================
//  命令执行（cmd /c，捕获输出，OEM→UTF-8）
// =====================================================================
static std::string oem_to_u8(const std::string &s) {
    if (s.empty()) return s;
    UINT cp = GetOEMCP();
    int n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), NULL, 0);
    if (n <= 0) return s;
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), &w[0], n);
    return w2u8(w);
}

static bool run_cmd(const wstr &cmd, int timeoutSec, int &exitCode, std::string &out) {
    exitCode = -1; out.clear();
    SECURITY_ATTRIBUTES sa; sa.nLength = sizeof(sa); sa.lpSecurityDescriptor = NULL; sa.bInheritHandle = TRUE;
    HANDLE ro = NULL, wo = NULL;
    if (!CreatePipe(&ro, &wo, &sa, 0)) return false;
    SetHandleInformation(ro, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            &sa, OPEN_EXISTING, 0, NULL);

    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wo; si.hStdError = wo;
    si.hStdInput = (nul != INVALID_HANDLE_VALUE) ? nul : NULL;

    wstr cl = L"cmd.exe /c " + cmd;
    std::vector<wchar_t> cmdbuf(cl.begin(), cl.end()); cmdbuf.push_back(0);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    BOOL ok = CreateProcessW(NULL, cmdbuf.data(), NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(wo);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) { CloseHandle(ro); return false; }

    DWORD start = GetTickCount();
    char buf[8192]; DWORD rd = 0, avail = 0;
    bool timedOut = false;
    for (;;) {
        while (PeekNamedPipe(ro, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            if (!ReadFile(ro, buf, (avail < sizeof(buf)) ? avail : sizeof(buf), &rd, NULL) || rd == 0) break;
            if (out.size() < 256 * 1024) out.append(buf, rd);
        }
        if (WaitForSingleObject(pi.hProcess, 120) != WAIT_TIMEOUT) break;
        if (timeoutSec > 0 && GetTickCount() - start > (DWORD)timeoutSec * 1000) {
            TerminateProcess(pi.hProcess, (DWORD)-1);
            timedOut = true;
            break;
        }
    }
    // 收尾再读一次
    while (PeekNamedPipe(ro, NULL, 0, NULL, &avail, NULL) && avail > 0) {
        if (!ReadFile(ro, buf, (avail < sizeof(buf)) ? avail : sizeof(buf), &rd, NULL) || rd == 0) break;
        if (out.size() < 256 * 1024) out.append(buf, rd);
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    exitCode = timedOut ? -1 : (int)code;
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(ro);
    out = oem_to_u8(out);
    if (timedOut) {
        out += "\r\n[MRMS] 命令超时，已强制结束进程。";
        return false;
    }
    return true;
}

// =====================================================================
//  屏幕采集（GDI → GDI+ JPEG）
// =====================================================================
static ULONG_PTR g_gdip = 0;

static int get_jpeg_encoder(CLSID *clsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    Gdiplus::ImageCodecInfo *arr = (Gdiplus::ImageCodecInfo *)malloc(size);
    if (!arr) return -1;
    Gdiplus::GetImageEncoders(num, size, arr);
    int found = -1;
    for (UINT i = 0; i < num; i++) {
        if (wcscmp(arr[i].MimeType, L"image/jpeg") == 0) {
            *clsid = arr[i].Clsid; found = (int)i; break;
        }
    }
    free(arr);
    return found;
}

static bool capture_jpeg(int quality, std::string &jpeg) {
    jpeg.clear();
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0) return false;

    HDC sdc = GetDC(NULL);
    if (!sdc) return false;
    HDC mdc = CreateCompatibleDC(sdc);
    HBITMAP hbm = CreateCompatibleBitmap(sdc, vw, vh);
    HGDIOBJ old = SelectObject(mdc, hbm);
    BOOL bl = BitBlt(mdc, 0, 0, vw, vh, sdc, vx, vy, SRCCOPY | CAPTUREBLT);
    SelectObject(mdc, old);
    DeleteDC(mdc);
    ReleaseDC(NULL, sdc);
    if (!bl) { DeleteObject(hbm); return false; }

    bool ok = false;
    Gdiplus::Bitmap bmp(hbm, NULL);
    if (bmp.GetLastStatus() == Gdiplus::Ok) {
        CLSID enc;
        if (get_jpeg_encoder(&enc) >= 0) {
            IStream *st = NULL;
            if (CreateStreamOnHGlobal(NULL, TRUE, &st) == S_OK && st) {
                ULONG q = (ULONG)quality;
                Gdiplus::EncoderParameters ep;
                ep.Count = 1;
                ep.Parameter[0].Guid = kEncoderQualityGuid;
                ep.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
                ep.Parameter[0].NumberOfValues = 1;
                ep.Parameter[0].Value = &q;
                if (bmp.Save(st, &enc, &ep) == Gdiplus::Ok) {
                    STATSTG stat; memset(&stat, 0, sizeof(stat));
                    if (st->Stat(&stat, STATFLAG_NONAME) == S_OK) {
                        HGLOBAL h = NULL;
                        LARGE_INTEGER zero = {0};
                        ULARGE_INTEGER pos = {0};
                        st->Seek(zero, STREAM_SEEK_SET, &pos);
                        if (GetHGlobalFromStream(st, &h) == S_OK && h) {
                            SIZE_T sz = (SIZE_T)stat.cbSize.QuadPart;
                            void *ptr = GlobalLock(h);
                            if (ptr && sz > 0 && sz < 12 * 1024 * 1024) {
                                jpeg.assign((const char *)ptr, sz);
                                ok = true;
                            }
                            GlobalUnlock(h);
                        }
                    }
                }
                st->Release();
            }
        }
    }
    DeleteObject(hbm);
    return ok;
}

// =====================================================================
//  鼠标 / 键盘注入（SendInput）
// =====================================================================
static void mouse_send(int nx, int ny, DWORD btnFlags) {
    INPUT in; ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dx = (nx * 65535 + 5000) / 10000;     // 0..10000 → 0..65535
    in.mi.dy = (ny * 65535 + 5000) / 10000;
    in.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE | btnFlags;
    SendInput(1, &in, sizeof(INPUT));
}
static void mouse_click(DWORD down, DWORD up) {
    INPUT in; ZeroMemory(&in, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = down; SendInput(1, &in, sizeof(INPUT));
    in.mi.dwFlags = up;   SendInput(1, &in, sizeof(INPUT));
}
static void mouse_action(int nx, int ny, const std::string &a) {
    if (a == "move") { mouse_send(nx, ny, 0); }
    else if (a == "left")  { mouse_send(nx, ny, 0); mouse_click(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP); }
    else if (a == "right") { mouse_send(nx, ny, 0); mouse_click(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP); }
    else if (a == "dbl")   { mouse_send(nx, ny, 0); mouse_click(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP);
                             mouse_click(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP); }
    // 按住拖动：按下与抬高分开发送，中间穿插 move 即形成拖拽
    else if (a == "ldown") { mouse_send(nx, ny, MOUSEEVENTF_LEFTDOWN); }
    else if (a == "lup")   { mouse_send(nx, ny, MOUSEEVENTF_LEFTUP); }
    else if (a == "rdown") { mouse_send(nx, ny, MOUSEEVENTF_RIGHTDOWN); }
    else if (a == "rup")   { mouse_send(nx, ny, MOUSEEVENTF_RIGHTUP); }
}

static void key_text(const wstr &text) {
    for (wchar_t c : text) {
        INPUT in; ZeroMemory(&in, sizeof(in));
        in.type = INPUT_KEYBOARD;
        in.ki.wScan = c; in.ki.dwFlags = KEYEVENTF_UNICODE;
        SendInput(1, &in, sizeof(INPUT));
        in.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(1, &in, sizeof(INPUT));
        Sleep(5);
    }
}

static bool vk_of(const std::string &name, DWORD &vk) {
    static const struct { const char *n; DWORD v; } MAP[] = {
        {"enter", 0x0D}, {"return", 0x0D}, {"esc", 0x1B}, {"tab", 0x09},
        {"bksp", 0x08}, {"backspace", 0x08}, {"space", 0x20}, {"del", 0x2E},
        {"delete", 0x2E}, {"ins", 0x2D}, {"insert", 0x2D},
        {"home", 0x24}, {"end", 0x23}, {"pgup", 0x21}, {"pgdn", 0x22},
        {"up", 0x26}, {"down", 0x28}, {"left", 0x25}, {"right", 0x27},
        {"win", 0x5B}, {"lwin", 0x5B}, {"rwin", 0x5C},
        {"ctrl", 0x11}, {"alt", 0x12}, {"shift", 0x10},
        {"f1", 0x70}, {"f2", 0x71}, {"f3", 0x72}, {"f4", 0x73}, {"f5", 0x74},
        {"f6", 0x75}, {"f7", 0x76}, {"f8", 0x77}, {"f9", 0x78}, {"f10", 0x79},
        {"f11", 0x7A}, {"f12", 0x7B},
    };
    std::string s;
    for (char c : name) s += (char)tolower((unsigned char)c);
    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++)
        if (s == MAP[i].n) { vk = MAP[i].v; return true; }
    if (s.size() == 1) {
        char c = s[0];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { vk = (DWORD)toupper((unsigned char)c); return true; }
    }
    return false;
}
static void key_press_vk(DWORD vk) {
    INPUT in; ZeroMemory(&in, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk; in.ki.dwFlags = 0;
    SendInput(1, &in, sizeof(INPUT));
    in.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
}
static void key_special(const std::string &name) {
    std::vector<std::string> parts;
    split(name, '+', parts);
    if (parts.empty()) return;
    std::vector<DWORD> mods;
    DWORD main = 0;
    for (size_t i = 0; i < parts.size(); i++) {
        DWORD vk;
        if (!vk_of(parts[i], vk)) return;
        if (i + 1 == parts.size()) main = vk;
        else mods.push_back(vk);
    }
    if (!main) main = 0x0D;
    INPUT in; ZeroMemory(&in, sizeof(in));
    in.type = INPUT_KEYBOARD;
    for (DWORD m : mods) {
        in.ki.wVk = (WORD)m; in.ki.dwFlags = 0;
        SendInput(1, &in, sizeof(INPUT));
    }
    key_press_vk(main);
    for (size_t i = mods.size(); i > 0; i--) {
        in.ki.wVk = (WORD)mods[i - 1]; in.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &in, sizeof(INPUT));
    }
}

// =====================================================================
//  电源
// =====================================================================
static void do_power(bool shutdown) {
    HANDLE tok = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        TOKEN_PRIVILEGES tp; ZeroMemory(&tp, sizeof(tp));
        tp.PrivilegeCount = 1;
        LookupPrivilegeValueW(NULL, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
        CloseHandle(tok);
    }
    logf("power: %s", shutdown ? "shutdown" : "reboot");
    Sleep(400);   // 给日志落盘留时间
    ExitWindowsEx((shutdown ? EWX_POWEROFF : EWX_REBOOT) | EWX_FORCE, 0);
}

// =====================================================================
//  上报 / 结果回传
// =====================================================================
static void post_result(const std::string &id, int code, const std::string &output) {
    std::string form;
    form_add(form, "cid", w2u8(g_cid));
    form_add(form, "id", id);
    form_add(form, "code", std::to_string(code));
    form_add(form, "output", output);
    form_add(form, "token", g_token);
    int st = 0; std::string resp;
    http_post(L"/result", "application/x-www-form-urlencoded", form, L"", st, resp);
}

static void post_screen(const std::string &jpeg) {
    wstr hdrs = L"X-CID: " + g_cid + L"\r\nX-Token: " + u82w(g_token) + L"\r\n";
    int st = 0; std::string resp;
    http_post(L"/screen", "application/octet-stream", jpeg, hdrs, st, resp);
}

static bool do_poll() {
    std::string form;
    form_add(form, "cid", w2u8(g_cid));
    form_add(form, "host", w2u8(g_host));
    form_add(form, "user", w2u8(g_user));
    form_add(form, "os", g_os);
    form_add(form, "arch", g_arch);
    form_add(form, "priv", g_priv);
    form_add(form, "ver", MRMS_VER);
    form_add(form, "sw", std::to_string(g_sw));
    form_add(form, "sh", std::to_string(g_sh));
    form_add(form, "caps", "screen,exec,input,power");
    form_add(form, "token", g_token);
    int st = 0; std::string resp;
    if (!http_post(L"/poll", "application/x-www-form-urlencoded", form, L"", st, resp))
        return false;
    if (st != 200) { logf("poll http %d", st); return false; }

    std::vector<std::string> lines;
    split(resp, '\n', lines);
    for (std::string ln : lines) {
        while (!ln.empty() && (ln.back() == '\r' || ln.back() == '\n')) ln.pop_back();
        if (ln.empty()) continue;
        std::vector<std::string> f;
        split(ln, '\x1f', f);
        const std::string &t = f[0];

        if (t == "INTERVAL" && f.size() >= 2) {
            double v = atof(f[1].c_str());
            if (v >= 0.15 && v <= 300) g_interval = v;
        } else if (t == "EXEC" && f.size() >= 4) {
            int timeout = atoi(f[2].c_str());
            wstr cmd = u82w(b64dec(f[3]));
            logf("exec: timeout=%d len=%zu", timeout, cmd.size());
            int code = 0; std::string out;
            run_cmd(cmd, timeout > 0 ? timeout : 60, code, out);
            post_result(f[1], code, out);
        } else if (t == "SCREEN" && f.size() >= 2) {
            int quality = atoi(f[1].c_str());
            std::string jpeg;
            if (capture_jpeg(quality > 0 && quality <= 100 ? quality : 55, jpeg) && !jpeg.empty())
                post_screen(jpeg);
        } else if (t == "MOUSE" && f.size() >= 4) {
            mouse_action(atoi(f[1].c_str()), atoi(f[2].c_str()), f[3]);
            // 同批多事件（如按下+抬起）间留一点间隔，确保程序能识别到“按住”
            const std::string &a = f[3];
            if (a == "ldown" || a == "lup" || a == "left" || a == "right" ||
                a == "dbl" || a == "rdown" || a == "rup")
                Sleep(15);
        } else if (t == "KEY" && f.size() >= 2) {
            key_text(u82w(b64dec(f[1])));
        } else if (t == "SKEY" && f.size() >= 2) {
            key_special(f[1]);
        } else if (t == "REBOOT") {
            do_power(false);
        } else if (t == "SHUTDOWN") {
            do_power(true);
        }
    }
    return true;
}

// =====================================================================
//  主流程
// =====================================================================
// ---------------------------------------------------------------- DPI 感知
// 关键：不声明 DPI 感知时，Windows 在高分屏（2560x1440@125%、4K@150% 等）
// 会对本进程做 GDI 虚拟化：GetSystemMetrics 返回缩小后的假分辨率、BitBlt
// 抓屏被 DWM 位图拉伸（右/下边缘缺失、模糊）、坐标与 SendInput 的物理像素
// 坐标系不一致 —— 表现为“画面不完整 + 点击坐标偏移”。声明感知后，抓屏 /
// 分辨率上报 / 鼠标注入三者统一为真实物理像素，任何分辨率与缩放比例下一致。
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)-4)
#endif
static void set_dpi_aware() {
    // Win10 1703+：每显示器感知 V2（动态加载，Win7/老 Win10 上不存在则回退）
    typedef BOOL(WINAPI *SPDAC_t)(HANDLE);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        SPDAC_t f = (SPDAC_t)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
        if (f && f(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    }
    SetProcessDPIAware();      // Win7+：系统级 DPI 感知
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    set_dpi_aware();           // 必须最先调用，任何 GDI/截图操作之前
    // 解析命令行参数
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool wantInstall = false, wantUninstall = false;
    for (int i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"/install") == 0) wantInstall = true;
        else if (_wcsicmp(argv[i], L"/uninstall") == 0) wantUninstall = true;
    }
    if (argv) LocalFree(argv);

    load_config();
    install_startup();          // 每次启动都重新写入自启动（需求）
    if (wantUninstall) { uninstall(); return 0; }
    if (wantInstall) return 0;

    parse_server();
    detect_os();
    {
        wchar_t usr[256] = {0}; DWORD n = 255;
        GetUserNameW(usr, &n); g_user = usr;
        SYSTEM_INFO si; GetNativeSystemInfo(&si);
        g_arch = (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
                  si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_IA64) ? "x64" : "x86";
        g_priv = is_elevated() ? "1" : "0";
        g_sw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        g_sh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    }
    make_cid();

    // 非管理员且已存在高权限计划任务 → 直接让位（避免抢锁导致任务实例退出）
    if (g_priv != "1" && task_exists()) {
        logf("defer to elevated scheduled task instance");
        return 0;
    }

    // 单实例互斥：若已有实例运行则等待后重试一次，仍被占用才退出。
    // 锁由进程持有至退出，由操作系统自动释放（异常退出也不会残留）。
    HANDLE mtx = CreateMutexW(NULL, TRUE, MRMS_MUTEX);
    if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mtx); mtx = NULL;
        Sleep(is_elevated() ? 3000 : 5000);
        mtx = CreateMutexW(NULL, TRUE, MRMS_MUTEX);
        if (mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(mtx);
            return 0;                        // 已有实例在运行
        }
    }
    (void)mtx;                               // 持有至进程结束

    Gdiplus::GdiplusStartupInput gin;
    if (Gdiplus::GdiplusStartup(&g_gdip, &gin, NULL) != Gdiplus::Ok) g_gdip = 0;

    logf("MRMS client %s started cid=%ls os=%s priv=%s arch=%s server=%ls",
         MRMS_VER, g_cid.c_str(), g_os.c_str(), g_priv.c_str(), g_arch.c_str(),
         g_server.c_str());

    int fails = 0;
    for (;;) {
        bool ok = do_poll();
        if (ok) {
            if (fails) logf("recovered after %d failures", fails);
            fails = 0;
        } else {
            fails++;
            if (fails % 10 == 1)
                logf("server unreachable (fail #%d): %ls", fails, g_server.c_str());
        }
        DWORD ms = (DWORD)((fails ? (3 + (fails > 3 ? 5 : 0)) : g_interval) * 1000);
        Sleep(ms);
    }
}
