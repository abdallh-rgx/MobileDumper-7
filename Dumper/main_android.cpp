#ifdef __ANDROID__

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <csignal>
#include <csetjmp>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <jni.h>
#include <memory>
#include <optional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include <cstdarg>
#include <mutex>
#include <android/log.h>

#include <KittyMemoryEx/KittyUtils.hpp>

#include "DumperMain.h"
#include "Settings.h"

#include "Architecture/IArchDecoder.h"
#include "Memory/IMemory.h"
#include "Memory/MemoryAndroid.h"
#include "Profile/IProfile.h"

#ifdef DUMPER_BUILD_EXECUTABLE
#include "Utils/argsparse/argsparse.hpp"
#define COLOR_RESET  "\033[0m"
#define COLOR_BOLD   "\033[1m"
#define COLOR_CYAN   "\033[36m"
#define COLOR_GREEN  "\033[32m"
#define COLOR_YELLOW "\033[33m"
#define COLOR_RED    "\033[31m"
#define COLOR_GRAY   "\033[90m"
#define COLOR_PURPLE "\033[35m"
#endif

#include "Profile/CustomProfiles/Shared/PUBG.h"
#include "Profile/CustomProfiles/Shared/DeltaForce.h"
#include "Profile/CustomProfiles/Shared/Fortnite.h"

#define LOG_TAG "MobileDumper-7"

static FILE*      g_LogFile = nullptr;
static std::mutex g_LogMutex;

static std::string GetCurrentPackage()
{
    char Buf[512] = {};
    FILE* F = fopen("/proc/self/cmdline", "r");
    if (F)
    {
        size_t N = fread(Buf, 1, sizeof(Buf) - 1, F);
        fclose(F);
        Buf[N] = '\0';
    }
    std::string Result(Buf);
    auto Slash = Result.find('/');
    if (Slash != std::string::npos)
        Result = Result.substr(Slash + 1);
    return Result;
}

static void InitLogFile()
{
    std::vector<std::string> Paths;
    const std::string Pkg = GetCurrentPackage();
    if (!Pkg.empty() && Pkg.find('.') != std::string::npos)
    {
        Paths.push_back("/storage/emulated/0/Android/data/" + Pkg + "/files/MobileDumper7.log");
        Paths.push_back("/sdcard/Android/data/" + Pkg + "/files/MobileDumper7.log");
        Paths.push_back("/data/data/" + Pkg + "/files/MobileDumper7.log");
    }
    Paths.push_back("/data/local/tmp/MobileDumper7.log");
    Paths.push_back("/sdcard/MobileDumper7.log");

    for (const auto& P : Paths)
    {
        FILE* F = fopen(P.c_str(), "w");
        if (F)
        {
            g_LogFile = F;
            return;
        }
    }
}

static void LOGF(const char* Fmt, ...)
{
    char Buf[4096];
    va_list ap;
    va_start(ap, Fmt);
    vsnprintf(Buf, sizeof(Buf), Fmt, ap);
    va_end(ap);

    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "%s", Buf);

    if (g_LogFile)
    {
        std::lock_guard<std::mutex> Lock(g_LogMutex);
        fprintf(g_LogFile, "%s\n", Buf);
        fflush(g_LogFile);
    }
}

static void CloseLogFile()
{
    std::lock_guard<std::mutex> Lock(g_LogMutex);
    if (g_LogFile)
    {
        fflush(g_LogFile);
        fclose(g_LogFile);
        g_LogFile = nullptr;
    }
}

static sigjmp_buf g_SafeReadJmp;
static volatile sig_atomic_t g_CanJump = 0;

static void SafeReadHandler(int Sig)
{
    if (g_CanJump)
        siglongjmp(g_SafeReadJmp, 1);
}

static void InstallSafeReadHandler()
{
    struct sigaction Sa;
    memset(&Sa, 0, sizeof(Sa));
    Sa.sa_handler = SafeReadHandler;
    Sa.sa_flags = SA_NODEFER;
    sigemptyset(&Sa.sa_mask);
    sigaction(SIGSEGV, &Sa, nullptr);
    sigaction(SIGBUS, &Sa, nullptr);
    sigaction(SIGILL, &Sa, nullptr);
}

template<typename T>
static bool SafeRead(uintptr_t Addr, T& Out)
{
    if (Addr < 0x10000) return false;
    if (sigsetjmp(g_SafeReadJmp, 1) == 0)
    {
        g_CanJump = 1;
        Out = *(T*)Addr;
        g_CanJump = 0;
        return true;
    }
    g_CanJump = 0;
    return false;
}

