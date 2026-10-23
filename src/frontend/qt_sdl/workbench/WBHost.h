/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#ifndef WB_HOST_H
#define WB_HOST_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <set>
#include <memory>
#include <mutex>
#include <QFile>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>
#include <QTimer>

#include "Workbench.h"
#include "Session.h"
#include "KnowledgeStore.h"
#include "GameArchive.h"
#include "Containers.h"
#include "WBApi.h"
#include "WBClassify.h"
#include "WBCompare.h"
#include "WBAssets.h"

class EmuInstance;
class WorkbenchWindow;
class ApiServer;

struct FuncSnap { qint64 Id; melonDS::u32 Addr, Size; bool Thumb; int Cpu; };

struct CollectedSession
{
    std::vector<melonDS::u8> Bytes;
    std::vector<WBKB::EvidenceRow> Evidence;
    std::vector<WBKB::AssetLinkRow> Assets;
    melonDS::u32 FrameStart = 0, FrameEnd = 0, Flags = 0;
    QString Sha, Dir;
    QString Path;           // where the session file goes
    QString HistoryPath;    // its whole write history (.dswbh), when the session file can't hold it all
};

// a saved session's write history, opened for queries (WBHost::OpenSavedHistory)
struct SavedHistory
{
    QString Path;           // the session file
    QString Source;         // history_file or session_file (WritesListResult.history_source)
    melonDS::WB::HistoryView View;
    QFile* File = nullptr;  // a mapped .dswbh
    uchar* Map = nullptr;
    QByteArray Bytes;       // or the decompressed session file
    ~SavedHistory();
};

// Snapshot of everything the indicators show, refreshed by the emulation thread.
struct WBStatus
{
    enum State { NoGame, Running, Paused, Break } EmuState = NoGame;
    melonDS::u32 Frame = 0;
    bool Recording = false, History = false, Origin = false;
    bool Available = true;          // false while the JIT is on
    melonDS::u32 FramesRecorded = 0;
    melonDS::u64 HistoryCount = 0, HistoryCap = 0;
    bool HistoryWrapped = false;    // full: the oldest writes are being dropped
    melonDS::u32 RecordingSince = 0, HistorySince = 0, OriginSince = 0;   // frame each was turned on
    melonDS::u32 StateLoadedFrame = 0;   // last savestate load (data origin was rebuilt by content then), 0 = none
    melonDS::u64 HistorySkipped = 0;
    melonDS::u32 HistoryOldestFrame = 0;
    melonDS::u32 Pages = 0;
    double Fps = 0;
    melonDS::u32 PC[2] = {0, 0};
    bool Thumb[2] = {false, false};
    melonDS::WB::BreakState BreakInfo;
    QString RomSha1, RomTitle, RomCode;
};

struct WBSettings
{
    QString KnowledgeStorePath;     // empty: <melonDS dir>/workbench/<gamecode>_<sha1>/knowledge.db
    QString KnowledgeStoreOverride; // --kb on the command line; never saved
    QString SourceRoot;             // decomp git tree
    QString CompareCommand;         // build compare command template; empty: WBHost::DefaultCompareCommand
    // this machine's compare tools (per game settings are the project's compare.json)
    QString PythonPath;             // empty: python on PATH
    QString ObjdiffPath;            // objdiff-cli
    QStringList Compilers;          // "label=path": the executable for each compiler version label
    int CompareTimeoutSec = 60;
    QString CompareApproved;        // fingerprint of the command and script the person approved
    QString CompareSelfTestOk;      // fingerprint of the toolchain whose self-test passed
    QString EditorCommand;          // "code -g {file}:{line}"
    QString AuthorName;
    quint64 HistoryCap = 4000000;
    quint32 HistoryFileMB = 0;      // write history in a memory-mapped file of this size; 0 = in memory
    quint32 HistoryKeepMB = 2048;   // disk budget for saved sessions' history files (per game)
    bool ApiEnabled = false;
    int ApiPort = 47800;            // read endpoint; control endpoint = port + 1
    bool ApiMemoryWrite = false;
    bool OriginAllRomFiles = true;
    bool OriginFollowAddresses = false;
    bool AutoImportSessions = true;
    // whenever recording starts, also turn these on (Recording panel, Workbench settings)
    bool AlsoHistory = false, AlsoOrigin = false;
};

