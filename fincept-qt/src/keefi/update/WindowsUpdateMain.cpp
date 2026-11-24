// Runs at the desktop user's privilege level. No service or elevation is used.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <sddl.h>
#include <sodium.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>
#include <cwctype>

namespace fs = std::filesystem;
using Str = std::wstring;
struct Error { int code; };
void require(bool ok, int code) { if (!ok) throw Error{code}; }
struct Handle {
    HANDLE h;
    explicit Handle(HANDLE value) : h(value) {}
    ~Handle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    Handle(const Handle&) = delete;
    bool valid() const { return h && h != INVALID_HANDLE_VALUE; }
};
Str folded(Str value) {
    for (auto& c : value) c = wchar_t(std::towlower(c));
    return value;
}
bool same(const fs::path& a, const fs::path& b) {
    return !_wcsicmp(a.c_str(), b.c_str());
}
bool within(const fs::path& child, const fs::path& parent) {
    auto ci = child.begin();
    for (const auto& component : parent) {
        if (ci == child.end() || !same(*ci, component)) return false;
        ++ci;
    }
    return true;
}
void noRedirection(const fs::path& p) {
    fs::path prefix = p.root_path();
    require(p.is_absolute() && p.root_name().wstring().size() == 2, 11);
    for (const auto& component : p.relative_path()) {
        require(component != L".." && component != L".", 11);
        prefix /= component;
        DWORD attrs = GetFileAttributesW(prefix.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            auto err = GetLastError();
            require(err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND, 11);
        } else require(!(attrs & FILE_ATTRIBUTE_REPARSE_POINT), 11);
    }
}
Str processPath(HANDLE handle) {
    Str buffer(32768, L'\0'); DWORD len = DWORD(buffer.size());
    require(QueryFullProcessImageNameW(handle, 0, buffer.data(), &len), 12);
    buffer.resize(len); return buffer;
}
Str quote(const Str& arg) {
    Str out(1,L'"'); size_t slashes = 0;
    for (auto ch : arg) {
        if (ch == L'\\') { ++slashes; continue; }
        if (ch == L'"') { out.append(2 * slashes + 1, L'\\'); out.push_back(ch); }
        else { out.append(slashes, L'\\'); out.push_back(ch); }
        slashes = 0;
    }
    out.append(2 * slashes, L'\\'); out.push_back(L'"'); return out;
}
HANDLE spawn(const fs::path& exe, const std::vector<Str>& args, const fs::path& cwd, bool hidden) {
    Str cmd = quote(exe.wstring()); for (const auto& a : args) cmd += L" " + quote(a);
    STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        hidden ? CREATE_NO_WINDOW : 0, nullptr, cwd.c_str(), &si, &pi)) return nullptr;
    CloseHandle(pi.hThread); return pi.hProcess;
}
Str hashString(const unsigned char* hash) {
    char hex[65]{}; sodium_bin2hex(hex, sizeof(hex), hash, 32);
    return Str(hex, hex + 64);
}
bool validDigest(HANDLE archive, const Str& expected) {
    LARGE_INTEGER bytes{};
    if (expected.size() != 64 || !GetFileSizeEx(archive,&bytes) || bytes.QuadPart <= 0 ||
        bytes.QuadPart > 512LL * 1024 * 1024) return false;
    crypto_hash_sha256_state state; crypto_hash_sha256_init(&state);
    std::array<unsigned char,65536> block{};
    DWORD count = 0;
    do {
        if (!ReadFile(archive,block.data(),DWORD(block.size()),&count,nullptr)) return false;
        crypto_hash_sha256_update(&state,block.data(),count);
    } while (count);
    unsigned char hash[32]; crypto_hash_sha256_final(&state,hash);
    return hashString(hash) == expected;
}
std::array<unsigned,4> fileVersion(const fs::path& exe) {
    DWORD dummy = 0, size = GetFileVersionInfoSizeW(exe.c_str(), &dummy);
    require(size > 0 && size < 1024*1024, 13);
    std::vector<unsigned char> data(size);
    require(GetFileVersionInfoW(exe.c_str(), 0, size, data.data()), 13);
    VS_FIXEDFILEINFO* info = nullptr; UINT len = 0;
    require(VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) &&
            len >= sizeof(VS_FIXEDFILEINFO) && info->dwSignature == 0xFEEF04BDU, 13);
    return {HIWORD(info->dwFileVersionMS),LOWORD(info->dwFileVersionMS),
            HIWORD(info->dwFileVersionLS),LOWORD(info->dwFileVersionLS)};
}
Str versionText(const std::array<unsigned,4>& v) {
    return std::to_wstring(v[0])+L"."+std::to_wstring(v[1])+L"."+std::to_wstring(v[2]);
}
void makePrivate(const fs::path& dir) {
    HANDLE raw = nullptr; require(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw),14);
    Handle token(raw); DWORD size = 0; GetTokenInformation(token.h,TokenUser,nullptr,0,&size);
    std::vector<unsigned char> storage(size);
    require(GetTokenInformation(token.h,TokenUser,storage.data(),size,&size),14);
    wchar_t* sid = nullptr;
    require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid,&sid),14);
    Str acl = L"O:"+Str(sid)+L"D:P(A;OICI;FA;;;"+Str(sid)+L")"; LocalFree(sid);
    PSECURITY_DESCRIPTOR desc = nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,&desc,nullptr),14);
    SECURITY_ATTRIBUTES attr{sizeof(attr),desc,FALSE};
    bool created = CreateDirectoryW(dir.c_str(),&attr); LocalFree(desc); require(created,14);
}
void move(const fs::path& from, const fs::path& to, bool replace = false) {
    // Antivirus can briefly hold freshly scanned files after child processes exit.
    for (int i=0; i<30; ++i) {
        if (MoveFileExW(from.c_str(),to.c_str(),MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0))) return;
        DWORD e=GetLastError();
        if(e != ERROR_SHARING_VIOLATION && e != ERROR_ACCESS_DENIED) break;
        Sleep(100);
    }
    throw Error{19};
}
void removeStage(const fs::path& path) {
    std::error_code ignored;
    fs::remove_all(path,ignored);
}
struct CaseOrder { bool operator()(const Str& a,const Str& b) const { return _wcsicmp(a.c_str(),b.c_str())<0; } };
using Names = std::set<Str,CaseOrder>;
constexpr const wchar_t* inventoryName = L"keefi-package-files.txt";
Names inventory(const fs::path& dir) {
    const fs::path f=dir/inventoryName; noRedirection(f);
    require(fs::is_regular_file(f) && fs::file_size(f)<1024*1024, 15);
    std::ifstream stream(f, std::ios::binary);
    Names result; std::string line;
    while(std::getline(stream,line)) {
        if(!line.empty() && line.back()=='\r') line.pop_back();
        require(!line.empty() && line.size()<512 && std::all_of(line.begin(),line.end(),
            [](unsigned char c){return (c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9') ||
                c=='/'||c=='+'||c=='-'||c=='_'||c=='.'||c==' ';}),15);
        fs::path rel(line); require(!rel.is_absolute() && !rel.has_root_name() &&
            rel.generic_string()==line && !rel.filename().empty(),15);
        for(const auto& component:rel) {
            Str n=component.wstring(); require(n!=L".." && n!=L"." && n.back()!=L'.' && n.back()!=L' ',15);
        }
        // Existing portable inventories can never authorize deletion of arbitrary
        // documents, wallets or account files beside the installed app.
        const auto lower=folded(rel.generic_wstring());
        if (!rel.has_parent_path()) {
            require(rel.extension()==L".dll" || lower==L"keefi.exe" || lower==L"keefiupdate.exe" ||
                    lower==L"qt.conf" || lower==L"license" || lower==L"notice" ||
                    lower==L"third-party-notices.md" || lower==inventoryName,15);
        } else {
            const Str top=folded(rel.begin()->wstring());
            require(top==L"licenses" || (rel.extension()==L".dll" &&
                (top==L"generic" || top==L"imageformats" || top==L"networkinformation" ||
                 top==L"platforms" || top==L"styles" || top==L"tls")),15);
        }
        require(result.emplace(rel.generic_wstring()).second,15);
    }
    require(stream.eof() && result.count(L"keefi.exe") && result.count(L"keefiupdate.exe") &&
            result.count(L"platforms/qwindows.dll") && result.count(inventoryName),15);
    return result;
}
void verifyStage(const fs::path& dir, const Names& owned) {
    Names found;
    for(const auto& it:fs::recursive_directory_iterator(dir)) {
        auto attrs=GetFileAttributesW(it.path().c_str());
        require(attrs!=INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT),15);
        if(it.is_directory()) continue;
        require(it.is_regular_file(),15);
        require(found.emplace(it.path().lexically_relative(dir).generic_wstring()).second,15);
    }
    require(owned==found,15);
}
void startInstalled(const fs::path& exe,const std::vector<Str>& profile) {
    Handle app(spawn(exe,profile,exe.parent_path(),false));
    require(app.valid() && WaitForSingleObject(app.h,1500)==WAIT_TIMEOUT,20);
}
void unpack(const fs::path& zip, const fs::path& dest) {
    wchar_t sys[MAX_PATH]; UINT len=GetSystemDirectoryW(sys,MAX_PATH); require(len && len<MAX_PATH,16);
    Handle tar(spawn(fs::path(sys)/"tar.exe",{L"-xf",zip.wstring(),L"-C",dest.wstring()},dest,true));
    require(tar.valid(),16);
    DWORD wait=WaitForSingleObject(tar.h,60000);
    if(wait!=WAIT_OBJECT_0) { TerminateProcess(tar.h,1); WaitForSingleObject(tar.h,5000); throw Error{16}; }
    DWORD code=1; require(GetExitCodeProcess(tar.h,&code)&&code==0,16);
    size_t roots=0; for(const auto& entry:fs::directory_iterator(dest)) {
        require(entry.path().filename()==L"KEEFI" && entry.is_directory(),16); ++roots;
    }
    require(roots==1,16);
}