inline std::vector<std::shared_ptr<IProfile>> UECustomProfiles;

std::vector<std::shared_ptr<IProfile>>& GetUECustomProfiles()
{
    if (UECustomProfiles.empty())
    {
        UECustomProfiles.push_back(std::make_shared<PUBGProfile>());
        UECustomProfiles.push_back(std::make_shared<DeltaForceProfile>());
        UECustomProfiles.push_back(std::make_shared<FortniteProfile>());
    }
    return UECustomProfiles;
}

class FProcessSuspension
{
    pid_t Pid = 0;

public:
    FProcessSuspension(const FProcessSuspension&)            = delete;
    FProcessSuspension& operator=(const FProcessSuspension&) = delete;

    explicit FProcessSuspension(pid_t InPid)
    {
        if (InPid <= 0) return;
        if (kill(InPid, SIGSTOP) != 0)
        {
            GLogger.FmtWrite(ELogLevel::Warning, "Failed to suspend process {}: {}\n", InPid, strerror(errno));
            return;
        }
        Pid = InPid;
        GLogger.FmtWrite(ELogLevel::Info, "Suspended process {}.\n", Pid);
    }

    ~FProcessSuspension()
    {
        if (Pid <= 0) return;
        if (kill(Pid, SIGCONT) != 0)
            GLogger.FmtWrite(ELogLevel::Warning, "Failed to resume process {}: {}\n", Pid, strerror(errno));
        else
            GLogger.FmtWrite(ELogLevel::Info, "Resumed process {}.\n", Pid);
    }
};

bool RunDump(int GamePid, const std::string& GamePackage, EKittyMemOP MemOp = EK_MEM_OP_SYSCALL, const char* OutputDir = nullptr, bool bDumpLib = false, bool bSuspend = false)
{
    if (!g_LogFile)
        InitLogFile();

    LOGF("========================================");
    LOGF("MobileDumper-7 started");
    LOGF("PID: %d", getpid());
    LOGF("Game: %s", GamePackage.c_str());
    LOGF("========================================");

    GLogger.SetOnDidLogMessage([](ELogLevel Level, const std::string& Msg)
    {
        LOGF("%s", Msg.c_str());
    });

    auto StartTime = std::chrono::high_resolution_clock::now();

    if (OutputDir && *OutputDir)
        GSettings.Generator.SDKGenerationPath = OutputDir;
    else
        GSettings.Generator.SDKGenerationPath = "/storage/emulated/0/Android/data/" + GamePackage + "/files/SDK";

    std::error_code Ec;
    std::filesystem::create_directories(GSettings.Generator.SDKGenerationPath, Ec);
    if (Ec)
    {
        GLogger.FmtWrite(ELogLevel::Error, "Failed to create output dir at \"{}\"\n", GSettings.Generator.SDKGenerationPath, Ec.message());
        FDumperMain::SetOnProgressCallback(nullptr);
        GLogger.CloseFileStream();
        return false;
    }

    GMemory = std::make_unique<FMemoryAndroid>(GamePid, MemOp);

#if defined(__LP64__)
    GArchDecoder = CreateArchDecoder(EArch::Arm64);
#else
    GArchDecoder = CreateArchDecoder(EArch::Arm32);
#endif

    GProfile = nullptr;
    for (const auto& it : GetUECustomProfiles())
    {
        for (const auto& Identifier : it->GetSupportedGames())
        {
            if (Identifier == GamePackage)
            {
                GLogger.FmtWrite(ELogLevel::Info, "Using Custom UE Profile Override ({}).\n", Identifier);
                GProfile = it;
                break;
            }
        }
        if (GProfile) break;
    }

    if (!GProfile)
    {
        auto& Profiles = GetUECustomProfiles();
        if (!Profiles.empty())
        {
            GProfile = Profiles.back();
            GLogger.FmtWrite(ELogLevel::Info, "No explicit package match — using fallback Custom UE Profile (last registered).\n");
        }
        else
        {
            GLogger.FmtWrite(ELogLevel::Info, "Using Generic UE Profile.\n");
            GProfile = std::make_shared<IProfile>();
        }
    }

    std::string OutDumpZip, OutErr;
    bool bSuccess = false;
    bool bIsBitOk = false;
    try
    {
        std::optional<FProcessSuspension> Suspension;
        if (bSuspend && GamePid != getpid())
            Suspension.emplace(GamePid);

        bool isLocal64bit  = !KittyMemoryEx::getMaps(getpid(), EProcMapFilter::Contains, "/lib64/").empty();
        bool isRemote64bit = !KittyMemoryEx::getMaps(GamePid, EProcMapFilter::Contains, "/lib64/").empty();
        bIsBitOk           = isLocal64bit == isRemote64bit;

        if (bIsBitOk)
            bSuccess = FDumperMain::Run(OutDumpZip, OutErr);
        else
        {
            bSuccess = false;
            OutErr   = std::format("Dumper is {}bit but target app is {}bit!", isLocal64bit ? "64" : "32", isRemote64bit ? "64" : "32");
        }
    }
    catch (const std::exception& E) { OutErr = E.what(); }
    catch (...) { OutErr = "Unknown exception"; }

    if (bDumpLib && GMemory && bIsBitOk)
    {
        const ModuleInfo Module = GMemory->GetUnrealModule();
        if (!Module.IsValid())
            GLogger.FmtWrite(ELogLevel::Error, "Memory dump failed, Unable to locate Unreal module!\n");
        else
        {
            const std::string ModuleStem = std::filesystem::path(Module.GetPathName()).stem().string();
            const std::string Destination = fmt::format("{}/{}_{}_{:X}-{:X}.so",
                                                        GSettings.Generator.SDKGenerationPath,
                                                        GamePackage,
                                                        ModuleStem.empty() ? "UnrealModule" : ModuleStem,
                                                        Module.GetStart(),
                                                        Module.GetEnd());
            GLogger.FmtWrite(ELogLevel::Info, "Dumping Unreal module to \"{}\"...\n", Destination);
            if (GMemory->DumpUnrealModule(Destination))
                GLogger.FmtWrite(ELogLevel::Info, "Dumped Unreal module (0x{:X} - 0x{:X}, {} bytes mapped).\n", Module.GetStart(), Module.GetEnd(), Module.GetSize());
            else
                GLogger.FmtWrite(ELogLevel::Error, "Failed to dump the Unreal module.\n");
        }
    }

    auto Elapsed = std::chrono::high_resolution_clock::now() - StartTime;
    GLogger.FmtWrite(ELogLevel::Info, "Duration: {}\n", Utils::ChronoDurationToString(Elapsed));

    if (bSuccess) GLogger.FmtWrite(ELogLevel::Info, "Dump succeded.\n");
    else
    {
        GLogger.FmtWrite(ELogLevel::Error, "Dump failed!\n");
        GLogger.FmtWrite(ELogLevel::Error, "Failure reason: \"{}\"\n", OutErr);
    }

    FDumperMain::SetOnProgressCallback(nullptr);
    GProfile.reset();
    GetUECustomProfiles().clear();
    GArchDecoder.reset();
    GMemory.reset();
    GLogger.CloseFileStream();

    LOGF("========================================");
    LOGF("MobileDumper-7 finished");
    LOGF("========================================");
    return bSuccess;
}