// What a feature needs before it has data (UI_REQUIREMENTS PR-1). Banners, badges, menus and the
// API all describe prerequisites through WBHost::Missing and WBHost::Covers.
enum class WBNeed { Game, Interpreter, Recording, WriteHistory, DataOrigin, Archive, Containers };

class WBHost : public QObject
{
    Q_OBJECT
public:
    WBHost(EmuInstance* inst);
    ~WBHost();

    melonDS::WB::Workbench& Core() { return WB; }
    WBKB::KnowledgeStore& KB() { return Kb; }
    EmuInstance* Instance() { return Inst; }
    WBSettings& Settings() { return Cfg; }
    void LoadSettings();
    void SaveSettings();

    // ---- emulation thread side (called from EmuThread / EmuInstance)
    void AttachNDS(melonDS::NDS* nds);
    void OnRomLoaded(const QString& path, melonDS::u32 filelen);
    void OnStateLoaded();
    void OnGameClosing();                    // stop / eject / new ROM / exit: closes the recording session
    void Shutdown();                         // main thread, before the emulation thread exits
    void RecordOnBoot(bool history = false, bool origin = false);   // --record, --write-history, --data-origin
    // what is missing before n can run, as a sentence ("Recording is off"); empty when nothing is
    QString Missing(WBNeed n) const;
    // what turning n on records, and from when
    static QString Covers(WBNeed n);
    // the JIT recompiler can't record or break: turn it off in the config and reset the game
    bool TurnOffJitAndReset(QString& err);
    void ApplyInput(melonDS::u32& keymask, bool& touching, melonDS::u16& tx, melonDS::u16& ty, bool& lid);
    bool AfterFrame();                       // true -> the emulator should pause now
    void IdleWait(int ms);
    void UpdateStatus(double fps);

    // ---- any thread
    WBStatus Status() const;
    bool EmuRunning() const;
    void RunOnEmu(std::function<void()> job) { WB.Dbg.PostJob(std::move(job)); }

    // run fn on the emulation thread and deliver the result on ctx's thread
    template <typename R>
    void Query(std::function<R()> fn, QObject* ctx, std::function<void(R)> cb)
    {
        QPointer<QObject> guard(ctx);
        WB.Dbg.PostJob([fn = std::move(fn), cb = std::move(cb), guard]() mutable
        {
            R r = fn();
            if (!guard) return;
            QMetaObject::invokeMethod(guard.data(), [r = std::move(r), cb = std::move(cb)]() { cb(r); }, Qt::QueuedConnection);
        });
    }

    // run fn on a pool thread (work that doesn't touch the emulator, like reading a saved session),
    // then cb on ctx's thread
    template <typename R>
    void Background(std::function<R()> fn, QObject* ctx, std::function<void(R)> cb)
    {
        QPointer<QObject> guard(ctx);
        QThreadPool::globalInstance()->start([fn, cb, guard]()
        {
            R r = fn();
            if (!guard) return;
            QMetaObject::invokeMethod(guard.data(), [r = std::move(r), cb]() { cb(r); }, Qt::QueuedConnection);
        });
    }

    // run fn on the emulation thread and wait (UI thread helpers; times out after timeoutms)
    bool RunSync(std::function<void()> fn, int timeoutms = 3000);

