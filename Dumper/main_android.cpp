#ifdef __ANDROID__

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cerrno>
#include <csignal>
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
        if (InPid <= 0)
            return;

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
        if (Pid <= 0)
            return;

        if (kill(Pid, SIGCONT) != 0)
            GLogger.FmtWrite(ELogLevel::Warning, "Failed to resume process {}: {}\n", Pid, strerror(errno));
        else
            GLogger.FmtWrite(ELogLevel::Info, "Resumed process {}.\n", Pid);
    }
};

bool RunDump(int GamePid, const std::string& GamePackage, EKittyMemOP MemOp = EK_MEM_OP_SYSCALL, const char* OutputDir = nullptr, bool bDumpLib = false, bool bSuspend = false)
{
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
        GLogger.FmtWrite(ELogLevel::Error, "Error: \"{}\"\n", Ec.message());
        FDumperMain::SetOnProgressCallback(nullptr);
        GLogger.CloseFileStream();
        CloseLogFile();
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
        {
            bSuccess = FDumperMain::Run(OutDumpZip, OutErr);
        }
        else
        {
            bSuccess = false;
            OutErr   = std::format("Dumper is {}bit but target app is {}bit!",
                                   isLocal64bit ? "64" : "32",
                                   isRemote64bit ? "64" : "32");
        }
    }
    catch (const std::exception& E)
    {
        OutErr = E.what();
    }
    catch (...)
    {
        OutErr = "Unknown exception";
    }

    if (bDumpLib && GMemory && bIsBitOk)
    {
        const ModuleInfo Module = GMemory->GetUnrealModule();
        if (!Module.IsValid())
        {
            GLogger.FmtWrite(ELogLevel::Error, "Memory dump failed, Unable to locate Unreal module!\n");
        }
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

    if (bSuccess)
    {
        GLogger.FmtWrite(ELogLevel::Info, "Dump succeded.\n");
    }
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
    CloseLogFile();

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
            if (sscanf(Line, "%lx-", &Start) == 1)
            {
                Base = Start;
                break;
            }
        }
    }
    fclose(f);
    return Base;
}

static std::string DecodeFName(uintptr_t Base, uint32_t NameIdx)
{
    if (NameIdx == 0 || NameIdx == 0xFFFFFFFF) return "";
    uintptr_t PoolBase = Base + 0x101B5EC0;
    uint32_t BlockIdx = (NameIdx >> 16) & 0x3FFF;
    uint32_t Offset = NameIdx & 0xFFFF;
    uintptr_t BlockPtr = *(uintptr_t*)(PoolBase + BlockIdx * 8);
    if (!BlockPtr)
    {
        uintptr_t Blocks = *(uintptr_t*)PoolBase;
        if (Blocks) BlockPtr = *(uintptr_t*)(Blocks + BlockIdx * 8);
    }
    if (!BlockPtr) return "";
    uintptr_t Entry = BlockPtr + Offset;
    uint16_t Header = *(uint16_t*)Entry;
    for (int Shift : {1, 6})
    {
        uint32_t Len = Header >> Shift;
        if (Len == 0 || Len > 512) continue;
        bool Wide = Header & 1;
        std::string Out;
        if (Wide)
        {
            char16_t* W = (char16_t*)(Entry + 2);
            for (uint32_t i = 0; i < Len && i < 256; i++)
                Out += (W[i] < 128) ? (char)W[i] : '?';
        }
        else
        {
            const char* S = (const char*)(Entry + 2);
            Out.assign(S, Len);
        }
        bool Valid = !Out.empty();
        for (char c : Out) if (c < 32 || c > 126) { Valid = false; break; }
        if (Valid) return Out;
    }
    return "";
}

static std::string GetWorldName(uintptr_t Base)
{
    uintptr_t GWorldAddr = Base + 0x10191070;
    void* World = *(void**)GWorldAddr;
    if (!World) return "";
    uint32_t NameIdx = *(uint32_t*)((uintptr_t)World + 0x18);
    return DecodeFName(Base, NameIdx);
}