#ifndef DUMPER_BUILD_EXECUTABLE

static uintptr_t FindUE4Base()
{
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char Line[512];
    uintptr_t Base = 0;
    while (fgets(Line, sizeof(Line), f))
    {
        if (strstr(Line, "libUE4.so"))
        {
            uintptr_t Start = 0;
            if (sscanf(Line, "%lx-", &Start) == 1) { Base = Start; break; }
        }
    }
    fclose(f);
    return Base;
}

static int CountValidObjects(uintptr_t Base, int SampleSize)
{
    uintptr_t GObjBase = Base + 0x10249200;
    void* ObjObjects = nullptr;
    if (!SafeRead(GObjBase + 0x10, ObjObjects)) return -1;
    if (!ObjObjects) return -1;

    int NumElements = 0;
    if (!SafeRead((uintptr_t)ObjObjects + 0x14, NumElements)) return -1;
    if (NumElements <= 1000 || NumElements > 5000000) return -1;

    void** Chunks = nullptr;
    if (!SafeRead((uintptr_t)ObjObjects + 0x00, Chunks)) return -1;
    if (!Chunks) return -1;

    int ValidCount = 0;
    for (int i = 0; i < SampleSize; ++i)
    {
        int Idx = (int)((long long)i * NumElements / SampleSize);
        int ChunkIdx = Idx >> 16;
        int InChunk = Idx & 0xFFFF;

        void* Chunk = nullptr;
        if (!SafeRead((uintptr_t)Chunks + (uintptr_t)ChunkIdx * 8, Chunk)) continue;
        if (!Chunk) continue;

        void* Obj = nullptr;
        if (!SafeRead((uintptr_t)Chunk + (uintptr_t)InChunk * 24, Obj)) continue;
        if (!Obj) continue;

        void* Class = nullptr;
        if (!SafeRead((uintptr_t)Obj + 0x10, Class)) continue;
        if (!Class) continue;

        ValidCount++;
    }
    return ValidCount;
}