    // ---- control (UI or API, main thread)
    void SetRecording(bool on);
    void SetWriteHistory(bool on);
    void SetDataOrigin(bool on);
    void SetHistoryCap(quint64 cap);
    // keep write history in a file (MB; 0 = memory). Starts history afresh. persist: also the default
    // for later runs (the settings dialog); the API changes only this run
    bool SetHistoryFile(quint32 mb, QString& error, bool persist);
    QString SaveSessionNow();               // saves without stopping recording
    void Pause();
    void Resume();
    void BreakNow(int cpu = 0);
    void Step(int cpu, melonDS::u32 count);
    void StepOver(int cpu);                 // step, or run until the call returns
    void IdentifyFunctions();               // create sub_ functions for entries the recording has seen
    // While paused: follow code the recording already reached and mark what it has not executed.
    // A finished snapshot stays across Resume. Clear unreached marks, the next run, or a new game drops it.
    void RunStaticAnalysis();
    void CancelStaticAnalysis();            // stop the walk; the previous snapshot stays
    void AbandonStaticAnalysis();           // the game resumed while the walk was reading memory
    void ClearStaticAnalysis();
    bool AnalysisRunning() const { return AnalysisBusy; }
    bool HasAnalysis() const { return !Analysis[0].empty() || !Analysis[1].empty(); }
    WBQ::AnalysisHints AnalysisNear(int cpu, melonDS::u32 start, melonDS::u32 end) const;
    const std::vector<WBQ::AnalysisHints::Range>& AnalysisAll(int cpu) const;
    bool FunctionOpen(int cpu, melonDS::u32 addr) const;
    bool FunctionNotched(int cpu, melonDS::u32 addr) const;     // this address is an entry inside a function the person owns
    bool FunctionHasNotch(int cpu, melonDS::u32 funcAddr) const;
    void RunFrames(melonDS::u32 n, std::function<void(bool interrupted)> done);
    void SendInput(melonDS::u32 keymask, bool touch, melonDS::u16 x, melonDS::u16 y, bool lid, melonDS::u32 frames, std::function<void(bool overridden, bool interrupted)> done);
    bool Boot(const QString& path, QString& error);
    bool Reset(QString& error);
    bool SaveState(const QString& path, QString& error);
    bool LoadState(const QString& path, QString& error);
    QString SavestatePath(int slot);

    // build and compare (Source and diff panel's Compare, kb.build.compare, Build > Compare setup)
    QString FunctionSourcePath(const WBKB::Function& f) const;
    static QString DefaultCompareCommand();
    QString CompareCommandTemplate() const;     // the setting, or the default
    QString CompareScriptPath() const;          // <project>/compare/compare.py
    QString CompareConfigPath() const;          // <project>/compare.json
    static QByteArray CompareScriptTemplate();  // the copy this build ships
    // first setup: the script and compare.json go into the project folder (never overwritten)
    bool EnsureCompareProject(QString& error);
    bool LoadCompareConfig(WBCmp::CompareConfig& c, QString& error) const;
    QString CompilerPath(const QString& label) const;
    void SetCompilerPath(const QString& label, const QString& path);
    // everything one Compare needs: the expanded command, and the target object and context header
    // it reads (written under <project>/compare/run/<id>/ when writeInputs)
    struct CompareJob
    {
        qint64 FunctionId = 0;
        QString Command, WorkDir, Symbol, TargetPath, ContextPath, CompilerLabel, Flags;
        QString Fingerprint;    // of the command template and the script
        bool SelfTest = false;
    };
    bool PrepareCompare(qint64 functionId, CompareJob& job, QString& error, bool writeInputs = true);
    bool PrepareSelfTest(qint64 functionId, CompareJob& job, QString& error);
    bool CompareApproved(const CompareJob& job) const { return job.Fingerprint == Cfg.CompareApproved; }
    void ApproveCompare(const CompareJob& job);
    struct CompareRun
    {
        bool Ok = false;            // the command answered with a result (matched or not)
        bool Recorded = false;
        QJsonObject Result;
        QStringList ContractErrors;
        QString Error, Explanation, Stdout, Stderr;
        int ExitCode = -1;
        bool TimedOut = false;
    };
    // runs a prepared job; with record, the result is recorded as `author` (0 = the person) unless
    // the tool recorded it. done runs on the main thread
    void RunCompare(const CompareJob& job, bool record, qint64 author, const QString& agentSession,
                    std::function<void(const CompareRun&)> done);
    // prepare and run, for the Source panel and kb.build.compare (refuses a command not yet approved)
    void CompareFunction(qint64 functionId, bool record, qint64 author, const QString& agentSession,
                         std::function<void(const CompareRun&)> done);
    struct SetupRow
    {
        QString Id, Label, Message, Fix;     // Fix: what the row's button does (python, compiler, ...)
        bool Ok = false, Optional = false;
    };
    // the setup checklist (Source panel, build.setup.check): probes run on a pool thread
    void CheckCompareSetup(qint64 functionId, QObject* ctx, std::function<void(std::vector<SetupRow>)> done);