static int CountNonZeroObjects(uintptr_t Base, int SampleSize)
{
    uintptr_t GObjBase = Base + 0x10249200;
    void* ObjObjects = (void*)(GObjBase + 0x10);
    void* ChunksPtr = *(void**)ObjObjects;
    if (!ChunksPtr) return -1;
    int NumElements = *(int*)((uintptr_t)ObjObjects + 0x14);
    if (NumElements <= 0 || NumElements > 5000000) return -1;
    int ValidCount = 0;
    for (int i = 0; i < SampleSize; ++i)
    {
        int Idx = (int)((long long)i * NumElements / SampleSize);
        int ChunkIdx = Idx >> 16;
        int InChunk = Idx & 0xFFFF;
        uintptr_t ChunkArray = *(uintptr_t*)ChunksPtr;
        if (!ChunkArray) continue;
        uintptr_t Chunk = *(uintptr_t*)(ChunkArray + ChunkIdx * 8);
        if (!Chunk) continue;
        void* Obj = *(void**)(Chunk + InChunk * 24);
        if (Obj) ++ValidCount;
    }
    return ValidCount;
}

__attribute__((constructor)) static void OnLibraryLoad()
{
    std::thread([]()
    {
        uintptr_t Base = 0;
        for (int i = 0; i < 1800 && !Base; ++i)
        {
            Base = FindUE4Base();
            if (!Base) sleep(1);
        }
        if (!Base)
        {
            LOGF("Timeout: libUE4.so never loaded (PartyHub stuck?)");
            return;
        }
        LOGF("libUE4.so base: 0x%lX", Base);

        LOGF("Waiting for UE4 Frontend to fully initialize...");

        std::string LastWorld;
        int WorldStableSeconds = 0;
        int GObjectsStableSeconds = 0;
        int LastGObjectsValid = -1;
        bool Ready = false;

        for (int Attempt = 1; Attempt <= 3600; ++Attempt)
        {
            void* WorldPtr = *(void**)(Base + 0x10191070);
            if (!WorldPtr)
            {
                WorldStableSeconds = 0;
                GObjectsStableSeconds = 0;
                if (Attempt % 30 == 0)
                    LOGF("[%d] GWorld null (UE4 not started yet)", Attempt);
                sleep(1);
                continue;
            }

            std::string World = GetWorldName(Base);
            if (World.empty())
            {
                if (Attempt % 30 == 0)
                    LOGF("[%d] GWorld=%p but name unresolved", Attempt, WorldPtr);
                WorldStableSeconds = 0;
                sleep(1);
                continue;
            }

            if (World != LastWorld)
            {
                LOGF("[%d] World: '%s' (prev: '%s')", Attempt, World.c_str(),
                     LastWorld.empty() ? "<none>" : LastWorld.c_str());
                LastWorld = World;
                WorldStableSeconds = 0;
            }
            else
            {
                ++WorldStableSeconds;
            }

            int ValidObjs = CountNonZeroObjects(Base, 100);
            if (ValidObjs >= 0)
            {
                if (ValidObjs == LastGObjectsValid)
                    ++GObjectsStableSeconds;
                else
                {
                    GObjectsStableSeconds = 0;
                    LastGObjectsValid = ValidObjs;
                }
                if (Attempt % 15 == 0)
                    LOGF("[%d] World='%s' (stable %ds), GObjects=%d/100 valid (stable %ds)",
                         Attempt, World.c_str(), WorldStableSeconds, ValidObjs, GObjectsStableSeconds);
            }

            bool IsMenu = (World.find("Frontend") != std::string::npos) ||
                          (World.find("FrontEnd") != std::string::npos);
            bool IsGame = (World.find("_Terrain") != std::string::npos) ||
                          (World.find("Creative") != std::string::npos);

            int RequiredWorldStable = (IsMenu || IsGame) ? 20 : 40;
            int RequiredGObjectsStable = 15;

            if (WorldStableSeconds >= RequiredWorldStable &&
                GObjectsStableSeconds >= RequiredGObjectsStable &&
                ValidObjs >= 90)
            {
                LOGF("Ready to dump: World='%s' stable %ds, GObjects stable %ds (%d/100)",
                     World.c_str(), WorldStableSeconds, GObjectsStableSeconds, ValidObjs);
                Ready = true;
                break;
            }

            if (Attempt == 3600)
            {
                LOGF("Timeout: World='%s' stable %ds, GObjects=%d/100 — dumping anyway",
                     World.c_str(), WorldStableSeconds, ValidObjs);
                Ready = true;
            }

            sleep(1);
        }

        if (!Ready)
            return;

        LOGF("Waiting 10 extra seconds for final safety margin...");
        sleep(10);

        std::string FinalWorld = GetWorldName(Base);
        int FinalValid = CountNonZeroObjects(Base, 100);
        LOGF("Final check: World='%s', GObjects=%d/100", FinalWorld.c_str(), FinalValid);

        if (FinalValid < 80)
        {
            LOGF("WARNING: GObjects only %d/100 — dump may fail validation", FinalValid);
        }

        const std::string GamePackage = GetCurrentPackage();
        if (GamePackage.empty() || GamePackage.find('.') == std::string::npos)
        {
            LOGF("Failed to detect current package from /proc/self/cmdline");
            return;
        }
        LOGF("Starting dump for '%s' from world '%s'", GamePackage.c_str(), FinalWorld.c_str());
        RunDump(getpid(), GamePackage, EK_MEM_OP_SYSCALL, nullptr, true, false);
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
    bool bDumpLib     = false;
    bool bSuspendGame = false;
    int MemAccessType = 0;

    Program.add_argument("-p", "--package")
        .store_into(GamePackge)
        .metavar("<name>");

    Program.add_argument("-o", "--output")
        .store_into(OutputDir)
        .metavar("<path>");

    Program.add_argument("-d", "--dump")
        .default_value(false)
        .implicit_value(true)
        .store_into(bDumpLib);

    Program.add_argument("-s", "--suspend")
        .default_value(false)
        .implicit_value(true)
        .store_into(bSuspendGame);

    Program.add_argument("-m", "--mem")
        .scan<'i', int>()
        .choices(1, 2)
        .store_into(MemAccessType);

    try
    {
        Program.parse_args(Argc, Args);
    }
    catch (const std::exception& Err)
    {
        GLogger.FmtWrite(ELogLevel::Error, "✘ Command Line Error: {}\n\n", Err.what());
        GLogger.FmtWrite(ELogLevel::Info, "{}", Program.help().str());
        CloseLogFile();
        return 1;
    }

    pid_t GamePid = 0;

    if (GamePackge.empty())
    {
        if (!ChooseUserProcess(GamePid, GamePackge))
        {
            GLogger.FmtWrite(ELogLevel::Error, "No user process was selected.\n");
            CloseLogFile();
            return 1;
        }
    }
    else
    {
        auto GamePids = KittyMemoryEx::getProcessIDs(GamePackge);
        if (GamePids.empty())
        {
            GLogger.FmtWrite(ELogLevel::Error, "Couldn't find target package \"{}\" in the running processes.\n", GamePackge);
            CloseLogFile();
            return 1;
        }

        if (!ChooseProcessFromMultiple(GamePids, GamePid))
        {
            GLogger.FmtWrite(ELogLevel::Error, "Failed to select process ID.\n");
            CloseLogFile();
            return 1;
        }
    }

    if (MemAccessType == 0)
    {
        if (!ChooseMemoryAccessType(MemAccessType))
        {
            GLogger.FmtWrite(ELogLevel::Error, "Failed to select memory access type.\n");
            CloseLogFile();
            return 1;
        }
    }

    if (OutputDir.empty())
    {
        if (!ChooseOutputDirectory(OutputDir))
        {
            GLogger.FmtWrite(ELogLevel::Error, "Output directory path is not specified.\n");
            CloseLogFile();
            return 1;
        }

        if (access(OutputDir.c_str(), W_OK) != 0)
        {
            GLogger.FmtWrite(ELogLevel::Error, "Output directory path (\"{}\") is not writeable!\n", OutputDir);
            CloseLogFile();
            return 1;
        }
    }

    std::cout << std::endl;

    EKittyMemOP MemOp = (MemAccessType == 2) ? EK_MEM_OP_IO : EK_MEM_OP_SYSCALL;

    GLogger.FmtWrite(ELogLevel::Info, "Process ID: {}\n", GamePid);
    GLogger.FmtWrite(ELogLevel::Info, "Process Name: {}\n", GamePackge);
    GLogger.FmtWrite(ELogLevel::Info, "Memory Access: {}\n", MemOp == EK_MEM_OP_IO ? "pread" : "process_vm_readv");
    GLogger.FmtWrite(ELogLevel::Info, "Output Directory: {}\n", OutputDir);
    GLogger.FmtWrite(ELogLevel::Info, "Dump UE Library: {}\n", bDumpLib ? "Yes" : "No");
    GLogger.FmtWrite(ELogLevel::Info, "Suspend Game: {}\n", bSuspendGame ? "Yes" : "No");
    GLogger.FmtWrite(ELogLevel::Info, "==========================\n");

    bool bSuccess = RunDump(GamePid, GamePackge, MemOp, OutputDir.c_str(), bDumpLib, bSuspendGame);
    return bSuccess ? 0 : 1;
}

bool ReadIntegerInput(int& OutputValue)
{
    std::string Input;
    if (!std::getline(std::cin, Input))
    {
        return false;
    }
    try
    {
        OutputValue = std::stoi(Input);
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

bool ChooseUserProcess(pid_t& OutPid, std::string& OutId)
{
    struct UserProcessEntry
    {
        pid_t Pid;
        std::string Name;
    };

    std::vector<UserProcessEntry> UserProcesses;

    DIR* ProcDir = opendir("/proc");
    if (!ProcDir)
    {
        GLogger.FmtWrite(ELogLevel::Error, "Failed to open /proc — is the process running as root?\n");
        return false;
    }

    struct dirent* DirEntry;
    while ((DirEntry = readdir(ProcDir)) != nullptr)
    {
        const char* DirName = DirEntry->d_name;
        pid_t Pid           = 0;
        for (const char* C = DirName; *C; ++C)
        {
            if (!isdigit(static_cast<unsigned char>(*C)))
            {
                Pid = -1;
                break;
            }
            Pid = Pid * 10 + (*C - '0');
        }
        if (Pid <= 0)
            continue;

        char CmdlinePath[255];
        snprintf(CmdlinePath, sizeof(CmdlinePath), "/proc/%d/cmdline", Pid);
        FILE* CmdlineFile = fopen(CmdlinePath, "r");
        if (!CmdlineFile)
            continue;

        char CmdlineBuf[512] = {};
        size_t CmdlineLen    = fread(CmdlineBuf, 1, sizeof(CmdlineBuf) - 1, CmdlineFile);
        fclose(CmdlineFile);

        if (CmdlineLen == 0)
            continue;

        CmdlineBuf[CmdlineLen] = '\0';
        std::string ProcName(CmdlineBuf);

        if (!std::isalpha(ProcName[0]))
            continue;
        if (ProcName.find('.') == std::string::npos)
            continue;
        if (ProcName.compare(0, 7, "vendor.") == 0)
            continue;
        if (ProcName.compare(0, 8, "android.") == 0)
            continue;
        if (ProcName.compare(0, 7, "google.") == 0)
            continue;
        if (ProcName.compare(0, 12, "com.android.") == 0)
            continue;
        if (ProcName.compare(0, 11, "com.google.") == 0)
            continue;
        if (ProcName.compare(0, 14, "org.lineageos.") == 0)
            continue;

        if (!fs::exists(KittyUtils::Android::getAppExternalFilesDir(ProcName)))
            continue;

        KittyMemoryEx::ProcStatus Status{};
        KittyMemoryEx::ProcStatus::parse(Pid, &Status);
        std::string State = Status.getString("State");
        if (State.empty() || State[0] == 'Z' || State[0] == 'X')
            continue;

        errno = 0;
        if (kill(Pid, 0) != 0 && errno != EPERM)
            continue;

        UserProcesses.push_back({Pid, ProcName});
    }
    closedir(ProcDir);

    if (UserProcesses.empty())
    {
        GLogger.FmtWrite(ELogLevel::Warning, "No user processes detected. Ensure the target game is running.\n");
        return false;
    }

    std::sort(UserProcesses.begin(), UserProcesses.end(), [](const UserProcessEntry& A, const UserProcessEntry& B)
    { return A.Name < B.Name; });

    std::stringstream LayoutStream;
    LayoutStream << "\n"
                 << COLOR_BOLD << COLOR_CYAN << "┌── User Processes" << COLOR_RESET << "\n"
                 << COLOR_CYAN << "│" << COLOR_RESET << "\n";

    for (size_t i = 0; i < UserProcesses.size(); ++i)
    {
        const std::string AppExternalFiles = KittyUtils::Android::getAppExternalFilesDir(UserProcesses[i].Name);
        const std::string UE4GameFolder    = AppExternalFiles + "/UE4Game";
        const std::string UE5GameFolder    = AppExternalFiles + "/UnrealGame";
        const bool bIsLikelyUEGame         = access(UE4GameFolder.c_str(), F_OK) != -1 || access(UE5GameFolder.c_str(), F_OK) != -1;
        const bool bHasOBB                 = access(KittyUtils::Android::getAppObbDir(UserProcesses[i].Name).c_str(), F_OK) != -1;
        const char* Color                  = bIsLikelyUEGame ? COLOR_GREEN : (bHasOBB ? COLOR_CYAN : COLOR_PURPLE);

        LayoutStream << COLOR_CYAN << "├──" << COLOR_RESET
                     << " ["
                     << std::right
                     << std::setfill('0') << std::setw(2) << (i + 1)
                     << std::setfill(' ') << "]  "
                     << COLOR_BOLD << Color << std::left << std::setw(44)
                     << UserProcesses[i].Name << COLOR_RESET
                     << "  PID: " << COLOR_BOLD << UserProcesses[i].Pid << COLOR_RESET
                     << "\n";
    }

    LayoutStream << COLOR_CYAN << "└──" << COLOR_RESET << " Select index "
                 << COLOR_GRAY << "[1-" << UserProcesses.size() << "]" << COLOR_RESET << ": ";

    std::cout << LayoutStream.str();

    int SelectedIndex = 0;
    if (!ReadIntegerInput(SelectedIndex) || SelectedIndex < 1 || SelectedIndex > static_cast<int>(UserProcesses.size()))
    {
        GLogger.FmtWrite(ELogLevel::Error, "Invalid selection index.\n");
        return false;
    }

    OutPid = UserProcesses[SelectedIndex - 1].Pid;
    OutId  = UserProcesses[SelectedIndex - 1].Name;
    return true;
}

bool ChooseProcessFromMultiple(const std::vector<pid_t>& Pids, pid_t& OutPid)
{
    if (Pids.empty())
    {
        return false;
    }

    if (Pids.size() == 1)
    {
        OutPid = Pids.front();
        return true;
    }

    std::stringstream LayoutStream;
    LayoutStream << "\n"
                 << COLOR_BOLD << COLOR_YELLOW << "┌── Multiple Instances Detected" << COLOR_RESET << "\n"
                 << COLOR_YELLOW << "│" << COLOR_RESET << "\n";

    for (size_t i = 0; i < Pids.size(); ++i)
    {
        LayoutStream << COLOR_YELLOW << "├──" << COLOR_RESET << " [" << COLOR_BOLD << (i + 1) << COLOR_RESET << "] "
                     << "PID: " << COLOR_YELLOW << Pids[i] << COLOR_RESET << "\n";
    }

    LayoutStream << COLOR_YELLOW << "└──" << COLOR_RESET << " Select index "
                 << COLOR_GRAY << "[1-" << Pids.size() << "]" << COLOR_RESET << ": ";

    std::cout << LayoutStream.str();

    int SelectedIndex = 0;
    if (!ReadIntegerInput(SelectedIndex) || SelectedIndex < 1 || SelectedIndex > static_cast<int>(Pids.size()))
    {
        GLogger.FmtWrite(ELogLevel::Error, "Invalid selection index.");
        return false;
    }

    OutPid = Pids[SelectedIndex - 1];
    return true;
}

bool ChooseMemoryAccessType(int& OutMemAccessType)
{
    std::stringstream LayoutStream;
    LayoutStream << "\n"
                 << COLOR_BOLD << COLOR_GREEN << "┌── Memory Access Type" << COLOR_RESET << "\n"
                 << COLOR_GREEN << "│" << COLOR_RESET << "\n"
                 << COLOR_GREEN << "├──" << COLOR_RESET << " [" << COLOR_BOLD << "1" << COLOR_RESET << "] process_vm_readv\n"
                 << COLOR_GREEN << "├──" << COLOR_RESET << " [" << COLOR_BOLD << "2" << COLOR_RESET << "] pread\n"
                 << COLOR_GREEN << "└──" << COLOR_RESET << " Select type "
                 << COLOR_GRAY << "[1-2]" << COLOR_RESET << ": ";

    std::cout << LayoutStream.str();

    int SelectedType = 0;
    if (!ReadIntegerInput(SelectedType) || SelectedType < 1 || SelectedType > 2)
    {
        GLogger.FmtWrite(ELogLevel::Error, "Selected memory access type is out of range.\n");
        return false;
    }

    OutMemAccessType = SelectedType;
    return true;
}

bool ChooseOutputDirectory(std::string& OutOutputDir)
{
    std::stringstream LayoutStream;
    LayoutStream << "\n"
                 << COLOR_BOLD << COLOR_GREEN << "┌── Output Directory" << COLOR_RESET << "\n"
                 << COLOR_GREEN << "│" << COLOR_RESET << "\n"
                 << COLOR_GREEN << "└──" << COLOR_RESET << " Enter path: ";

    std::cout << LayoutStream.str();

    std::getline(std::cin >> std::ws, OutOutputDir);

    if (OutOutputDir.empty())
    {
        GLogger.FmtWrite(ELogLevel::Error, "Output directory path cannot be empty.\n");
        return false;
    }

    return true;
}

#endif

#endif