// Preserve a complete runnable copy of the previous installation before the
// first mutation. Each live pathname is replaced in one write-through rename.
// Unmanaged documents and profile files never move, even temporarily.
void install(const fs::path& app,const fs::path& candidate,const fs::path& work,
             const Names& oldFiles,const Names& newFiles,const std::vector<Str>& profile) {
    const fs::path backup=work/L"previous";
    require(fs::create_directory(backup),19);
    for (const auto& name:oldFiles) {
        const fs::path rel(name), old=app/rel;
        noRedirection(old);
        require(fs::is_regular_file(old),18);
        fs::create_directories((backup/rel).parent_path());
        require(CopyFileW(old.c_str(),(backup/rel).c_str(),TRUE),19);
    }
    std::vector<fs::path> changed;
    changed.reserve(oldFiles.size()+newFiles.size());
    auto apply=[&](const fs::path& rel) {
        const auto dest=app/rel;
        noRedirection(dest);
        fs::create_directories(dest.parent_path());
        const bool previouslyOwned=oldFiles.count(rel.generic_wstring())!=0;
        move(candidate/rel,dest,previouslyOwned);
        changed.push_back(rel);
    };
    try {
        // Replace the entry-point last, after its DLLs, plugins and notices.
        for(const auto& name:newFiles) if(_wcsicmp(name.c_str(),L"KEEFI.exe")!=0) apply(fs::path(name));
        for(const auto& name:oldFiles) {
            if(newFiles.count(name)) continue;
            const fs::path rel(name);
            const auto obsolete=work/L"obsolete"/rel;
            fs::create_directories(obsolete.parent_path());
            move(app/rel,obsolete);
            changed.push_back(rel);
        }
        apply(L"KEEFI.exe");
        startInstalled(app/L"KEEFI.exe",profile);
    } catch(...) {
        try {
            for(auto it=changed.rbegin();it!=changed.rend();++it) {
                const auto dest=app/ *it;
                if(oldFiles.count(it->generic_wstring())) {
                    const auto restore=work/L"restore"/ *it;
                    fs::create_directories(restore.parent_path());
                    require(CopyFileW((backup/ *it).c_str(),restore.c_str(),TRUE),21);
                    move(restore,dest,true);
                } else {
                    require(DeleteFileW(dest.c_str()),21);
                }
            }
        } catch (...) { throw Error{21}; }
        throw;
    }
}
int update(int argc,wchar_t** argv) {
    require(argc==8 || argc==9,10);
    const DWORD pid=std::stoul(argv[1]); Str event=argv[2];
    require(pid && event.starts_with(L"Local\\KEEFI-Update-") && event.size()<100,10);
    const auto exe=fs::absolute(argv[3]).lexically_normal(), archive=fs::absolute(argv[4]).lexically_normal();
    const auto app=exe.parent_path();
    require(same(exe.filename(),L"KEEFI.exe") && !same(app,app.root_path()) && !within(archive,app),10);
    noRedirection(exe); noRedirection(archive);
    Handle parent(OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid));
    require(parent.valid() && fs::equivalent(fs::path(processPath(parent.h)),exe),12);
    Handle ready(OpenEventW(EVENT_MODIFY_STATE,FALSE,event.c_str()));
    Handle go(OpenEventW(SYNCHRONIZE,FALSE,(event+L"-commit").c_str()));
    require(ready.valid() && go.valid(),10);
    std::vector<Str> profile;
    if(argc==9) {
        auto path=fs::path(argv[8]);
        require(path.is_absolute() && !within(path.lexically_normal(),app) && !within(app,path.lexically_normal()),17);
        profile={L"--profile-dir",path.wstring()};
    }
    Handle zip(CreateFileW(archive.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    require(zip.valid() && validDigest(zip.h,argv[5]),22);
    const DWORD declared=std::stoul(argv[7]);
    const auto previous=fileVersion(exe); require(declared>previous[3] && declared<=65535,13);
    const Names oldFiles=inventory(app);
    Str pathKey=folded(app.wstring()); unsigned char hash[32];
    crypto_hash_sha256(hash,reinterpret_cast<const unsigned char*>(pathKey.data()),pathKey.size()*sizeof(wchar_t));
    Handle mutex(CreateMutexW(nullptr,TRUE,(L"Local\\KEEFI-Package-"+hashString(hash)).c_str()));
    require(mutex.valid() && GetLastError()!=ERROR_ALREADY_EXISTS,18);
    const auto staging=app.parent_path()/(L".keefi-update-"+event.substr(event.rfind(L'-')+1)+L"-"+std::to_wstring(pid));
    makePrivate(staging);
    bool closed=false;
    int status=0;
    try {
        require(SetEvent(ready.h) && WaitForSingleObject(go.h,10000)==WAIT_OBJECT_0,23);
        require(WaitForSingleObject(parent.h,60000)==WAIT_OBJECT_0,23);
        closed=true;
        unpack(archive,staging);
        const auto payload=staging/L"KEEFI";
        const auto newFiles=inventory(payload);
        verifyStage(payload,newFiles);
        const auto next=fileVersion(payload/L"KEEFI.exe");
        require(next[3]==declared && versionText(next)==argv[6] && next>previous,13);
        // A collision with data that was never part of the installed package
        // aborts before moving a single old file.
        for(const auto& name:newFiles) {
            auto dest=app/fs::path(name); noRedirection(dest);
            require(oldFiles.count(name) || !fs::exists(dest),18);
        }
        install(app,payload,staging,oldFiles,newFiles,profile);
    } catch(const Error& e){status=e.code;}
      catch(...){status=19;}
    if(status!=21) removeStage(staging);
    if(closed && status) {
        if(status!=21) {try { startInstalled(exe,profile); } catch(...) {}}
        MessageBoxW(nullptr,status==21 ? L"업데이트를 복구하지 못했습니다. 앱 옆 .keefi-update 폴더에 이전 파일을 보관했습니다."
            : L"업데이트를 완료하지 못해 이전 버전으로 돌아갑니다. 계좌와 금고는 보존했습니다.",
            L"KEEFI 업데이트",MB_OK|MB_ICONERROR);
    }
    return status;
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,LPWSTR,int) {
    int count=0; auto args=CommandLineToArgvW(GetCommandLineW(),&count);
    if(!args || sodium_init()<0) return 10;
    int status=10;
    try { status=update(count,args); } catch(const Error& e){status=e.code;} catch(...){}
    LocalFree(args); return status;
}