    // project files
    QString ProjectDir() const;
    QString SessionsDir() const;
    void LoadAllTimeSessions();             // background merge of saved sessions for the "all time" scope
    melonDS::WB::MergedSessions* AllTime() { return AllTimeData.get(); }   // emulation thread only

    // game archive resolver (TrackMania DS's vfs.zip), built in the background when a game loads
    std::shared_ptr<const WBArc::ArchiveIndex> Archive() const;
    // plain ROM files and their unpacked forms (null until indexed)
    std::shared_ptr<const WBArc::RomFileIndex> RomFiles() const;
    QString ArchiveStatus() const;          // none, indexing, ready, failed: <reason>
    // NARC and SDAT containers in NitroFS (null until indexed, or when there are none)
    std::shared_ptr<const WBArc::ContainerIndex> Containers() const;
    QString ContainerStatus() const;        // none, indexing, ready
    // graphics files in the ROM, containers and the archive (null until indexed)
    std::shared_ptr<const WBAsset::Catalog> Graphics() const;
    QString GraphicsStatus() const;         // none, indexing, ready

    // UI
    WorkbenchWindow* Window(bool create = true);
    void ShowWindow();
    QString TitlePrefix() const;
    ApiServer* Api() { return Server; }
    void StartApi(int port, bool memwrite);
    int OverlayLoadedAt(melonDS::u32 addr) const;   // recorder's overlay owner (cpu 0), -1 = static/unknown
    QString RomPath() const { return CurRomPath; }
    QString LastSession() const { return LastSessionPath; }
    // saved sessions of this game, newest first (main thread)
    std::vector<WBApi::SessionInfo> ListSessions() const;
    // a session file name from ListSessions (or a path) as a path (main thread)
    QString ResolveSession(const QString& name) const;
    // its write history: the .dswbh when there is one, else what the session file keeps (any thread;
    // the last one opened is kept open)
    std::shared_ptr<const SavedHistory> OpenSavedHistory(const QString& path, QString& error);

signals:
    void statusChanged();
    void breakChanged(bool inBreak);
    void breakpointHit(int id, int cpu, quint32 pc);
    void recordingChanged();
    void overlayLoaded(int cpu, int id);
    void framesDone(bool interrupted);
    void inputDone(bool overridden);
    void knowledgeChanged();
    void analysisChanged();
    // cpu 0 ARM9, 1 ARM7; instructions kept so far in the current CPU's walk
    void analysisProgress(int cpu, int instructions);
    // a finished run. noteCpu -1 when nothing was left unmarked. firstInsn is 0 when that CPU marked none
    void analysisFinished(QString text, int noteCpu, quint32 noteAddr, quint32 firstInsn0, quint32 firstInsn1);
    void sessionSaved(QString path);
    void romChanged();
    void archiveChanged();
    void containersChanged();
    void graphicsChanged();
    void pausedChanged(bool paused);
    // breakpoints were added, removed or changed outside the emulation (the API, a panel)
    void breakpointsChanged();
    // a state change the user didn't make directly ("Recording started so that write history can run")
    void notice(const QString& text);

private slots:
    void PollKB();

private:
    void OnCoreEvent(const melonDS::WB::WBEvent& ev);
    void FinishSession(bool keepRecording);
    bool CollectSession(bool keepRecording, CollectedSession& out);
    QString StoreSession(const CollectedSession& s);
    // whole session file, decompressed
    static QByteArray ReadSessionFile(const QString& path);
    void ImportCollected(const QString& path, const CollectedSession& s);
    // emulation thread, as a session is saved: its whole write history goes next to the session file,
    // moved when the session ends (move), copied when it goes on
    QString KeepSessionHistory(const CollectedSession& s, bool move);
    // the history files over the disk budget go, oldest first, never `keep`
    void PruneHistoryFiles(const QString& dir, const QString& keep);
    void RefreshFuncSnapshot();
    void OpenKnowledgeStore();
    void StartRecordingNow();               // emulation thread
    void StartArchiveIndex();               // emulation thread, after the ROM is parsed
    bool ApplyHistoryFile(QString* error);  // emulation thread: map or unmap the history file per Cfg.HistoryFileMB
    void SeedArchiveAssets();               // main thread: archived files become knowledge store assets
    void SeedContainerAssets();             // main thread: so do the files inside containers
    void InstallArchiveResolver();          // main thread: data origin labels name files inside the archive
    void StartGraphicsCatalog();            // main thread, after the archive or containers changed