__attribute__((constructor)) static void OnLibraryLoad()
{
    InitLogFile();
    LOGF("=== OnLibraryLoad: entry ===");

    std::thread([]()
    {
        InstallSafeReadHandler();
        LOGF("SafeRead handler installed");

        uintptr_t Base = 0;
        LOGF("Waiting for libUE4.so...");
        for (int i = 0; i < 1800 && !Base; ++i)
        {
            Base = FindUE4Base();
            if (!Base)
            {
                if (i % 30 == 0) LOGF("[%d] libUE4.so not loaded yet", i);
                sleep(1);
            }
        }
        if (!Base) { LOGF("TIMEOUT: libUE4.so never loaded"); return; }
        LOGF("libUE4.so base: 0x%lX", Base);

        const std::string Package = GetCurrentPackage();
        if (Package.empty() || Package.find('.') == std::string::npos)
        {
            LOGF("Cannot determine package name");
            return;
        }
        LOGF("Package: %s", Package.c_str());

        LOGF("Waiting for GObjects to stabilize...");
        int LastValid = -1;
        int StableCount = 0;
        bool Ready = false;

        for (int Attempt = 1; Attempt <= 3600; ++Attempt)
        {
            int Valid = CountValidObjects(Base, 100);

            if (Valid >= 0)
            {
                if (Valid == LastValid && Valid >= 80)
                    ++StableCount;
                else
                    StableCount = 0;
                LastValid = Valid;

                if (Attempt % 30 == 0)
                    LOGF("[%d] GObjects: %d/100 valid (stable %ds)", Attempt, Valid, StableCount);

                if (StableCount >= 20)
                {
                    LOGF("Ready: GObjects stable at %d/100 for %ds", Valid, StableCount);
                    Ready = true;
                    break;
                }
            }
            else if (Attempt % 30 == 0)
            {
                LOGF("[%d] GObjects: unavailable (waiting for initialization)", Attempt);
            }

            sleep(1);
        }

        if (!Ready)
        {
            LOGF("TIMEOUT: GObjects never stabilized");
            return;
        }

        LOGF("Waiting 15 extra seconds...");
        sleep(15);

        int FinalValid = CountValidObjects(Base, 100);
        LOGF("Final GObjects check: %d/100", FinalValid);

        for (int Attempt = 1; Attempt <= 3; ++Attempt)
        {
            LOGF("=== Dump attempt %d/3 ===", Attempt);
            if (RunDump(getpid(), Package, EK_MEM_OP_SYSCALL, nullptr, true, false))
            {
                LOGF("SUCCESS on attempt %d", Attempt);
                return;
            }
            LOGF("Attempt %d failed, waiting 3 min before retry", Attempt);
            sleep(180);
        }
        LOGF("All dump attempts failed");
    }).detach();
}

#else

bool ReadIntegerInput(int& OutputValue);
bool ChooseUserProcess(pid_t& OutPid, std::string& OutId);
bool ChooseProcessFromMultiple(const std::vector<pid_t>& Pids, pid_t& OutPid);
bool ChooseMemoryAccessType(int& OutMemAccessType);
bool ChooseOutputDirectory(std::string& OutOutputDir);

int main(int Argc, char** Args)
{
    setbuf(stdout, nullptr);
    setbuf(stderr, nullptr);
    setbuf(stdin, nullptr);

    InitLogFile();
    LOGF("MobileDumper-7 CLI started");

    argparse::ArgumentParser Program(FDumperMain::kProgramName, FDumperMain::kProgramVer);

    std::string GamePackge, OutputDir;
    bool bDumpLib = false, bSuspendGame = false;
    int MemAccessType = 0;

    Program.add_argument("-p", "--package").store_into(GamePackge).metavar("<name>");
    Program.add_argument("-o", "--output").store_into(OutputDir).metavar("<path>");
    Program.add_argument("-d", "--dump").default_value(false).implicit_value(true).store_into(bDumpLib);
    Program.add_argument("-s", "--suspend").default_value(false).implicit_value(true).store_into(bSuspendGame);
    Program.add_argument("-m", "--mem").scan<'i', int>().choices(1, 2).store_into(MemAccessType);

    try { Program.parse_args(Argc, Args); }
    catch (const std::exception& Err)
    {
        GLogger.FmtWrite(ELogLevel::Error, "Command Line Error: {}\n\n", Err.what());
        GLogger.FmtWrite(ELogLevel::Info, "{}", Program.help().str());
        return 1;
    }

    pid_t GamePid = 0;
    if (GamePackge.empty())
    {
        if (!ChooseUserProcess(GamePid, GamePackge)) return 1;
    }
    else
    {
        auto GamePids = KittyMemoryEx::getProcessIDs(GamePackge);
        if (GamePids.empty()) { GLogger.FmtWrite(ELogLevel::Error, "No process for \"{}\"\n", GamePackge); return 1; }
        if (!ChooseProcessFromMultiple(GamePids, GamePid)) return 1;
    }

    if (MemAccessType == 0 && !ChooseMemoryAccessType(MemAccessType)) return 1;
    if (OutputDir.empty())
    {
        if (!ChooseOutputDirectory(OutputDir)) return 1;
        if (access(OutputDir.c_str(), W_OK) != 0) { GLogger.FmtWrite(ELogLevel::Error, "Not writable: {}\n", OutputDir); return 1; }
    }

    EKittyMemOP MemOp = (MemAccessType == 2) ? EK_MEM_OP_IO : EK_MEM_OP_SYSCALL;
    bool bSuccess = RunDump(GamePid, GamePackge, MemOp, OutputDir.c_str(), bDumpLib, bSuspendGame);
    return bSuccess ? 0 : 1;
}

bool ReadIntegerInput(int& OutputValue)
{
    std::string Input;
    if (!std::getline(std::cin, Input)) return false;
    try { OutputValue = std::stoi(Input); return true; }
    catch (...) { return false; }
}

bool ChooseUserProcess(pid_t& OutPid, std::string& OutId)
{
    struct E { pid_t Pid; std::string Name; };
    std::vector<E> Procs;
    DIR* d = opendir("/proc");
    if (!d) return false;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr)
    {
        pid_t p = 0;
        for (const char* c = de->d_name; *c; ++c)
        {
            if (!isdigit((unsigned char)*c)) { p = -1; break; }
            p = p * 10 + (*c - '0');
        }
        if (p <= 0) continue;
        char path[64]; snprintf(path, sizeof(path), "/proc/%d/cmdline", p);
        FILE* f = fopen(path, "r"); if (!f) continue;
        char buf[512] = {}; size_t n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f);
        if (!n) continue;
        std::string name(buf);
        if (!std::isalpha(name[0]) || name.find('.') == std::string::npos) continue;
        if (!fs::exists(KittyUtils::Android::getAppExternalFilesDir(name))) continue;
        Procs.push_back({p, name});
    }
    closedir(d);
    if (Procs.empty()) return false;
    std::sort(Procs.begin(), Procs.end(), [](const E& a, const E& b) { return a.Name < b.Name; });
    for (size_t i = 0; i < Procs.size(); ++i)
        std::cout << "[" << (i + 1) << "] " << Procs[i].Name << " PID: " << Procs[i].Pid << "\n";
    std::cout << "Select: ";
    int idx = 0;
    if (!ReadIntegerInput(idx) || idx < 1 || idx > (int)Procs.size()) return false;
    OutPid = Procs[idx - 1].Pid;
    OutId = Procs[idx - 1].Name;
    return true;
}

bool ChooseProcessFromMultiple(const std::vector<pid_t>& Pids, pid_t& OutPid)
{
    if (Pids.empty()) return false;
    if (Pids.size() == 1) { OutPid = Pids.front(); return true; }
    for (size_t i = 0; i < Pids.size(); ++i) std::cout << "[" << (i + 1) << "] PID " << Pids[i] << "\n";
    int idx = 0;
    if (!ReadIntegerInput(idx) || idx < 1 || idx > (int)Pids.size()) return false;
    OutPid = Pids[idx - 1];
    return true;
}

bool ChooseMemoryAccessType(int& OutMemAccessType)
{
    std::cout << "1) process_vm_readv\n2) pread\nSelect: ";
    int t = 0;
    if (!ReadIntegerInput(t) || t < 1 || t > 2) return false;
    OutMemAccessType = t;
    return true;
}

bool ChooseOutputDirectory(std::string& OutOutputDir)
{
    std::cout << "Output path: ";
    std::getline(std::cin >> std::ws, OutOutputDir);
    return !OutOutputDir.empty();
}

#endif

#endif