    bool IdentBusy = false;
    bool AnalysisBusy = false;
    std::atomic<bool> AnalysisCancel {false};
    std::atomic<melonDS::u32> AnalysisGen {0};
    std::vector<WBQ::AnalysisHints::Range> Analysis[2];
    // emu thread: the walk reports each committed batch. Dropped when this run was cancelled.
    void ReportAnalysisProgress(melonDS::u32 gen, int cpu, int instructions);
    EmuInstance* Inst;
    melonDS::WB::Workbench WB;
    WBKB::KnowledgeStore Kb;
    WBSettings Cfg;
    QString CurRomPath;
    QPointer<WorkbenchWindow> Win;
    ApiServer* Server = nullptr;
    QTimer KbTimer;

    mutable std::mutex StatusLock;
    WBStatus Cur;

    // agent input / frame running (emulation thread state guarded by InputLock)
    std::mutex InputLock;
    melonDS::u32 AgentKeys = 0xFFF;
    bool AgentTouch = false, AgentLid = false;
    melonDS::u16 AgentX = 0, AgentY = 0;
    melonDS::u32 AgentFrames = 0;
    bool AgentOverridden = false;
    std::function<void(bool, bool)> InputDoneCb;
    bool AgentStarted = false;      // a frame has run with the agent's input
    melonDS::u32 FramesToRun = 0;
    bool FramesActive = false;
    bool FramesInterrupted = false;
    std::function<void(bool)> FramesDoneCb;
    bool PauseAfterInput = false;

    std::mutex SnapLock;
    std::vector<FuncSnap> FuncSnapshot;
    QString ProjectDirCache;
    QString LastSessionPath;
    bool RecordAtBoot = false;
    bool HistoryAtBoot = false, OriginAtBoot = false;
    std::atomic<melonDS::u32> RecordingSince {0}, HistorySince {0}, OriginSince {0};
    bool WrapNoticed = false;
    bool HistoryFileOverride = false;       // the API set a history file for this run only
    quint32 HistoryFileSaved = 0;

    std::shared_ptr<melonDS::WB::MergedSessions> AllTimeData;
    std::atomic<bool> AllTimeLoading {false};

    // shared with the indexing thread, which may outlive this object
    struct ArcShared
    {
        std::mutex Lock;
        std::shared_ptr<const WBArc::ArchiveIndex> Index;
        std::shared_ptr<const WBArc::RomFileIndex> Files;
        QString Status = "none";
        std::shared_ptr<const WBArc::ContainerIndex> Containers;
        QString ContainerStatus = "none";
        std::shared_ptr<std::vector<WBAsset::Catalog::RomInput>> GfxRom;   // NitroFS files that may be graphics
        std::shared_ptr<const WBAsset::Catalog> Gfx;
        QString GfxStatus = "none";
        melonDS::u32 Generation = 0;
        WBHost* Host = nullptr;
    };
    std::shared_ptr<ArcShared> Arc;

    // the mapped history file (emulation thread only)
    static quint64 MarkKey(int cpu, melonDS::u32 addr) { return ((quint64)(cpu & 1) << 32) | addr; }
    std::set<quint64> OpenMarks, NotchAt, NotchedFunc;

    QFile* HistFile = nullptr;
    uchar* HistMap = nullptr;
    quint32 HistMapMB = 0;

    std::mutex SavedLock;
    std::shared_ptr<const SavedHistory> SavedCache;
};

#endif
