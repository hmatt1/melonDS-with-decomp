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

#include <algorithm>
#include <condition_variable>
#include <map>
#include <thread>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>

#include "WBHost.h"
#include "WorkbenchWindow.h"
#include "Panels.h"
#include "ApiServer.h"
#include "EmuInstance.h"
#include "EmuThread.h"
#include "Config.h"
#include "main.h"
#include "NDS.h"
#include "NDSCart.h"
#include "ARM.h"
#include "Disasm.h"
#include "WBSpans.h"
#include "WBQueries.h"

using namespace melonDS;
using namespace melonDS::WB;

WBHost::WBHost(EmuInstance* inst) : QObject(nullptr), Inst(inst)
{
    Arc = std::make_shared<ArcShared>();
    Arc->Host = this;
    connect(this, &WBHost::archiveChanged, this, &WBHost::SeedArchiveAssets);
    connect(this, &WBHost::archiveChanged, this, &WBHost::InstallArchiveResolver);
    connect(this, &WBHost::containersChanged, this, &WBHost::SeedContainerAssets);
    connect(this, &WBHost::containersChanged, this, &WBHost::InstallArchiveResolver);
    connect(this, &WBHost::containersChanged, this, &WBHost::StartGraphicsCatalog);
    connect(this, &WBHost::archiveChanged, this, &WBHost::StartGraphicsCatalog);
    LoadSettings();
    WB.Rec.SetHistoryCap(Cfg.HistoryCap);

    WB.OnEvent = [this](const WBEvent& ev) { OnCoreEvent(ev); };
    WB.Dbg.HostWantsAbort = [this]()
    {
        EmuThread* t = Inst->getEmuThread();
        return t ? t->serviceMessagesInBreak() : false;
    };

    connect(&Kb, &WBKB::KnowledgeStore::changed, this, &WBHost::knowledgeChanged);
    connect(this, &WBHost::knowledgeChanged, this, &WBHost::RefreshFuncSnapshot);
    if (EmuThread* t = Inst->getEmuThread())
    {
        connect(t, &EmuThread::windowEmuPause, this, [this](bool paused) { emit pausedChanged(paused); emit statusChanged(); });
        connect(t, &EmuThread::windowEmuStart, this, [this]() { emit pausedChanged(false); emit statusChanged(); });
        connect(t, &EmuThread::windowEmuStop, this, [this]() { emit pausedChanged(true); emit statusChanged(); });
    }
    KbTimer.setInterval(1000);
    connect(&KbTimer, &QTimer::timeout, this, &WBHost::PollKB);
    KbTimer.start();

    Server = new ApiServer(this, this);
}

WBHost::~WBHost()
{
    if (HistFile)
    {
        WB.Rec.SetHistoryStorage(nullptr, 0);
        if (HistMap) HistFile->unmap(HistMap);
        delete HistFile;
    }
    {
        std::lock_guard<std::mutex> l(Arc->Lock);
        Arc->Host = nullptr;
    }
    if (Win) delete Win.data();
    delete Server;
    Server = nullptr;
    Kb.Close();
    WB.OnEvent = nullptr;
    WB.Dbg.HostWantsAbort = nullptr;
}

void WBHost::LoadSettings()
{
    Config::Table cfg = Config::GetGlobalTable();
    Cfg.KnowledgeStorePath = cfg.GetQString("Workbench.KnowledgeStorePath");
    Cfg.SourceRoot = cfg.GetQString("Workbench.SourceRoot");
    Cfg.CompareCommand = cfg.GetQString("Workbench.CompareCommand");
    Cfg.PythonPath = cfg.GetQString("Workbench.PythonPath");
    Cfg.ObjdiffPath = cfg.GetQString("Workbench.ObjdiffPath");
    Cfg.Compilers.clear();
    Config::Array comps = cfg.GetArray("Workbench.Compilers");
    for (size_t i = 0; i < comps.Size(); i++)
        if (!comps.GetQString((int)i).isEmpty()) Cfg.Compilers << comps.GetQString((int)i);
    int cto = cfg.GetInt("Workbench.CompareTimeoutSec");
    if (cto > 0) Cfg.CompareTimeoutSec = cto;
    Cfg.CompareApproved = cfg.GetQString("Workbench.CompareApproved");
    Cfg.CompareSelfTestOk = cfg.GetQString("Workbench.CompareSelfTestOk");
    Cfg.EditorCommand = cfg.GetQString("Workbench.EditorCommand");
    Cfg.AuthorName = cfg.GetQString("Workbench.AuthorName");
    if (Cfg.AuthorName.isEmpty())
    {
        QString user = qEnvironmentVariable("USERNAME", qEnvironmentVariable("USER", "person"));
        Cfg.AuthorName = user;
    }
    int capm = cfg.GetInt("Workbench.HistoryCapEntries");
    if (capm > 0) Cfg.HistoryCap = (quint64)capm;
    int fmb = cfg.GetInt("Workbench.HistoryFileMB");
    Cfg.HistoryFileMB = fmb > 0 ? (quint32)fmb : 0;
    int kmb = cfg.GetInt("Workbench.HistoryKeepMB");
    if (kmb > 0) Cfg.HistoryKeepMB = (quint32)kmb;
    Cfg.ApiEnabled = cfg.GetBool("Workbench.ApiEnabled");
    int port = cfg.GetInt("Workbench.ApiPort");
    if (port > 0 && port < 65535) Cfg.ApiPort = port;
    Cfg.ApiMemoryWrite = cfg.GetBool("Workbench.ApiMemoryWrite");
    Cfg.OriginAllRomFiles = !cfg.GetBool("Workbench.OriginOnlyChosenSources");
    Cfg.OriginFollowAddresses = cfg.GetBool("Workbench.OriginFollowAddresses");
    Cfg.AutoImportSessions = !cfg.GetBool("Workbench.NoSessionImport");
    Cfg.AlsoHistory = cfg.GetBool("Workbench.RecordAlsoHistory");
    Cfg.AlsoOrigin = cfg.GetBool("Workbench.RecordAlsoOrigin");
}

void WBHost::SaveSettings()
{
    Config::Table cfg = Config::GetGlobalTable();
    cfg.SetQString("Workbench.KnowledgeStorePath", Cfg.KnowledgeStorePath);
    cfg.SetQString("Workbench.SourceRoot", Cfg.SourceRoot);
    cfg.SetQString("Workbench.CompareCommand", Cfg.CompareCommand);
    cfg.SetQString("Workbench.PythonPath", Cfg.PythonPath);
    cfg.SetQString("Workbench.ObjdiffPath", Cfg.ObjdiffPath);
    Config::Array comps = cfg.GetArray("Workbench.Compilers");
    comps.Clear();
    for (int i = 0; i < Cfg.Compilers.size(); i++) comps.SetQString(i, Cfg.Compilers[i]);
    cfg.SetInt("Workbench.CompareTimeoutSec", Cfg.CompareTimeoutSec);
    cfg.SetQString("Workbench.CompareApproved", Cfg.CompareApproved);
    cfg.SetQString("Workbench.CompareSelfTestOk", Cfg.CompareSelfTestOk);
    cfg.SetQString("Workbench.EditorCommand", Cfg.EditorCommand);
    cfg.SetQString("Workbench.AuthorName", Cfg.AuthorName);
    cfg.SetInt("Workbench.HistoryCapEntries", (int)std::min<quint64>(Cfg.HistoryCap, 0x7FFFFFFF));
    cfg.SetInt("Workbench.HistoryFileMB", (int)(HistoryFileOverride ? HistoryFileSaved : Cfg.HistoryFileMB));
    cfg.SetInt("Workbench.HistoryKeepMB", (int)Cfg.HistoryKeepMB);
    cfg.SetBool("Workbench.ApiEnabled", Cfg.ApiEnabled);
    cfg.SetInt("Workbench.ApiPort", Cfg.ApiPort);
    cfg.SetBool("Workbench.ApiMemoryWrite", Cfg.ApiMemoryWrite);
    cfg.SetBool("Workbench.OriginOnlyChosenSources", !Cfg.OriginAllRomFiles);
    cfg.SetBool("Workbench.OriginFollowAddresses", Cfg.OriginFollowAddresses);
    cfg.SetBool("Workbench.NoSessionImport", !Cfg.AutoImportSessions);
    cfg.SetBool("Workbench.RecordAlsoHistory", Cfg.AlsoHistory);
    cfg.SetBool("Workbench.RecordAlsoOrigin", Cfg.AlsoOrigin);
    Config::Save();
}

// ---------------------------------------------------------------- emulation thread side

void WBHost::AttachNDS(NDS* nds)
{
    WB.Attach(nds);
}

void WBHost::OnRomLoaded(const QString& path, u32 filelen)
{
    NDS* nds = WB.GetNDS();
    if (!nds || !nds->GetNDSCart()) return;
    // a new game closes the running session
    if (WB.Rec.Enabled())
    {
        WB.Rec.SetEnabled(false);
        WB.Taint.Enable(false);
    }
    const NDSCart::CartCommon* cart = nds->GetNDSCart();
    WB.OnRomLoaded(cart->GetROM(), cart->GetROMLength(), filelen);
    WB.Taint.SetAllRomFiles(Cfg.OriginAllRomFiles);
    ApplyHistoryFile(nullptr);
    StartArchiveIndex();
    if (RecordAtBoot)
    {
        if (WB.JITActive())
        {
            QMetaObject::invokeMethod(this, [this]()
            {
                emit notice("Recording stays off because the JIT recompiler is on. Switch to the interpreter in Config > Emu settings (the game resets), then turn Recording on from the Recording menu.");
            }, Qt::QueuedConnection);
        }
        else StartRecordingNow();
    }
    {
        std::lock_guard<std::mutex> l(StatusLock);
        Cur.RomSha1 = QString::fromStdString(WB.Rom.Sha1Hex());
        Cur.RomTitle = QString::fromLatin1(WB.Rom.Title).trimmed();
        Cur.RomCode = QString::fromLatin1(WB.Rom.GameCode);
    }
    QMetaObject::invokeMethod(this, [this, path]()
    {
        CurRomPath = path;
        AllTimeData.reset();
        OpenKnowledgeStore();
        emit romChanged();
        emit recordingChanged();
    }, Qt::QueuedConnection);
}

bool WBHost::ApplyHistoryFile(QString* error)
{
    quint32 want = Cfg.HistoryFileMB;
    if (want == HistMapMB && (want == 0 || HistMap)) return true;
    // drop the current mapping first: the recorder must never see an unmapped buffer
    WB.Rec.SetHistoryStorage(nullptr, 0);
    WB.Rec.SetHistoryCap(Cfg.HistoryCap);
    if (HistFile)
    {
        if (HistMap) HistFile->unmap(HistMap);
        HistFile->close();
        if (!want) HistFile->remove();      // back in memory: free the disk space
        delete HistFile;
        HistFile = nullptr;
        HistMap = nullptr;
    }
    HistMapMB = 0;
    if (!want) return true;
    QString dir = QDir(emuDirectory).filePath("workbench");
    QDir().mkpath(dir);
    auto fail = [&](const QString& e)
    {
        if (error) *error = e;
        Inst->osdAddMessage(0xFFA0A0, "Write history file: %s", e.toUtf8().constData());
        if (HistFile) { HistFile->close(); delete HistFile; HistFile = nullptr; }
        return false;
    };
    HistFile = new QFile(QDir(dir).filePath("history.dswbh"));
    if (!HistFile->open(QIODevice::ReadWrite)) return fail("cannot open " + HistFile->fileName() + ": " + HistFile->errorString());
    qint64 bytes = (qint64)want << 20;
    if (!HistFile->resize(bytes)) return fail(QString("cannot make a %1 MB file: %2").arg(want).arg(HistFile->errorString()));
    HistMap = HistFile->map(0, bytes);
    if (!HistMap) return fail("cannot map " + HistFile->fileName() + ": " + HistFile->errorString());
    WB.Rec.SetHistoryStorage((WriteHistoryEntry*)HistMap, (u64)bytes / sizeof(WriteHistoryEntry));
    HistMapMB = want;
    return true;
}

bool WBHost::SetHistoryFile(quint32 mb, QString& error, bool persist)
{
    quint32 saved = Cfg.HistoryFileMB;
    Cfg.HistoryFileMB = mb;
    if (persist) SaveSettings();
    else
    {
        // SaveSettings is also called for other changes: keep the saved default as it was
        HistoryFileSaved = saved;
        HistoryFileOverride = true;
    }
    bool ok = true;
    QString e;
    if (!RunSync([this, &ok, &e]() { ok = ApplyHistoryFile(&e); }, 30000))
        return true;                // applied when the emulation thread next runs jobs (or the next game loads)
    error = e;
    return ok;
}

void WBHost::StartArchiveIndex()
{
    u32 zipId, binId, datId;
    u32 gen;
    bool found = WBArc::ArchiveIndex::DetectVfs(WB.Rom, zipId, binId, datId);
    WB.SubFile = nullptr;
    WB.SubFileName = nullptr;
    WB.SubFileResolve = nullptr;
    WB.FileIndexed = nullptr;
    WB.FilePackedMembers = nullptr;
    // NARC and SDAT files, and files that may be one packed whole
    auto containers = std::make_shared<std::vector<WBArc::ContainerIndex::Input>>();
    {
        const u8* rom = WB.Rom.Data();
        for (const RomFile& f : WB.Rom.Files)
            if (f.Id < 0xF000 && f.OverlayId < 0 && !f.Path.empty() && f.Path[0] == '/' && !(found && f.Id == zipId) && f.End > f.Start &&
                WBArc::ContainerIndex::Candidate(rom + f.Start, f.End - f.Start))
                containers->push_back({f.Id, f.Path, f.Start, std::vector<u8>(rom + f.Start, rom + f.End)});
    }
    {
        std::lock_guard<std::mutex> l(Arc->Lock);
        gen = ++Arc->Generation;
        Arc->Index.reset();
        Arc->Files.reset();
        Arc->Status = found ? "indexing" : "none";
        Arc->Containers.reset();
        Arc->ContainerStatus = containers->empty() ? "none" : "indexing";
        // NitroFS files that are graphics, or packed and might unpack to graphics
        auto gfx = std::make_shared<std::vector<WBAsset::Catalog::RomInput>>();
        const u8* rom = WB.Rom.Data();
        for (const RomFile& f : WB.Rom.Files)
            if (f.Id < 0xF000 && f.OverlayId < 0 && !f.Path.empty() && f.Path[0] == '/' && !(found && f.Id == zipId) && f.End > f.Start &&
                WBAsset::Catalog::Candidate(rom + f.Start, f.End - f.Start))
                gfx->push_back({f.Id, f.Path, std::vector<u8>(rom + f.Start, rom + f.End)});
        Arc->GfxRom = gfx;
        Arc->Gfx.reset();
        Arc->GfxStatus = "indexing";
    }
    if (!containers->empty())
    {
        std::shared_ptr<ArcShared> shared = Arc;
        std::thread([shared, gen, containers]()
        {
            auto idx = std::make_shared<WBArc::ContainerIndex>();
            idx->Build(std::move(*containers));
            std::lock_guard<std::mutex> l(shared->Lock);
            if (gen != shared->Generation || !shared->Host) return;
            if (!idx->Containers().empty()) shared->Containers = idx;
            shared->ContainerStatus = idx->Containers().empty() ? "none" : "ready";
            WBHost* h = shared->Host;
            QMetaObject::invokeMethod(h, [h]() { emit h->containersChanged(); }, Qt::QueuedConnection);
        }).detach();
    }
    else QMetaObject::invokeMethod(this, [this]() { emit containersChanged(); }, Qt::QueuedConnection);
    {
        // the plain NitroFS files and their unpacked forms, for content labelling
        auto files = std::make_shared<std::vector<WBArc::RomFileIndex::Input>>();
        const u8* rom = WB.Rom.Data();
        for (const RomFile& f : WB.Rom.Files)
            if (f.Id < 0xF000 && f.OverlayId < 0 && !f.Path.empty() && f.Path[0] == '/' && !(found && f.Id == zipId) &&
                f.End > f.Start && f.End - f.Start <= WBArc::RomFileIndex().MaxFileSize)
                files->push_back({f.Id, f.Path, std::vector<u8>(rom + f.Start, rom + f.End)});
        // the static modules as they are: the game copies parts of them (autoload, relocation)
        auto module = [&](u32 id, const char* path, u32 off, u32 size)
        {
            if (size && (u64)off + size <= WB.Rom.Length()) files->push_back({id, path, std::vector<u8>(rom + off, rom + off + size), true});
        };
        module(FileARM9, "arm9.bin", WB.Rom.ARM9RomOffset, WB.Rom.ARM9Size);
        module(FileARM7, "arm7.bin", WB.Rom.ARM7RomOffset, WB.Rom.ARM7Size);
        std::shared_ptr<ArcShared> shared = Arc;
        std::thread([shared, gen, files]()
        {
            auto idx = std::make_shared<WBArc::RomFileIndex>();
            idx->Build(std::move(*files));
            std::lock_guard<std::mutex> l(shared->Lock);
            if (gen != shared->Generation || !shared->Host) return;
            shared->Files = idx;
            WBHost* h = shared->Host;
            QMetaObject::invokeMethod(h, [h]() { h->InstallArchiveResolver(); }, Qt::QueuedConnection);
        }).detach();
    }
    if (!found)
    {
        QMetaObject::invokeMethod(this, [this]() { emit archiveChanged(); }, Qt::QueuedConnection);
        return;
    }
    const RomFile* zf = WB.Rom.FileById(zipId);
    const RomFile* bf = WB.Rom.FileById(binId);
    const RomFile* df = WB.Rom.FileById(datId);
    const u8* rom = WB.Rom.Data();
    // the ROM buffer belongs to the cart: copy what the indexer needs
    auto zip = std::make_shared<std::vector<u8>>(rom + zf->Start, rom + zf->End);
    auto bin = std::make_shared<std::vector<u8>>(rom + bf->Start, rom + bf->End);
    auto dat = std::make_shared<std::vector<u8>>(rom + df->Start, rom + df->End);
    auto idx = std::make_shared<WBArc::ArchiveIndex>();
    idx->ArchivePath = zf->Path;
    idx->IndexPath = bf->Path;
    idx->ArchiveFileId = zf->Id;
    idx->ArchiveRomStart = zf->Start;
    std::shared_ptr<ArcShared> shared = Arc;
    std::thread([shared, gen, zip, bin, dat, idx]()
    {
        std::string err;
        bool ok = idx->Build(zip->data(), (u32)zip->size(), bin->data(), (u32)bin->size(), dat->data(), (u32)dat->size(), err);
        std::lock_guard<std::mutex> l(shared->Lock);
        if (gen != shared->Generation || !shared->Host) return;
        if (ok) { shared->Index = idx; shared->Status = "ready"; }
        else shared->Status = "failed: " + QString::fromStdString(err);
        WBHost* h = shared->Host;
        QMetaObject::invokeMethod(h, [h]() { emit h->archiveChanged(); }, Qt::QueuedConnection);
    }).detach();
}

QString WBHost::FunctionSourcePath(const WBKB::Function& f) const
{
    if (f.SourceFile.isEmpty()) return QString();
    return QFileInfo(f.SourceFile).isAbsolute() ? f.SourceFile : QDir(Cfg.SourceRoot).filePath(f.SourceFile);
}

QString WBHost::DefaultCompareCommand()
{
    return "\"{python}\" \"{script}\" --file \"{file}\" --symbol {name} --target \"{target}\" --context \"{context}\" "
           "--compiler \"{compiler}\" --compiler-label \"{compiler_label}\" --flags \"{flags}\" --objdiff \"{objdiff}\" --timeout {timeout}";
}

QString WBHost::CompareCommandTemplate() const
{
    return Cfg.CompareCommand.trimmed().isEmpty() ? DefaultCompareCommand() : Cfg.CompareCommand;
}

QString WBHost::CompareScriptPath() const { return QDir(ProjectDir()).filePath("compare/compare.py"); }
QString WBHost::CompareConfigPath() const { return QDir(ProjectDir()).filePath("compare.json"); }

QByteArray WBHost::CompareScriptTemplate()
{
    QFile f(":/compare/compare.py");
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

bool WBHost::EnsureCompareProject(QString& error)
{
    QString script = CompareScriptPath();
    if (!QFileInfo::exists(script))
    {
        QDir().mkpath(QFileInfo(script).absolutePath());
        QFile f(script);
        if (!f.open(QIODevice::WriteOnly) || f.write(CompareScriptTemplate()) < 0)
        {
            error = "cannot write " + script;
            return false;
        }
    }
    QString cfg = CompareConfigPath();
    if (!QFileInfo::exists(cfg))
    {
        QString first;
        for (const WBKB::Module& m : Kb.Modules())
            if (m.Cpu == "arm9") { first = m.Name; break; }
        QFile f(cfg);
        if (!f.open(QIODevice::WriteOnly) || f.write(WBCmp::CompareConfig::Template(first)) < 0)
        {
            error = "cannot write " + cfg;
            return false;
        }
    }
    return true;
}

bool WBHost::LoadCompareConfig(WBCmp::CompareConfig& c, QString& error) const
{
    QFile f(CompareConfigPath());
    if (!f.exists()) { c = WBCmp::CompareConfig(); return true; }
    if (!f.open(QIODevice::ReadOnly)) { error = "cannot read " + f.fileName(); return false; }
    return c.Parse(f.readAll(), error);
}

QString WBHost::CompilerPath(const QString& label) const
{
    for (const QString& e : Cfg.Compilers)
    {
        int eq = e.indexOf('=');
        if (eq > 0 && e.left(eq).trimmed() == label) return e.mid(eq + 1).trimmed();
    }
    return QString();
}

void WBHost::SetCompilerPath(const QString& label, const QString& path)
{
    for (int i = Cfg.Compilers.size() - 1; i >= 0; i--)
        if (Cfg.Compilers[i].section('=', 0, 0).trimmed() == label) Cfg.Compilers.removeAt(i);
    if (!path.isEmpty()) Cfg.Compilers << label + "=" + path;
}

void WBHost::ApproveCompare(const CompareJob& job)
{
    Cfg.CompareApproved = job.Fingerprint;
    SaveSettings();
}

static QString ScriptFingerprint(const QString& templ, const QString& scriptPath)
{
    QFile f(scriptPath);
    QByteArray script = f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    return WBCmp::CommandFingerprint(templ, script);
}

static QString Expand(QString cmd, const std::map<QString, QString>& vars)
{
    for (auto& [k, v] : vars) cmd.replace("{" + k + "}", v);
    return cmd;
}

bool WBHost::PrepareCompare(qint64 functionId, CompareJob& job, QString& error, bool writeInputs)
{
    const WBKB::Function* f = Kb.FunctionById(functionId);
    if (!f) { error = "no such function"; return false; }
    const WBKB::Module* m = Kb.ModuleById(f->ModuleId);
    int cpu = m ? m->CpuNum() : 0;
    int ov = cpu == 0 ? OverlayLoadedAt(f->Addr) : -1;
    QString name = Kb.NameAt(cpu, f->Addr, ov);
    WBCmp::CompareConfig cc;
    if (!LoadCompareConfig(cc, error)) return false;
    WBCmp::CompilerChoice choice = cc.Resolve(m ? m->Name : QString(), f->Addr);
    QString dir = QDir(ProjectDir()).filePath(QString("compare/run/%1").arg(f->Id));
    job = CompareJob();
    job.FunctionId = f->Id;
    job.WorkDir = Cfg.SourceRoot.isEmpty() ? ProjectDir() : Cfg.SourceRoot;
    job.Symbol = name;
    job.TargetPath = QDir(dir).filePath("target.o");
    job.ContextPath = choice.Context ? QDir(dir).filePath("context.h") : QString();
    job.CompilerLabel = choice.Label;
    job.Flags = choice.Flags;
    QString compiler = CompilerPath(choice.Label);
    if (compiler.isEmpty() && QFileInfo::exists(choice.Label)) compiler = choice.Label;
    QString templ = CompareCommandTemplate();
    job.Command = Expand(templ, {
        {"addr", QString("0x%1").arg(f->Addr, 8, 16, QChar('0'))},
        {"offset", QString("0x%1").arg(f->Offset, 0, 16)},
        {"module", m ? m->Name : QString()},
        {"name", name},
        {"file", FunctionSourcePath(*f)},
        {"source_root", Cfg.SourceRoot},
        {"kb", Kb.Path()},
        {"function_id", QString::number(f->Id)},
        {"project", ProjectDir()},
        {"python", Cfg.PythonPath.isEmpty() ? QString("python") : Cfg.PythonPath},
        {"script", CompareScriptPath()},
        {"target", job.TargetPath},
        {"context", job.ContextPath},
        {"compiler", compiler},
        {"compiler_label", choice.Label},
        {"flags", choice.Flags},
        {"objdiff", Cfg.ObjdiffPath.isEmpty() ? QString("objdiff-cli") : Cfg.ObjdiffPath},
        {"timeout", QString::number(Cfg.CompareTimeoutSec)},
    });
    job.Fingerprint = ScriptFingerprint(templ, CompareScriptPath());
    if (!writeInputs) return true;

    // the original bytes: from RAM, or from the ROM while another overlay holds the function's place.
    // The other functions in the same C file are read too: the context header covers the whole file
    std::vector<const WBKB::Function*> own = {f};
    if (!f->SourceFile.isEmpty())
        for (const WBKB::Function& g : Kb.Functions())
            if (g.Id != f->Id && g.SourceFile == f->SourceFile && own.size() < 256) own.push_back(&g);
    std::vector<std::vector<u8>> bytes(own.size());
    QString readErr;
    bool ran = RunSync([&]()
    {
        for (size_t i = 0; i < own.size(); i++)
        {
            const WBKB::Function* g = own[i];
            const WBKB::Module* gm = Kb.ModuleById(g->ModuleId);
            int gcpu = gm ? gm->CpuNum() : 0;
            u32 size = std::max<u32>(g->Size, g->Thumb() ? 2u : 4u);
            QString otherErr;       // only the compared function's bytes must be readable
            QString& err = i == 0 ? readErr : otherErr;
            if (gm && gm->OverlayId >= 0 && gcpu == 0 && OverlayLoadedAt(g->Addr) != gm->OverlayId)
            {
                const OverlayInfo* oi = WB.Rom.Overlay((u32)gcpu, (u32)gm->OverlayId);
                std::vector<u8> image;
                if (!oi || !WBQ::OverlayImage(WB.Rom, *oi, image) || g->Addr < oi->RamStart || g->Addr - oi->RamStart + size > image.size())
                {
                    err = "cannot read the function's overlay from the ROM";
                    continue;
                }
                u32 o = g->Addr - oi->RamStart;
                bytes[i].assign(image.begin() + o, image.begin() + o + size);
                continue;
            }
            for (u32 a = g->Addr; a < g->Addr + size; a++)
            {
                u32 v = 0;
                if (!WB.Peek((u32)gcpu, a, 1, v)) { err = QString("cannot read 0x%1").arg(a, 8, 16, QChar('0')); bytes[i].clear(); break; }
                bytes[i].push_back((u8)v);
            }
        }
    });
    if (!ran) { error = "the emulator did not answer in time"; return false; }
    if (!readErr.isEmpty()) { error = readErr; return false; }

    auto resolve = [&](u32 v, bool call, WBCmp::SymbolRef& s) -> bool
    {
        int vov = cpu == 0 ? OverlayLoadedAt(v & ~1u) : -1;
        if (const WBKB::Function* g = Kb.FunctionStarting(cpu, v & ~1u, vov))
        {
            if (!call && g->Thumb() != ((v & 1) != 0)) return false;
            s.Name = Kb.NameAt(cpu, g->Addr, vov);
            s.Function = true;
            s.FunctionId = g->Id;
            return true;
        }
        if (call) return false;
        if (const WBKB::DataObject* d = Kb.DataObjectAt(cpu, v, vov))
        {
            if (d->Name.isEmpty()) return false;
            s.Name = d->Name;
            s.Addend = v - d->Addr;
            s.DataObjectId = d->Id;
            return true;
        }
        if (const WBKB::Label* l = Kb.LabelAt(cpu, v, vov))
        {
            if (l->Addr != v || l->Name.isEmpty()) return false;
            s.Name = l->Name;
            return true;
        }
        return false;
    };
    WBCmp::CodeAnalysis code = WBCmp::AnalyzeCode(bytes[0], f->Addr, f->Thumb(), resolve);
    QDir().mkpath(dir);
    QFile t(job.TargetPath);
    if (!t.open(QIODevice::WriteOnly) || t.write(WBCmp::WriteTargetObject(name, bytes[0], f->Thumb(), code)) < 0)
    {
        error = "cannot write " + job.TargetPath;
        return false;
    }
    t.close();
    if (!job.ContextPath.isEmpty())
    {
        WBCmp::CodeAnalysis all = code;
        for (size_t i = 1; i < own.size(); i++)
        {
            if (bytes[i].empty()) continue;
            WBCmp::CodeAnalysis more = WBCmp::AnalyzeCode(bytes[i], own[i]->Addr, own[i]->Thumb(), resolve);
            all.Relocs.insert(all.Relocs.end(), more.Relocs.begin(), more.Relocs.end());
        }
        QString title = f->SourceFile.isEmpty() ? name : QFileInfo(f->SourceFile).fileName();
        QFile c(job.ContextPath);
        if (!c.open(QIODevice::WriteOnly) || c.write(WBCmp::GenerateContext(Kb, own, title, all).toUtf8()) < 0)
        {
            error = "cannot write " + job.ContextPath;
            return false;
        }
    }
    return true;
}

bool WBHost::PrepareSelfTest(qint64 functionId, CompareJob& job, QString& error)
{
    WBCmp::CompareConfig cc;
    if (!LoadCompareConfig(cc, error)) return false;
    const WBKB::Function* f = functionId ? Kb.FunctionById(functionId) : nullptr;
    const WBKB::Module* m = f ? Kb.ModuleById(f->ModuleId) : nullptr;
    WBCmp::CompilerChoice choice = cc.Resolve(m ? m->Name : QString(), f ? f->Addr : 0);
    QString compiler = CompilerPath(choice.Label);
    if (compiler.isEmpty() && QFileInfo::exists(choice.Label)) compiler = choice.Label;
    job = CompareJob();
    job.SelfTest = true;
    job.FunctionId = functionId;
    job.WorkDir = ProjectDir();
    job.CompilerLabel = choice.Label;
    job.Flags = choice.Flags;
    job.Command = Expand("\"{python}\" \"{script}\" --self-test --compiler \"{compiler}\" --compiler-label \"{compiler_label}\" "
                         "--flags \"{flags}\" --objdiff \"{objdiff}\" --timeout {timeout}", {
        {"python", Cfg.PythonPath.isEmpty() ? QString("python") : Cfg.PythonPath},
        {"script", CompareScriptPath()},
        {"compiler", compiler},
        {"compiler_label", choice.Label},
        {"flags", choice.Flags},
        {"objdiff", Cfg.ObjdiffPath.isEmpty() ? QString("objdiff-cli") : Cfg.ObjdiffPath},
        {"timeout", QString::number(Cfg.CompareTimeoutSec)},
    });
    job.Fingerprint = ScriptFingerprint(CompareCommandTemplate(), CompareScriptPath());
    return true;
}

void WBHost::RunCompare(const CompareJob& job, bool record, qint64 author, const QString& agentSession,
                        std::function<void(const CompareRun&)> done)
{
    QProcess* p = new QProcess(this);
    p->setWorkingDirectory(job.WorkDir);
    QTimer* timer = new QTimer(p);
    timer->setSingleShot(true);
    auto timedOut = std::make_shared<bool>(false);
    connect(timer, &QTimer::timeout, p, [p, timedOut]() { *timedOut = true; p->kill(); });
    connect(p, &QProcess::finished, this, [this, p, job, record, author, agentSession, done, timedOut](int code, QProcess::ExitStatus)
    {
        CompareRun r;
        r.ExitCode = code;
        r.TimedOut = *timedOut;
        QByteArray out = p->readAllStandardOutput();
        QByteArray err = p->readAllStandardError();
        p->deleteLater();
        r.Stdout = QString::fromUtf8(out.left(200000));
        r.Stderr = QString::fromUtf8(err.left(200000));
        if (r.TimedOut)
        {
            r.Error = QString("the compare command ran past its timeout (%1 s) and was stopped").arg(Cfg.CompareTimeoutSec);
            r.Explanation = WBCmp::ExplainCompareFailure("compile", "timed out", QString());
            done(r);
            return;
        }
        WBCmp::CompareOutput o = WBCmp::ParseCompareOutput(out);
        r.ContractErrors = o.Errors;
        r.Result = o.Obj;
        if (!o.Found)
        {
            r.Error = QString("The compare command printed no JSON result (exit %1):\n%2\n%3")
                          .arg(code).arg(QString::fromUtf8(out.left(2000)), QString::fromUtf8(err.left(2000)));
            done(r);
            return;
        }
        QString stage = o.Obj.value("stage").toString(o.Obj.contains("error") ? "compile" : "done");
        if (o.Obj.contains("error"))
        {
            r.Error = o.Obj.value("error").toString();
            r.Explanation = WBCmp::ExplainCompareFailure(stage, r.Error, o.Obj.value("log").toString());
            done(r);
            return;
        }
        if (!o.Errors.isEmpty())
        {
            r.Error = "The compare command's result breaks the contract: " + o.Errors.join("; ");
            done(r);
            return;
        }
        r.Ok = true;
        if (job.SelfTest)
        {
            if (o.Obj.value("score").toDouble() >= 100.0)
            {
                Cfg.CompareSelfTestOk = job.Fingerprint + "|" + job.CompilerLabel + "|" + CompilerPath(job.CompilerLabel) + "|" + Cfg.ObjdiffPath;
                SaveSettings();
            }
            else r.Error = "The self-test compiled, but its function did not match itself. Check the compiler flags are deterministic.";
            done(r);
            return;
        }
        if (record && !o.Obj.value("recorded").toBool())
        {
            WBKB::BuildResult b;
            b.FunctionId = job.FunctionId;
            b.Compiler = o.Obj.value("compiler").toString(job.CompilerLabel.isEmpty() ? QString("unknown") : job.CompilerLabel);
            b.Flags = o.Obj.value("flags").toString(job.Flags);
            b.Score = o.Obj.value("score").toDouble();
            b.Matched = o.Obj.value("matched").toBool(b.Score >= 100.0);
            b.Diff = QString::fromUtf8(QJsonDocument(o.Obj.value("diff").toArray()).toJson(QJsonDocument::Compact));
            b.CommitHash = o.Obj.value("commit").toString();
            b.SourceFile = o.Obj.value("source_file").toString();
            QString e;
            WBKB::KnowledgeStore::AuthorScope scope(Kb, author, agentSession);
            if (!Kb.RecordBuildResult(b, e)) { r.Ok = false; r.Error = e; done(r); return; }
            r.Recorded = true;
        }
        else if (record)
        {
            Kb.Reload();
            r.Recorded = true;
        }
        done(r);
    });
    connect(p, &QProcess::errorOccurred, this, [p, done](QProcess::ProcessError)
    {
        if (p->state() != QProcess::NotRunning) return;
        CompareRun r;
        r.Error = "the compare command could not be started: " + p->errorString();
        r.Explanation = WBCmp::ExplainCompareFailure("run", r.Error, QString());
        p->deleteLater();
        done(r);
    });
    timer->start((Cfg.CompareTimeoutSec + 5) * 1000);
    p->startCommand(job.Command);
}

void WBHost::CompareFunction(qint64 functionId, bool record, qint64 author, const QString& agentSession,
                             std::function<void(const CompareRun&)> done)
{
    CompareJob job;
    CompareRun r;
    if (!EnsureCompareProject(r.Error) || !PrepareCompare(functionId, job, r.Error)) { done(r); return; }
    if (!CompareApproved(job))
    {
        r.Error = "The compare command (or the project's compare script) changed and has not been approved yet. "
                  "Run it once from the Source and diff panel, or approve it in Build > Compare setup.";
        done(r);
        return;
    }
    RunCompare(job, record, author, agentSession, done);
}

void WBHost::CheckCompareSetup(qint64 functionId, QObject* ctx, std::function<void(std::vector<SetupRow>)> done)
{
    // gathered here (main thread); the probes that start programs run on a pool thread
    QString python = Cfg.PythonPath.isEmpty() ? QString("python") : Cfg.PythonPath;
    QString objdiff = Cfg.ObjdiffPath.isEmpty() ? QString("objdiff-cli") : Cfg.ObjdiffPath;
    QString root = Cfg.SourceRoot;
    WBCmp::CompareConfig cc;
    QString cfgErr;
    bool cfgOk = LoadCompareConfig(cc, cfgErr);
    const WBKB::Function* f = functionId ? Kb.FunctionById(functionId) : nullptr;
    const WBKB::Module* m = f ? Kb.ModuleById(f->ModuleId) : nullptr;
    QString module = m ? m->Name : QString();
    WBCmp::CompilerChoice choice = cc.Resolve(module, f ? f->Addr : 0);
    QString compiler = CompilerPath(choice.Label);
    if (compiler.isEmpty() && QFileInfo::exists(choice.Label)) compiler = choice.Label;
    bool selfOk = !Cfg.CompareSelfTestOk.isEmpty() &&
        Cfg.CompareSelfTestOk == ScriptFingerprint(CompareCommandTemplate(), CompareScriptPath()) + "|" + choice.Label + "|" + compiler + "|" + Cfg.ObjdiffPath;
    Background<std::vector<SetupRow>>([=]()
    {
        auto probe = [](const QString& prog, const QStringList& args, QString& out) -> bool
        {
            QProcess p;
            p.start(prog, args);
            if (!p.waitForStarted(5000)) return false;
            if (!p.waitForFinished(10000)) { p.kill(); p.waitForFinished(1000); return false; }
            out = QString::fromUtf8(p.readAllStandardOutput() + p.readAllStandardError()).trimmed();
            return true;
        };
        std::vector<SetupRow> rows;
        SetupRow r;
        QString out;

        r = SetupRow();
        r.Id = "python"; r.Label = "Python"; r.Fix = "python";
        r.Ok = probe(python, {"--version"}, out) && out.startsWith("Python 3");
        r.Message = r.Ok ? out : QString("Python 3 was not found (%1). Locate python.exe.").arg(python);
        rows.push_back(r);

        r = SetupRow();
        r.Id = "compiler"; r.Label = "Compiler"; r.Fix = "compiler";
        if (choice.Label.isEmpty()) r.Message = "No compiler is set for this module yet.";
        else if (compiler.isEmpty()) r.Message = QString("No executable is set for compiler '%1' on this computer. Browse for it.").arg(choice.Label);
        else if (!QFileInfo(compiler).isFile()) r.Message = QString("%1 does not exist.").arg(compiler);
        else
        {
            bool gcc = QFileInfo(compiler).fileName().contains("gcc", Qt::CaseInsensitive);
            r.Ok = probe(compiler, gcc ? QStringList{"--version"} : QStringList{"-version"}, out);
            QString first = out.section('\n', 0, 0).trimmed();
            r.Message = r.Ok ? QString("%1: %2").arg(choice.Label, first.isEmpty() ? QFileInfo(compiler).fileName() : first)
                             : QString("%1 did not run.").arg(compiler);
        }
        rows.push_back(r);

        r = SetupRow();
        r.Id = "objdiff"; r.Label = "objdiff-cli"; r.Fix = "objdiff";
        if (probe(objdiff, {"--version"}, out))
        {
            QRegularExpressionMatch v = QRegularExpression("(\\d+)\\.(\\d+)\\.(\\d+)").match(out);
            int major = v.captured(1).toInt(), minor = v.captured(2).toInt();
            r.Ok = v.hasMatch() && (major > 3 || (major == 3 && minor >= 8));
            r.Message = r.Ok ? out : QString("%1 is too old: 3.8 or newer is needed.").arg(out);
        }
        else r.Message = QString("objdiff-cli was not found (%1). Browse for objdiff-cli.exe.").arg(objdiff);
        rows.push_back(r);

        r = SetupRow();
        r.Id = "source_root"; r.Label = "Source tree"; r.Fix = "source_root";
        QFileInfo ri(root);
        r.Ok = !root.isEmpty() && ri.isDir() && ri.isWritable();
        r.Message = root.isEmpty() ? QString("No source tree is set. Choose the folder holding the C files.")
                    : r.Ok ? root : QString("%1 is not a writable folder.").arg(root);
        rows.push_back(r);

        r = SetupRow();
        r.Id = "settings"; r.Label = "Compiler settings"; r.Fix = "settings";
        if (!cfgOk) r.Message = cfgErr;
        else if (choice.Label.isEmpty())
            r.Message = module.isEmpty() ? QString("compare.json names no default compiler.")
                                         : QString("compare.json has no compiler for %1. Add settings for %1.").arg(module);
        else
        {
            r.Ok = true;
            r.Message = QString("%1 %2 (%3)").arg(choice.Label, choice.Flags, choice.From);
        }
        rows.push_back(r);

        r = SetupRow();
        r.Id = "self_test"; r.Label = "Self-test"; r.Fix = "self_test";
        r.Ok = selfOk;
        r.Message = selfOk ? QString("passed with this toolchain") : QString("Not run with this toolchain yet.");
        rows.push_back(r);
        return rows;
    }, ctx, done);
}

void WBHost::InstallArchiveResolver()
{
    auto arc = Archive();
    auto files = RomFiles();
    auto cont = Containers();
    RunOnEmu([this, arc, files, cont]()
    {
        WB.SubFile = nullptr;
        WB.SubFileName = nullptr;
        WB.SubFileResolve = nullptr;
        WB.FileIndexed = nullptr;
        WB.FilePackedMembers = nullptr;
        WB.SubFileInside = nullptr;
        if (files) WB.FileIndexed = [files](u32 fileId) { return files->Indexed(fileId); };
        if (cont)
        {
            WB.FilePackedMembers = [cont](u32 fileId)
            {
                const WBArc::Container* c = cont->ByFile(fileId);
                return c && !c->Packed.empty();
            };
            WB.SubFileInside = [cont](u32 fileId, u32 outer, u32 inner)
            {
                const WBArc::Container* c = cont->ByFile(fileId);
                return c && outer && inner && cont->Inside(*c, (int)outer - 1, (int)inner - 1);
            };
        }
        if (!arc && !files && !cont) return;
        WB.SubFileResolve = [arc, files, cont](const u8* mem, u32 base, u32 len, u32 addr, u32 size, u32 which, std::vector<SubFileRun>& out)
        {
            if (arc && (which & 1))
                for (const WBArc::Match& mt : arc->Resolve(mem, base, len, addr, size, 64))
                {
                    SubFileRun r {};
                    r.Addr = mt.Addr;
                    r.Size = mt.Size;
                    r.FileId = arc->ArchiveFileId;
                    r.Members[r.Count++] = (u32)mt.Member + 1;
                    for (int alt : mt.Alternatives)
                        if (r.Count < 8) r.Members[r.Count++] = (u32)alt + 1;
                    out.push_back(r);
                }
            if (cont && (which & 1))
                for (const WBArc::Match& mt : cont->Resolve(mem, base, len, addr, size, 64))
                {
                    // the same bytes in several containers: one run per container file
                    std::vector<SubFileRun> runs;
                    auto add = [&](int global)
                    {
                        auto [ci, mi] = cont->FromGlobal(global);
                        if (ci < 0) return;
                        u32 id = cont->Containers()[ci].FileId;
                        auto it = std::find_if(runs.begin(), runs.end(), [id](const SubFileRun& r) { return r.FileId == id; });
                        if (it == runs.end()) { runs.push_back(SubFileRun{mt.Addr, mt.Size, id, {}, 0}); it = runs.end() - 1; }
                        if (it->Count < 8) it->Members[it->Count++] = (u32)mi + 1;
                    };
                    add(mt.Member);
                    for (int alt : mt.Alternatives) add(alt);
                    out.insert(out.end(), runs.begin(), runs.end());
                }
            if (files && (which & 2))
                for (const WBArc::Match& mt : files->Resolve(mem, base, len, addr, size, 64))
                {
                    // identical ROM files are separate sources: one run each
                    std::vector<u32> seen;
                    auto add = [&](int form)
                    {
                        u32 id = files->Forms()[form].FileId;
                        if (std::find(seen.begin(), seen.end(), id) != seen.end()) return;
                        seen.push_back(id);
                        SubFileRun r {};
                        r.Addr = mt.Addr;
                        r.Size = mt.Size;
                        r.FileId = id;
                        r.Members[r.Count++] = 0;
                        out.push_back(r);
                    };
                    add(mt.Member);
                    for (int alt : mt.Alternatives) add(alt);
                }
        };
        std::function<u32(u32, u32)> archiveFile;
        std::function<std::string(u32, u32)> archiveName;
        if (arc)
        {
            // card words arrive in order: remember the last file's data range
            auto last = std::make_shared<std::pair<u32, u32>>(0, 0);    // [start, end) archive offsets
            auto lastMember = std::make_shared<u32>(0);
            archiveFile = [arc, last, lastMember](u32 fileId, u32 romoff) -> u32
            {
                if (fileId != arc->ArchiveFileId || romoff < arc->ArchiveRomStart) return 0;
                u32 off = romoff - arc->ArchiveRomStart;
                if (off >= last->first && off < last->second) return *lastMember;
                std::string part;
                int i = arc->MemberAt(off, &part);
                if (i < 0 || part == "local header" || part == "central directory record") return 0;   // index reads, not file data
                const WBArc::Member& m = arc->Members()[i];
                *last = {m.DataOffset, m.DataOffset + m.PackedSize};
                *lastMember = (u32)i + 1;
                return (u32)i + 1;
            };
            archiveName = [arc](u32 fileId, u32 member) -> std::string
            {
                if (fileId != arc->ArchiveFileId || !member || member > arc->Members().size()) return std::string();
                return arc->Members()[member - 1].Path;
            };
        }
        if (!arc && !cont) return;
        // a container file's member at a ROM offset: card reads of it arrive in order too
        auto lastC = std::make_shared<std::tuple<u32, u32, u32, u32>>(0xFFFFFFFF, 0, 0, 0);   // file, [start, end) ROM offsets, member
        WB.SubFile = [arc, archiveFile, cont, lastC](u32 fileId, u32 romoff) -> u32
        {
            if (arc && fileId == arc->ArchiveFileId) return archiveFile(fileId, romoff);
            if (!cont) return 0;
            auto& [lf, ls, le, lm] = *lastC;
            if (fileId == lf && romoff >= ls && romoff < le) return lm;
            const WBArc::Container* c = cont->ByFile(fileId);
            if (!c || !c->Packed.empty() || romoff < c->RomStart) return 0;
            int i = cont->MemberAt(*c, romoff - c->RomStart);
            if (i < 0) return 0;
            const WBArc::ContainerMember& m = c->Members[i];
            *lastC = {fileId, c->RomStart + m.Offset, c->RomStart + m.Offset + m.StoredSize, (u32)i + 1};
            // a nested container's own header and tables are inside its member too: only its files are cached
            if (m.Kind == "NARC" || m.Kind == "SDAT") std::get<0>(*lastC) = 0xFFFFFFFF;
            return (u32)i + 1;
        };
        WB.SubFileName = [arc, archiveName, cont](u32 fileId, u32 member) -> std::string
        {
            if (arc && fileId == arc->ArchiveFileId) return archiveName(fileId, member);
            const WBArc::Container* c = cont ? cont->ByFile(fileId) : nullptr;
            if (!c || !member || member > c->Members.size()) return std::string();
            return c->Members[member - 1].Name;
        };
    });
}

void WBHost::SeedArchiveAssets()
{
    auto arc = Archive();
    if (!arc || !Kb.IsOpen()) return;
    std::vector<WBKB::ArchiveAssetRow> rows;
    rows.reserve(arc->Members().size());
    for (const WBArc::Member& m : arc->Members())
        rows.push_back({QString::fromStdString(m.Path), arc->ArchiveRomStart + m.DataOffset, m.Size, m.PackedSize});
    QString err;
    if (!Kb.SeedArchiveAssets(arc->ArchiveFileId, QString::fromStdString(arc->ArchivePath), rows, err))
        Inst->osdAddMessage(0xFFA0A0, "Could not add the archived files to the knowledge store: %s", err.toUtf8().constData());
}

void WBHost::SeedContainerAssets()
{
    auto cont = Containers();
    if (!cont || !Kb.IsOpen()) return;
    for (const WBArc::Container& c : cont->Containers())
    {
        std::vector<WBKB::ArchiveAssetRow> rows;
        rows.reserve(c.Members.size());
        // members inside a container packed whole have no place of their own in the ROM: the container's
        for (const WBArc::ContainerMember& m : c.Members)
            rows.push_back({QString::fromStdString(m.Name), c.RomStart + (m.Offset == WBArc::ContainerMember::NoOffset ? 0 : m.Offset),
                            m.Size, m.StoredSize});
        QString err;
        if (!Kb.SeedArchiveAssets(c.FileId, QString::fromStdString(c.Path), rows, err))
        {
            Inst->osdAddMessage(0xFFA0A0, "Could not add the files in containers to the knowledge store: %s", err.toUtf8().constData());
            return;
        }
    }
}

std::shared_ptr<const WBArc::ContainerIndex> WBHost::Containers() const
{
    std::lock_guard<std::mutex> l(Arc->Lock);
    return Arc->Containers;
}

std::shared_ptr<const WBAsset::Catalog> WBHost::Graphics() const
{
    std::lock_guard<std::mutex> l(Arc->Lock);
    return Arc->Gfx;
}

QString WBHost::GraphicsStatus() const
{
    std::lock_guard<std::mutex> l(Arc->Lock);
    return Arc->GfxStatus;
}

void WBHost::StartGraphicsCatalog()
{
    // runs again whenever the archive or the containers are ready: each build covers what is indexed by then
    std::shared_ptr<ArcShared> shared = Arc;
    std::shared_ptr<std::vector<WBAsset::Catalog::RomInput>> rom;
    std::shared_ptr<const WBArc::ContainerIndex> cont;
    std::shared_ptr<const WBArc::ArchiveIndex> arch;
    u32 gen;
    {
        std::lock_guard<std::mutex> l(Arc->Lock);
        rom = Arc->GfxRom;
        cont = Arc->Containers;
        arch = Arc->Index;
        gen = Arc->Generation;
    }
    if (!rom) return;
    std::thread([shared, gen, rom, cont, arch]()
    {
        auto cat = std::make_shared<WBAsset::Catalog>();
        cat->Build(*rom, cont, arch);
        std::lock_guard<std::mutex> l(shared->Lock);
        if (gen != shared->Generation || !shared->Host) return;
        // a later build (more indexes ready) may have finished first: keep the one that covers more
        if (shared->Gfx && shared->Gfx->Files().size() > cat->Files().size()) return;
        shared->Gfx = cat;
        shared->GfxStatus = cat->Files().empty() ? "none" : "ready";
        WBHost* h = shared->Host;
        QMetaObject::invokeMethod(h, [h]() { emit h->graphicsChanged(); }, Qt::QueuedConnection);
    }).detach();
}

QString WBHost::ContainerStatus() const
{
    std::lock_guard<std::mutex> l(Arc->Lock);
    return Arc->ContainerStatus;
}

std::shared_ptr<const WBArc::ArchiveIndex> WBHost::Archive() const
{
    std::lock_guard<std::mutex> l(Arc->Lock);
    return Arc->Index;
}

std::shared_ptr<const WBArc::RomFileIndex> WBHost::RomFiles() const
{
    std::lock_guard<std::mutex> l(Arc->Lock);
    return Arc->Files;
}

QString WBHost::ArchiveStatus() const
{
    std::lock_guard<std::mutex> l(Arc->Lock);
    return Arc->Status;
}

void WBHost::OnStateLoaded()
{
    WB.OnStateLoaded();
    // data origin labels described the memory the state replaced: rebuilt from what content proves
    bool on = WB.Taint.Enabled();
    u64 n = WB.Taint.RebuildAfterStateLoad();
    if (on)
        emit notice(QString("Savestate loaded at frame %1: data origin was rebuilt from content. %2 bytes still match ROM code, "
                            "overlays or archived files; the rest has no origin.").arg(WB.Frame()).arg(n));
}

void WBHost::ApplyInput(u32& keymask, bool& touching, u16& tx, u16& ty, bool& lid)
{
    std::lock_guard<std::mutex> l(InputLock);
    if (!AgentFrames) return;
    AgentStarted = true;
    bool person = (keymask & 0xFFF) != 0xFFF || touching;
    if (person)
    {
        // the person's input always wins
        AgentOverridden = true;
        return;
    }
    keymask &= AgentKeys;
    if (AgentTouch)
    {
        touching = true;
        tx = AgentX;
        ty = AgentY;
    }
    if (AgentLid) lid = true;
}

bool WBHost::AfterFrame()
{
    bool pause = false;
    std::function<void(bool)> framescb;
    std::function<void(bool, bool)> inputcb;
    bool overridden = false, interrupted = false;
    {
        std::lock_guard<std::mutex> l(InputLock);
        if (AgentFrames)
        {
            if (--AgentFrames == 0)
            {
                inputcb = std::move(InputDoneCb);
                InputDoneCb = nullptr;
                overridden = AgentOverridden;
                if (PauseAfterInput) pause = true;
                PauseAfterInput = false;
                if (AgentLid)
                    if (NDS* nds = WB.GetNDS()) nds->SetLidClosed(false);
                AgentLid = false;
            }
        }
        if (FramesActive)
        {
            if (FramesToRun) FramesToRun--;
            if (!FramesToRun)
            {
                FramesActive = false;
                framescb = std::move(FramesDoneCb);
                FramesDoneCb = nullptr;
                interrupted = FramesInterrupted;
                pause = true;
            }
        }
    }
    if (inputcb)
        QMetaObject::invokeMethod(this, [this, inputcb, overridden]() { inputcb(overridden, false); emit inputDone(overridden); }, Qt::QueuedConnection);
    if (framescb)
        QMetaObject::invokeMethod(this, [this, framescb, interrupted]() { framescb(interrupted); emit framesDone(interrupted); }, Qt::QueuedConnection);
    return pause;
}

void WBHost::IdleWait(int ms)
{
    // a person paused while an agent was running frames or holding input: report it as interrupted
    std::function<void(bool)> framescb;
    std::function<void(bool, bool)> inputcb;
    bool overridden = false;
    EmuThread* t = Inst->getEmuThread();
    {
        std::lock_guard<std::mutex> l(InputLock);
        if (FramesActive && t && !t->emuIsRunning())
        {
            FramesActive = false;
            FramesToRun = 0;
            framescb = std::move(FramesDoneCb);
            FramesDoneCb = nullptr;
        }
        // input sent to a paused game resumes it: until its first frame, paused is not yet the person's doing
        if (AgentFrames && t && !t->emuIsRunning() && (!t->emuIsActive() || AgentStarted || !PauseAfterInput))
        {
            AgentFrames = 0;
            overridden = AgentOverridden;
            PauseAfterInput = false;
            inputcb = std::move(InputDoneCb);
            InputDoneCb = nullptr;
            if (AgentLid)
                if (NDS* nds = WB.GetNDS()) nds->SetLidClosed(false);
            AgentLid = false;
        }
    }
    if (framescb)
        QMetaObject::invokeMethod(this, [this, framescb]() { framescb(true); emit framesDone(true); }, Qt::QueuedConnection);
    if (inputcb)
        QMetaObject::invokeMethod(this, [this, inputcb, overridden]() { inputcb(overridden, true); emit inputDone(overridden); }, Qt::QueuedConnection);
    WB.Dbg.WaitJobs(ms);
}

void WBHost::UpdateStatus(double fps)
{
    WBStatus s;
    NDS* nds = WB.GetNDS();
    EmuThread* t = Inst->getEmuThread();
    bool active = t && t->emuIsActive();
    if (!active || !nds) s.EmuState = WBStatus::NoGame;
    else if (WB.Dbg.InBreak()) s.EmuState = WBStatus::Break;
    else if (t->emuIsRunning()) s.EmuState = WBStatus::Running;
    else s.EmuState = WBStatus::Paused;
    s.Frame = WB.Frame();
    s.Recording = WB.Rec.Enabled();
    s.History = WB.Rec.WriteHistoryOn();
    s.Origin = WB.Taint.Enabled();
    s.Available = !WB.JITActive();
    s.FramesRecorded = WB.Rec.FramesRecorded();
    s.HistoryCount = WB.Rec.HistoryCount();
    s.HistoryCap = WB.Rec.HistoryCap();
    s.HistoryWrapped = WB.Rec.HistoryWrapped();
    s.HistorySkipped = WB.Rec.HistorySkipped();
    s.HistoryOldestFrame = WB.Rec.HistoryOldestFrame();
    s.RecordingSince = RecordingSince;
    s.HistorySince = HistorySince;
    s.OriginSince = OriginSince;
    s.StateLoadedFrame = WB.Taint.StateLoadedFrame();
    // the first time history drops writes in a session, say so once
    if (s.HistoryWrapped && !WrapNoticed)
    {
        WrapNoticed = true;
        Inst->osdAddMessage(0xFFD080, "Write history is full: writes before frame %u are being dropped. "
                            "Filter writes or use a larger history file (Recording panel).", s.HistoryOldestFrame);
    }
    if (!s.HistoryWrapped) WrapNoticed = false;
    s.Pages = WB.Rec.Store().PageCount();
    s.Fps = s.EmuState == WBStatus::Running ? fps : 0;
    for (int c = 0; c < 2; c++)
    {
        s.PC[c] = WB.CurrentPC(c);
        s.Thumb[c] = WB.CPSR(c) & 0x20;
    }
    if (s.EmuState == WBStatus::Break) s.BreakInfo = WB.Dbg.CurrentBreak();
    bool changed;
    {
        std::lock_guard<std::mutex> l(StatusLock);
        s.RomSha1 = Cur.RomSha1;
        s.RomTitle = Cur.RomTitle;
        s.RomCode = Cur.RomCode;
        changed = s.EmuState != Cur.EmuState || s.Recording != Cur.Recording || s.History != Cur.History || s.Origin != Cur.Origin ||
                  s.Frame / 10 != Cur.Frame / 10 || s.Available != Cur.Available;
        Cur = s;
    }
    if (changed) emit statusChanged();
}

// ---------------------------------------------------------------- any thread

WBStatus WBHost::Status() const
{
    std::lock_guard<std::mutex> l(StatusLock);
    return Cur;
}

bool WBHost::EmuRunning() const
{
    EmuThread* t = Inst->getEmuThread();
    return t && t->emuIsRunning();
}

bool WBHost::RunSync(std::function<void()> fn, int timeoutms)
{
    struct Shared { std::mutex m; std::condition_variable cv; bool done = false; };
    auto sh = std::make_shared<Shared>();
    WB.Dbg.PostJob([fn = std::move(fn), sh]()
    {
        fn();
        std::lock_guard<std::mutex> l(sh->m);
        sh->done = true;
        sh->cv.notify_all();
    });
    std::unique_lock<std::mutex> lk(sh->m);
    return sh->cv.wait_for(lk, std::chrono::milliseconds(timeoutms), [&] { return sh->done; });
}

void WBHost::OnCoreEvent(const WBEvent& ev)
{
    // emulation thread
    switch (ev.Type)
    {
    case WBEvent::BreakHitEvent:
        {
            {
                std::lock_guard<std::mutex> l(StatusLock);
                Cur.EmuState = WBStatus::Break;
                Cur.BreakInfo = ev.Break;
                Cur.PC[0] = WB.CurrentPC(0);
                Cur.PC[1] = WB.CurrentPC(1);
                Cur.Thumb[0] = WB.CPSR(0) & 0x20;
                Cur.Thumb[1] = WB.CPSR(1) & 0x20;
                Cur.Frame = WB.Frame();
            }
            std::function<void(bool)> framescb;
            {
                std::lock_guard<std::mutex> l(InputLock);
                if (FramesActive)
                {
                    FramesActive = false;
                    FramesToRun = 0;
                    framescb = std::move(FramesDoneCb);
                    FramesDoneCb = nullptr;
                }
            }
            if (framescb)
                QMetaObject::invokeMethod(this, [this, framescb]() { framescb(true); emit framesDone(true); }, Qt::QueuedConnection);
            emit statusChanged();
            emit breakChanged(true);
            emit breakpointHit(ev.Hit.Id, (int)ev.Break.Cpu, ev.Hit.Id >= 0 ? ev.Hit.PC : ev.Break.PC);
        }
        break;
    case WBEvent::BreakLeftEvent:
        {
            std::lock_guard<std::mutex> l(StatusLock);
            Cur.EmuState = WBStatus::Running;
        }
        emit statusChanged();
        emit breakChanged(false);
        break;
    case WBEvent::OverlayLoadedEvent:
        emit overlayLoaded((int)ev.Hit.Cpu, ev.Hit.Id);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- control

void WBHost::StartRecordingNow()
{
    // emulation thread
    if (WB.Rec.Enabled() || WB.JITActive() || !WB.GetNDS()) return;
    ApplyHistoryFile(nullptr);      // the last session took its history file along
    WB.Rec.Reset();
    WB.Rec.SetHistoryCap(Cfg.HistoryCap);
    WB.Taint.SetAllRomFiles(Cfg.OriginAllRomFiles);
    WB.Taint.SetFollowAddresses(Cfg.OriginFollowAddresses);
    WB.Rec.SetEnabled(true);
    RecordingSince = WB.Frame();
    // armed in settings or on the command line
    if (Cfg.AlsoHistory || HistoryAtBoot)
    {
        WB.Rec.SetWriteHistory(true);
        HistorySince = WB.Frame();
    }
    if (Cfg.AlsoOrigin || OriginAtBoot)
    {
        WB.Taint.Enable(true);
        OriginSince = WB.Frame();
    }
    // overlays that were loaded before recording started
    NDS* nds = WB.GetNDS();
    if (WB.Rom.Valid())
    {
        for (const OverlayInfo& o : WB.Rom.Overlays9)
        {
            if (o.Compressed() || (o.RamStart >> 24) != 0x02) continue;
            const RomFile* f = WB.Rom.FileById(o.FileId);
            if (!f) continue;
            u32 n = std::min<u32>(std::min<u32>(o.RamSize, f->End - f->Start), 0x400);
            if (n < 16) continue;
            if (!memcmp(nds->MainRAM + (o.RamStart & nds->MainRAMMask), WB.Rom.Data() + f->Start, n))
                WB.Rec.MarkOverlayLoaded(0, o.Id);
        }
    }
}

void WBHost::SetRecording(bool on)
{
    if (on)
    {
        if (WB.JITActive()) return;
        RunSync([this]() { StartRecordingNow(); });
    }
    else
    {
        FinishSession(false);
    }
    emit recordingChanged();
    emit statusChanged();
}

void WBHost::SetWriteHistory(bool on)
{
    // both need Recording: turning one on starts it (UI_REQUIREMENTS D-4)
    bool started = false, blocked = false;
    RunSync([this, on, &started, &blocked]()
    {
        if (on && !WB.Rec.Enabled())
        {
            StartRecordingNow();
            started = WB.Rec.Enabled();
            blocked = !started;
        }
        if (on && !WB.Rec.Enabled()) return;
        if (on && !WB.Rec.WriteHistoryOn()) HistorySince = WB.Frame();
        WB.Rec.SetWriteHistory(on);
    });
    if (started) emit notice("Recording started so that write history can run.");
    if (blocked) emit notice("Write history needs Recording: " + Missing(WBNeed::Recording));
    emit recordingChanged();
    emit statusChanged();
}

void WBHost::SetDataOrigin(bool on)
{
    bool allrom = Cfg.OriginAllRomFiles;
    bool started = false, blocked = false;
    RunSync([this, on, allrom, &started, &blocked]()
    {
        if (on && !WB.Rec.Enabled())
        {
            StartRecordingNow();
            started = WB.Rec.Enabled();
            blocked = !started;
        }
        if (on && !WB.Rec.Enabled()) return;
        if (on && !WB.Taint.Enabled()) OriginSince = WB.Frame();
        WB.Taint.SetAllRomFiles(allrom);
        WB.Taint.Enable(on);
    });
    if (started) emit notice("Recording started so that data origin can run.");
    if (blocked) emit notice("Data origin needs Recording: " + Missing(WBNeed::Recording));
    emit recordingChanged();
    emit statusChanged();
}

QString WBHost::Missing(WBNeed n) const
{
    WBStatus s = Status();
    switch (n)
    {
    case WBNeed::Game:
        return s.EmuState == WBStatus::NoGame ? "No game is running." : QString();
    case WBNeed::Interpreter:
        return !s.Available ? "The JIT recompiler is on; this needs the interpreter." : QString();
    case WBNeed::Recording:
        if (!s.Available) return Missing(WBNeed::Interpreter);
        if (s.EmuState == WBStatus::NoGame) return Missing(WBNeed::Game);
        return s.Recording ? QString() : "Recording is off.";
    case WBNeed::WriteHistory:
        if (!s.Recording && !s.Available) return Missing(WBNeed::Interpreter);
        return s.History ? QString() : "Write history is off.";
    case WBNeed::DataOrigin:
        if (!s.Recording && !s.Available) return Missing(WBNeed::Interpreter);
        return s.Origin ? QString() : "Data origin is off.";
    case WBNeed::Archive:
    {
        QString st = ArchiveStatus();
        if (st == "ready") return QString();
        if (st == "indexing") return "The game archive is still being indexed.";
        if (st == "none") return "No game archive (TrackMania's vfs.zip) was found in this ROM.";
        return "The game archive could not be read: " + st;
    }
    case WBNeed::Containers:
    {
        QString st = ContainerStatus();
        if (st == "ready") return QString();
        if (st == "indexing") return "The NARC and SDAT files are still being read.";
        return "No NARC or SDAT files were found in this ROM.";
    }
    }
    return QString();
}

QString WBHost::Covers(WBNeed n)
{
    switch (n)
    {
    case WBNeed::Recording:
        return "Recording covers what runs from the moment it starts: coverage, execution counts, calls and the call stack. No restart needed.";
    case WBNeed::WriteHistory:
        return "Write history records writes from the moment it is turned on, up to its capacity. It starts Recording if needed.";
    case WBNeed::DataOrigin:
        return "Data origin labels data from the moment it is turned on; data loaded earlier has no origin, except files recognised by their content. "
               "It starts Recording if needed. To cover loading, use Recording > Reset game with tracking.";
    case WBNeed::Interpreter:
        return "Recording, breakpoints and stepping need the interpreter. Turning the JIT off resets the game.";
    default:
        return QString();
    }
}

bool WBHost::TurnOffJitAndReset(QString& err)
{
    Config::Table cfg = Config::GetGlobalTable();
    cfg.SetBool("JIT.Enable", false);
    Config::Save();
    // the console picks the setting up when it is rebuilt by a reset
    if (Status().EmuState == WBStatus::NoGame) return true;
    if (!Reset(err)) return false;
    emit notice("The JIT recompiler is off and the game was reset.");
    return true;
}

void WBHost::SetHistoryCap(quint64 cap)
{
    Cfg.HistoryCap = cap;
    RunSync([this, cap]() { WB.Rec.SetHistoryCap(cap); });
    emit recordingChanged();
}

QString WBHost::ProjectDir() const
{
    if (Kb.IsOpen()) return QFileInfo(Kb.Path()).absolutePath();
    QString sha = Status().RomSha1;
    QString code = Status().RomCode;
    return QDir(emuDirectory).filePath(QString("workbench/%1_%2").arg(code.isEmpty() ? "GAME" : code, sha.left(12)));
}

QString WBHost::SessionsDir() const
{
    return QDir(ProjectDir()).filePath("sessions");
}

void WBHost::RefreshFuncSnapshot()
{
    std::vector<FuncSnap> funcs;
    for (const WBKB::Function& f : Kb.Functions())
    {
        const WBKB::Module* m = Kb.ModuleById(f.ModuleId);
        funcs.push_back({f.Id, f.Addr, f.Size, f.Thumb(), m ? m->CpuNum() : 0});
    }
    std::sort(funcs.begin(), funcs.end(), [](const FuncSnap& a, const FuncSnap& b) { return a.Addr < b.Addr; });
    for (size_t i = 0; i < funcs.size(); i++)
        if (!funcs[i].Size)
        {
            u32 next = (i + 1 < funcs.size()) ? funcs[i + 1].Addr : funcs[i].Addr + 0x100;
            funcs[i].Size = std::min<u32>(next - funcs[i].Addr, 0x10000);
        }
    // the recorder recognises tail calls into these
    std::vector<u32> entries[2];
    for (const FuncSnap& f : funcs) entries[f.Cpu & 1].push_back(f.Addr);
    RunOnEmu([this, e9 = std::move(entries[0]), e7 = std::move(entries[1])]()
    {
        WB.Rec.SetKnownFunctions(0, e9);
        WB.Rec.SetKnownFunctions(1, e7);
    });
    std::lock_guard<std::mutex> l(SnapLock);
    FuncSnapshot = std::move(funcs);
    ProjectDirCache = ProjectDir();
}

bool WBHost::CollectSession(bool keepRecording, CollectedSession& out)
{
    // emulation thread
    if (!WB.Rec.Enabled()) return false;
    std::vector<FuncSnap> funcs;
    {
        std::lock_guard<std::mutex> l(SnapLock);
        funcs = FuncSnapshot;
        out.Dir = QDir(ProjectDirCache).filePath("sessions");
    }
    u32& fs = out.FrameStart;
    u32& fe = out.FrameEnd;
    u32& flags = out.Flags;
    QString& sha = out.Sha;
    Recorder& rec = WB.Rec;
    rec.FlushTransfers();
    flags = (rec.WriteHistoryOn() ? 1 : 0) | (WB.Taint.Enabled() ? 2 : 0);
    rec.SerializeSession(out.Bytes, flags);
    fs = rec.SessionStartFrame();
    fe = WB.Frame();
    sha = QString::fromStdString(WB.Rom.Sha1Hex());
    // two saves in one second at one frame (saving without stopping, then stopping) get _2, _3...,
    // which also sort after the first
    QString base = QDir(out.Dir).filePath(QString("%1_f%2-%3").arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss")).arg(fs).arg(fe));
    out.Path = base + ".dswbs";
    for (int i = 2; QFile::exists(out.Path); i++) out.Path = QString("%1_%2.dswbs").arg(base).arg(i);

    // per-function evidence
    auto funcIndex = [&](int cpu, u32 pc) -> int
    {
        auto it = std::upper_bound(funcs.begin(), funcs.end(), pc, [](u32 a, const FuncSnap& f) { return a < f.Addr; });
        while (it != funcs.begin())
        {
            --it;
            if (pc >= it->Addr && pc < it->Addr + it->Size && it->Cpu == cpu) return (int)(it - funcs.begin());
            if (pc - it->Addr > 0x10000) break;
        }
        return -1;
    };

    struct Acc
    {
        u32 Exec = 0, Instr = 0, Bytes = 0;
        QJsonArray Callers, Callees;
        std::map<u32, std::pair<u32, int>> Data;   // start -> (end, access bits)
    };
    std::vector<Acc> acc(funcs.size());

    for (size_t i = 0; i < funcs.size(); i++)
    {
        const FuncSnap& f = funcs[i];
        for (u32 a = f.Addr; a < f.Addr + f.Size; a++)
        {
            const ByteRecord* r = rec.Get(WB.Translate(f.Cpu, a));
            if (!r || !r->exec_count) continue;
            acc[i].Bytes++;
            if (r->flags & ByteFlag::InstrStart) acc[i].Instr += r->exec_count;
            if (a == f.Addr) acc[i].Exec = r->exec_count;
        }
    }
    for (int c = 0; c < 2; c++)
        for (const auto& kv : rec.Edges(c))
        {
            const CallEdge& e = kv.second;
            int callee = funcIndex(c, e.callee & ~1u);
            int caller = funcIndex(c, e.call_site);
            if (callee >= 0 && funcs[callee].Addr == (e.callee & ~1u))
            {
                QJsonObject o;
                o["addr"] = (double)(caller >= 0 ? funcs[caller].Addr : e.caller_func);
                o["call_site"] = (double)e.call_site;
                o["count"] = (double)e.count;
                if (acc[callee].Callers.size() < 256) acc[callee].Callers.append(o);
            }
            if (caller >= 0)
            {
                QJsonObject o;
                o["addr"] = (double)e.callee;
                o["call_site"] = (double)e.call_site;
                o["count"] = (double)e.count;
                if (acc[caller].Callees.size() < 256) acc[caller].Callees.append(o);
            }
        }

    // data touched: last writer and recent readers of every recorded byte
    rec.Store().ForEach([&](u16 region, u32 page, const RecordPage* p)
    {
        if (IsOverlayRegion(region) || region == (u16)Region::MainRAM || region == (u16)Region::DTCM || region == (u16)Region::ITCM ||
            region == (u16)Region::SharedWRAM || region == (u16)Region::ARM7WRAM)
        {
            // reconstruct a representative virtual address for the region
            u32 base;
            switch ((Region)region)
            {
            case Region::DTCM: base = WB.GetNDS() ? WB.GetNDS()->ARM9.DTCMBase : 0; break;
            case Region::ITCM: base = 0x01FF8000; break;
            case Region::SharedWRAM: base = 0x03000000; break;
            case Region::ARM7WRAM: base = 0x03800000; break;
            default: base = 0x02000000; break;
            }
            u32 vpage = base + (page << PageShift);
            for (u32 i = 0; i < PageSize; i++)
            {
                const ByteRecord& r = p->Rec[i];
                if (r.write_count && r.last_writer_cpu < 2)
                {
                    int fi = funcIndex(r.last_writer_cpu, r.last_writer_pc);
                    if (fi >= 0)
                    {
                        auto& d = acc[fi].Data;
                        u32 a = vpage + i;
                        auto it = d.upper_bound(a);
                        if (it != d.begin() && std::prev(it)->second.first == a) { std::prev(it)->second.first = a + 1; std::prev(it)->second.second |= 2; }
                        else if (d.size() < 256) d[a] = {a + 1, 2};
                    }
                }
                if ((i & 3) == 0)
                    for (int k = 0; k < 2; k++)
                    {
                        u32 rp = p->Readers[i >> 2][k];
                        if (!rp) continue;
                        int fi = funcIndex(rp & 1, rp & ~1u);
                        if (fi < 0) continue;
                        auto& d = acc[fi].Data;
                        u32 a = vpage + i;
                        auto it = d.upper_bound(a);
                        if (it != d.begin() && std::prev(it)->second.first >= a) { auto& e = std::prev(it)->second; e.first = std::max(e.first, a + 4); e.second |= 1; }
                        else if (d.size() < 256) d[a] = {a + 4, 1};
                    }
            }
        }
    });

    for (size_t i = 0; i < funcs.size(); i++)
    {
        const Acc& a = acc[i];
        if (!a.Bytes && a.Callers.isEmpty() && a.Callees.isEmpty()) continue;
        QJsonArray data;
        for (const auto& kv : a.Data)
        {
            QJsonObject o;
            o["start"] = (double)kv.first;
            o["end"] = (double)kv.second.first;
            o["access"] = kv.second.second == 3 ? "rw" : kv.second.second == 2 ? "w" : "r";
            data.append(o);
        }
        WBKB::EvidenceRow row;
        row.FunctionId = funcs[i].Id;
        row.ExecCount = a.Exec;
        row.InstrCount = a.Instr;
        row.BytesExecuted = a.Bytes;
        row.Callers = QString::fromUtf8(QJsonDocument(a.Callers).toJson(QJsonDocument::Compact));
        row.Callees = QString::fromUtf8(QJsonDocument(a.Callees).toJson(QJsonDocument::Compact));
        row.DataTouched = QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact));
        out.Evidence.push_back(row);
    }

    // asset provenance: card reads and decompressions of card data
    std::shared_ptr<const WBArc::ArchiveIndex> arc = Archive();
    auto archived = [&](const WBArc::Member& m, const char* kind, u32 ramStart, u32 ramEnd, u32 pc, u32 frame)
    {
        QString member = QString::fromStdString(m.Path);
        out.Assets.push_back({arc->ArchiveFileId, QString::fromStdString(arc->ArchivePath) + "/" + member, arc->ArchiveRomStart + m.DataOffset,
                              m.Size, kind, ramStart, ramEnd, pc, frame, member, m.PackedSize});
    };
    std::shared_ptr<const WBArc::ContainerIndex> cont = Containers();
    auto contained = [&](const WBArc::Container& c, const WBArc::ContainerMember& m, const char* kind, u32 ramStart, u32 ramEnd, u32 pc, u32 frame)
    {
        QString member = QString::fromStdString(m.Name);
        u32 rom = c.RomStart + (m.Offset == WBArc::ContainerMember::NoOffset ? 0 : m.Offset);
        out.Assets.push_back({c.FileId, QString::fromStdString(c.Path) + "/" + member, rom, m.Size, kind, ramStart, ramEnd, pc, frame, member, m.StoredSize});
    };
    // the code that last wrote a main RAM byte (an unpacker or a copy loop), 0 if not recorded
    auto lastWriter = [&](u32 addr) -> u32
    {
        PhysAddr pa = WB.Translate(0, addr);
        if (pa.Recordable())
            if (RecordPage* pg = rec.Store().Get(pa.Region, pa.Offset >> PageShift))
            {
                const ByteRecord& b = pg->Rec[pa.Offset & (PageSize - 1)];
                if ((b.write_count || b.dma_write_count) && (b.last_writer_cpu & 0xF) < 2) return b.last_writer_pc;
            }
        return 0;
    };
    for (const ProvenanceEvent& ev : rec.Provenance())
    {
        if (ev.kind == (u8)ProvenanceKind::CardRead && ev.file_id != FileNone)
        {
            const RomFile* f = WB.Rom.FileById(ev.file_id);
            if (!f) continue;
            out.Assets.push_back({ev.file_id, QString::fromStdString(f->Path), f->Start, f->End - f->Start, "card_read",
                               ev.dst_addr, ev.dst_addr + ev.dst_size, ev.pc, ev.frame});
            // reads of a game archive: which archived files' bytes they fetched
            if (arc && ev.file_id == arc->ArchiveFileId && ev.rom_offset >= arc->ArchiveRomStart)
            {
                u32 off = ev.rom_offset - arc->ArchiveRomStart, endoff = off + std::max<u32>(ev.src_size, 1);
                while (off < endoff)
                {
                    // central directory reads are index lookups, not file data
                    if (off >= arc->CdStart && off < arc->CdStart + arc->CdSize) { off = arc->CdStart + arc->CdSize; continue; }
                    int i = arc->MemberAt(off);
                    if (i < 0) { off = (off | 0x1FF) + 1; continue; }
                    const WBArc::Member& m = arc->Members()[i];
                    u32 stop = std::min(endoff, std::max(m.DataOffset + m.PackedSize, off + 1));
                    u32 rs = ev.dst_addr + (off - (ev.rom_offset - arc->ArchiveRomStart));
                    archived(m, "archive_read", rs, rs + (stop - off), ev.pc, ev.frame);
                    off = stop;
                }
            }
            // reads of a container: which of its files' bytes they fetched (a container packed whole has
            // no file bytes of its own in the ROM)
            else if (const WBArc::Container* c = cont ? cont->ByFile(ev.file_id) : nullptr; c && c->Packed.empty() && ev.rom_offset >= c->RomStart)
            {
                u32 off = ev.rom_offset - c->RomStart, endoff = off + std::max<u32>(ev.src_size, 1);
                while (off < endoff)
                {
                    // the next member to start: a nested container's header ends where its first file starts
                    auto nx = std::upper_bound(c->ByOffset.begin(), c->ByOffset.end(), off, [c](u32 v, int k) { return v < c->Members[k].Offset; });
                    u32 next = nx == c->ByOffset.end() ? endoff : c->Members[*nx].Offset;
                    int i = cont->MemberAt(*c, off);
                    if (i < 0) { off = std::min(endoff, std::max(next, off + 1)); continue; }
                    const WBArc::ContainerMember& m = c->Members[i];
                    u32 stop = std::min({endoff, std::max(m.Offset + m.StoredSize, off + 1), std::max(next, off + 1)});
                    u32 rs = ev.dst_addr + (off - (ev.rom_offset - c->RomStart));
                    contained(*c, m, "archive_read", rs, rs + (stop - off), ev.pc, ev.frame);
                    off = stop;
                }
            }
        }
        else if (ev.kind == (u8)ProvenanceKind::Decompress)
        {
            for (const ProvenanceEvent& src : rec.Provenance())
            {
                if (src.kind != (u8)ProvenanceKind::CardRead || src.file_id == FileNone) continue;
                if (ev.src_addr >= src.dst_addr && ev.src_addr < src.dst_addr + src.dst_size)
                {
                    const RomFile* f = WB.Rom.FileById(src.file_id);
                    if (!f) break;
                    out.Assets.push_back({src.file_id, QString::fromStdString(f->Path), f->Start, f->End - f->Start, "decompress",
                                       ev.dst_addr, ev.dst_addr + ev.dst_size, ev.pc, ev.frame});
                    break;
                }
            }
        }
    }

    // archived files whose unpacked bytes are in main RAM now, and the code that wrote them there
    if (arc && WB.GetNDS() && WB.GetNDS()->MainRAM)
    {
        NDS* nds = WB.GetNDS();
        u32 len = nds->MainRAMMask + 1;
        for (const WBArc::Match& mt : arc->Resolve(nds->MainRAM, 0x02000000, len, 0x02000000, len, 20000))
        {
            if (mt.Size < 256) continue;
            ByteRecord r;
            u32 writer = 0;
            PhysAddr pa = WB.Translate(0, mt.Addr);
            if (pa.Recordable())
                if (RecordPage* pg = rec.Store().Get(pa.Region, pa.Offset >> PageShift))
                {
                    const ByteRecord& b = pg->Rec[pa.Offset & (PageSize - 1)];
                    if ((b.write_count || b.dma_write_count) && (b.last_writer_cpu & 0xF) < 2) writer = b.last_writer_pc;
                }
            archived(arc->Members()[mt.Member], "content", mt.Addr, mt.Addr + mt.Size, writer, fe);
        }
    }
    // the same for files inside containers
    if (cont && WB.GetNDS() && WB.GetNDS()->MainRAM)
    {
        NDS* nds = WB.GetNDS();
        u32 len = nds->MainRAMMask + 1;
        for (const WBArc::Match& mt : cont->Resolve(nds->MainRAM, 0x02000000, len, 0x02000000, len, 20000))
        {
            if (mt.Size < 256) continue;
            auto [ci, mi] = cont->FromGlobal(mt.Member);
            if (ci < 0) continue;
            const WBArc::Container& c = cont->Containers()[ci];
            contained(c, c.Members[mi], "content", mt.Addr, mt.Addr + mt.Size, lastWriter(mt.Addr), fe);
        }
    }

    if (!keepRecording)
    {
        WB.Taint.Enable(false);
        rec.SetWriteHistory(false);
        rec.SetEnabled(false);
        rec.LogEvent(RecEventKind::SessionSaved);
    }
    if (!out.Bytes.empty()) out.HistoryPath = KeepSessionHistory(out, !keepRecording);
    return !out.Bytes.empty();
}

// Session files are zlib-compressed (records are mostly repetitive: ~60x smaller at level 1):
// "DSWBSZ1\0", u64 LE uncompressed size, zlib stream. Uncompressed files are still read.
static const char SessionZMagic[8] = {'D', 'S', 'W', 'B', 'S', 'Z', '1', 0};

static QByteArray CompressSession(const std::vector<u8>& raw)
{
    QByteArray z = qCompress(raw.data(), (int)raw.size(), 1);     // 4-byte BE size, then the zlib stream
    if (z.size() < 4) return {};
    QByteArray out(SessionZMagic, 8);
    u64 n = raw.size();
    for (int i = 0; i < 8; i++) out.append((char)(n >> (i * 8)));
    out.append(z.constData() + 4, z.size() - 4);
    return out;
}

QByteArray WBHost::ReadSessionFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QByteArray data = f.readAll();
    if (data.size() < 16 || memcmp(data.constData(), SessionZMagic, 8) != 0) return data;
    u64 n = 0;
    for (int i = 0; i < 8; i++) n |= (u64)(u8)data[8 + i] << (i * 8);
    if (n >= 0x7FFFFFFF) return {};
    QByteArray z;
    z.reserve(data.size() - 12);
    for (int i = 3; i >= 0; i--) z.append((char)(n >> (i * 8)));
    z.append(data.constData() + 16, data.size() - 16);
    return qUncompress(z);
}

QString WBHost::StoreSession(const CollectedSession& s)
{
    QDir().mkpath(s.Dir);
    QString path = s.Path;
    QFile f(path);
    bool ok = f.open(QIODevice::WriteOnly);
    if (ok)
    {
        QByteArray z = CompressSession(s.Bytes);
        ok = z.isEmpty() ? f.write((const char*)s.Bytes.data(), (qint64)s.Bytes.size()) == (qint64)s.Bytes.size()
                         : f.write(z) == z.size();
        f.close();
    }
    if (!ok)
    {
        QFile::remove(path);
        if (!s.HistoryPath.isEmpty()) QFile::remove(s.HistoryPath);
        return QString();
    }
    if (!s.HistoryPath.isEmpty()) PruneHistoryFiles(s.Dir, s.HistoryPath);
    return path;
}

void WBHost::ImportCollected(const QString& path, const CollectedSession& s)
{
    // main thread
    if (path.isEmpty())
    {
        Inst->osdAddMessage(0xFFA0A0, "Could not write the session file");
        return;
    }
    LastSessionPath = path;
    if (Kb.IsOpen() && Cfg.AutoImportSessions)
    {
        QString err;
        if (!Kb.ImportSession(path, s.Sha, s.FrameStart, s.FrameEnd, s.Flags, s.Evidence, s.Assets, err))
            Inst->osdAddMessage(0xFFA0A0, "Session import failed: %s", err.toUtf8().constData());
    }
    AllTimeData.reset();
    Inst->osdAddMessage(0, "Recording session saved");
    emit sessionSaved(path);
    emit recordingChanged();
    emit statusChanged();
}

QString WBHost::KeepSessionHistory(const CollectedSession& s, bool move)
{
    // emulation thread. A history file is renamed when the session ends, and its used part copied
    // when the session goes on; history in memory is written out only when the session file couldn't
    // keep all of it
    Recorder& rec = WB.Rec;
    HistoryView v = rec.LiveHistory();
    bool inFile = rec.HistoryInFile() && HistFile && HistMap;
    if (!v.Used || (!inFile && v.Used <= rec.SessionHistoryMax)) return QString();
    QString path = s.Path;
    path.chop(QString(".dswbs").size());
    path += ".dswbh";
    QDir().mkpath(s.Dir);
    QFile::remove(path);
    HistoryFileTrailer t = MakeHistoryTrailer(v, s.FrameStart, s.FrameEnd);
    bool ok;
    if (inFile && move)
    {
        // the recorder lets go of the file first; the next session maps a new one
        rec.SetHistoryStorage(nullptr, 0);
        HistFile->unmap(HistMap);
        HistMap = nullptr;
        HistMapMB = 0;
        ok = true;
        if (!v.Wrapped())
        {
            ok = HistFile->resize((qint64)v.Used * sizeof(WriteHistoryEntry));
            t.Slots = v.Used;
        }
        ok = ok && HistFile->seek(HistFile->size()) && HistFile->write((const char*)&t, sizeof(t)) == (qint64)sizeof(t);
        HistFile->close();
        // the same disk as the project, normally; across disks Qt copies
        ok = ok && HistFile->rename(path);
        if (!ok) HistFile->remove();
        delete HistFile;
        HistFile = nullptr;
    }
    else
    {
        // slots in ring order: all of them once it wrapped, else the used ones from the first
        u64 nslots = v.Wrapped() ? v.Slots : v.Used;
        t.Slots = nslots;
        QFile f(path);
        ok = f.open(QIODevice::WriteOnly);
        const char* d = (const char*)v.Data;
        for (u64 done = 0, total = nslots * sizeof(WriteHistoryEntry); ok && done < total; )
        {
            qint64 n = (qint64)std::min<u64>(total - done, 64 << 20);
            ok = f.write(d + done, n) == n;
            done += (u64)n;
        }
        ok = ok && f.write((const char*)&t, sizeof(t)) == (qint64)sizeof(t);
        f.close();
        if (!ok) f.remove();
    }
    if (!ok)
    {
        Inst->osdAddMessage(0xFFA0A0, "Could not keep the session's write history in %s", path.toUtf8().constData());
        return QString();
    }
    return path;
}

void WBHost::PruneHistoryFiles(const QString& dir, const QString& keep)
{
    // a saved history open for browsing can't be deleted on Windows: let go of it
    {
        std::lock_guard<std::mutex> l(SavedLock);
        SavedCache.reset();
    }
    QDir d(dir);
    // names start with the time saved: newest first
    QFileInfoList files = d.entryInfoList({"*.dswbh"}, QDir::Files, QDir::Name | QDir::Reversed);
    qint64 budget = (qint64)Cfg.HistoryKeepMB << 20, used = 0;
    for (const QFileInfo& fi : files)
    {
        used += fi.size();
        if (used > budget && QFileInfo(keep) != fi) QFile::remove(fi.absoluteFilePath());
    }
}

std::vector<WBApi::SessionInfo> WBHost::ListSessions() const
{
    std::vector<WBApi::SessionInfo> out;
    QDir d(SessionsDir());
    for (const QFileInfo& fi : d.entryInfoList({"*.dswbs"}, QDir::Files, QDir::Name | QDir::Reversed))
    {
        WBApi::SessionInfo s;
        s.file = fi.fileName();
        s.path = fi.absoluteFilePath();
        s.bytes = (u64)fi.size();
        // yyyyMMdd-HHmmss_f<start>-<end>.dswbs
        QString base = fi.completeBaseName();
        QDateTime when = QDateTime::fromString(base.section('_', 0, 0), "yyyyMMdd-HHmmss");
        s.saved = when.isValid() ? when.toString("yyyy-MM-dd HH:mm:ss") : fi.lastModified().toString("yyyy-MM-dd HH:mm:ss");
        QString frames = base.section('_', 1, 1);
        if (frames.startsWith('f'))
        {
            s.frame_start = frames.mid(1).section('-', 0, 0).toUInt();
            s.frame_end = frames.section('-', 1, 1).toUInt();
        }
        QFile h(d.filePath(base + ".dswbh"));
        HistoryFileTrailer t;
        if (h.open(QIODevice::ReadOnly) && h.size() >= (qint64)sizeof(t) && h.seek(h.size() - sizeof(t)) &&
            h.read((char*)&t, sizeof(t)) == (qint64)sizeof(t))
        {
            // the trailer alone checks the magic and entry size; the slots are checked on opening
            if (!memcmp(t.Magic, "DSWBH1", 6) && t.EntrySize == sizeof(WriteHistoryEntry))
            {
                s.history_file = true;
                s.history_entries = t.Used;
                s.history_wrapped = t.Pos > t.Used;
                s.history_bytes = (u64)h.size();
            }
        }
        out.push_back(s);
    }
    return out;
}

QString WBHost::ResolveSession(const QString& name) const
{
    if (name.contains('/') || name.contains('\\')) return name;
    return QDir(SessionsDir()).filePath(name);
}

SavedHistory::~SavedHistory()
{
    if (File)
    {
        if (Map) File->unmap(Map);
        File->close();
        delete File;
    }
}

std::shared_ptr<const SavedHistory> WBHost::OpenSavedHistory(const QString& path, QString& error)
{
    {
        std::lock_guard<std::mutex> l(SavedLock);
        if (SavedCache && SavedCache->Path == path) return SavedCache;
    }
    auto h = std::make_shared<SavedHistory>();
    h->Path = path;
    if (!QFile::exists(path)) { error = "no session file " + path; return nullptr; }
    QString hp = path;
    if (hp.endsWith(".dswbs")) hp.chop(6);
    hp += ".dswbh";
    if (QFile::exists(hp))
    {
        h->File = new QFile(hp);
        if (h->File->open(QIODevice::ReadOnly)) h->Map = h->File->map(0, h->File->size());
        if (!h->Map || !ReadHistoryFile(h->Map, (size_t)h->File->size(), h->View))
        {
            error = "cannot read the history file " + hp;
            return nullptr;
        }
        h->Source = "history_file";
    }
    else
    {
        h->Bytes = ReadSessionFile(path);
        if (h->Bytes.isEmpty() || !SessionHistory((const u8*)h->Bytes.constData(), h->Bytes.size(), h->View))
        {
            error = "cannot read the session file " + path;
            return nullptr;
        }
        h->Source = "session_file";
    }
    std::lock_guard<std::mutex> l(SavedLock);
    SavedCache = h;
    return h;
}

void WBHost::FinishSession(bool keepRecording)
{
    if (!WB.Rec.Enabled()) return;
    RefreshFuncSnapshot();
    auto cs = std::make_shared<CollectedSession>();
    bool ok = false;
    if (!RunSync([&, this]() { ok = CollectSession(keepRecording, *cs); }, 30000) || !ok) return;
    ImportCollected(StoreSession(*cs), *cs);
    // the ended session took its history file along: a fresh one now, so the state reports it
    if (!keepRecording) RunSync([this]() { ApplyHistoryFile(nullptr); }, 30000);
}

void WBHost::OnGameClosing()
{
    // emulation thread: the game stops, another one boots, or melonDS quits
    if (!WB.Rec.Enabled()) return;
    auto cs = std::make_shared<CollectedSession>();
    if (!CollectSession(false, *cs)) return;
    QString path = StoreSession(*cs);
    QMetaObject::invokeMethod(this, [this, path, cs]() { ImportCollected(path, *cs); }, Qt::QueuedConnection);
}

void WBHost::Shutdown()
{
    if (WB.Rec.Enabled()) FinishSession(false);
}

void WBHost::RecordOnBoot(bool history, bool origin)
{
    RecordAtBoot = true;
    HistoryAtBoot = history;
    OriginAtBoot = origin;
}

QString WBHost::SaveSessionNow()
{
    FinishSession(true);
    return SessionsDir();
}

void WBHost::LoadAllTimeSessions()
{
    if (AllTimeLoading.exchange(true)) return;
    QString dir = SessionsDir();
    QString sha = Status().RomSha1;
    auto merged = std::make_shared<MergedSessions>();
    std::thread([this, dir, sha, merged]()
    {
        QDir d(dir);
        for (const QString& fn : d.entryList({"*.dswbs"}, QDir::Files, QDir::Name))
        {
            QByteArray data = ReadSessionFile(d.filePath(fn));
            if (data.isEmpty()) continue;
            SessionHeader hdr;
            std::vector<SessionSectionView> sections;
            if (!ReadSession((const u8*)data.constData(), data.size(), hdr, sections)) continue;
            char hex[41];
            for (int i = 0; i < 20; i++) snprintf(hex + i * 2, 3, "%02x", hdr.rom_sha1[i]);
            if (sha != QString::fromLatin1(hex, 40)) continue;
            MergeSession((const u8*)data.constData(), data.size(), *merged);
        }
        WB.Dbg.PostJob([this, merged]()
        {
            AllTimeData = merged;
            AllTimeLoading = false;
        });
    }).detach();
}

void WBHost::Pause()
{
    EmuThread* t = Inst->getEmuThread();
    if (!t || !t->emuIsActive()) return;
    if (t->emuIsRunning()) t->emuPause();
}

void WBHost::Resume()
{
    if (WB.Dbg.InBreak())
    {
        WB.Dbg.CancelStep();
        WB.Dbg.Continue();
    }
    EmuThread* t = Inst->getEmuThread();
    if (!t || !t->emuIsActive()) return;
    if (!t->emuIsRunning()) t->emuRun();
}

void WBHost::BreakNow(int cpu)
{
    if (WB.JITActive()) return;
    WB.Dbg.RequestBreak(cpu, "pause");
    EmuThread* t = Inst->getEmuThread();
    if (t && t->emuIsActive() && !t->emuIsRunning()) t->emuRun();
}

void WBHost::StepOver(int cpu)
{
    if (WB.JITActive() || !WB.GetNDS()) return;
    bool call = false;
    RunSync([&]()
    {
        if (!WB.GetNDS()) return;
        u32 pc = WB.Dbg.InBreak() ? WB.Dbg.CurrentBreak().PC : WB.CurrentPC((u32)cpu);
        const ARM& arm = cpu ? (const ARM&)WB.GetNDS()->ARM7 : (const ARM&)WB.GetNDS()->ARM9;
        bool thumb = arm.CPSR & 0x20;
        u32 instr = 0;
        if (!WB.Peek((u32)cpu, pc, 4, instr)) { WB.Dbg.RequestStep(cpu, 1); return; }
        DisasmInsn in;
        Disassemble(pc, instr, thumb, in);
        if (!in.IsCall) { WB.Dbg.RequestStep(cpu, 1); return; }
        Breakpoint bp;
        bp.Kind = BreakKind::Exec;
        bp.Addr = (pc + in.Size) & ~1u;
        bp.Size = 1;
        bp.Cpu = cpu;
        bp.Temporary = true;
        std::string e;
        WB.Dbg.AddBreakpoint(bp, e);
        call = true;
    });
    if (call) Resume();
    else if (WB.Dbg.InBreak()) WB.Dbg.Continue();
    else
    {
        EmuThread* t = Inst->getEmuThread();
        if (t && t->emuIsActive() && !t->emuIsRunning()) t->emuRun();
    }
}

bool WBHost::FunctionOpen(int cpu, u32 addr) const { return OpenMarks.count(MarkKey(cpu, addr)) != 0; }
bool WBHost::FunctionNotched(int cpu, u32 addr) const { return NotchAt.count(MarkKey(cpu, addr)) != 0; }
bool WBHost::FunctionHasNotch(int cpu, u32 funcAddr) const { return NotchedFunc.count(MarkKey(cpu, funcAddr)) != 0; }

void WBHost::IdentifyFunctions()
{
    if (!Kb.IsOpen() || IdentBusy) return;
    IdentBusy = true;
    struct Cand { int cpu; u32 addr; u32 size; bool thumb; bool open; int overlay; };
    Query<std::vector<Cand>>([this]()
    {
        std::vector<Cand> out;
        std::vector<Recorder::ObservedEntry> entries;
        if (!WB.GetNDS() || !WB.Rec.TakeEntries(entries)) return out;
        std::vector<u32> byCpu[2];
        for (const auto& e : entries) byCpu[e.Cpu & 1].push_back(e.Addr & ~1u);
        for (int c = 0; c < 2; c++) std::sort(byCpu[c].begin(), byCpu[c].end());
        NDS* nds = WB.GetNDS();
        u32 mask = nds ? nds->MainRAMMask : 0x3FFFFF;
        for (const auto& e : entries)
        {
            u32 addr = e.Addr & ~1u;
            if (addr < 0x1000) continue;
            bool thumb = e.Thumb;
            const std::vector<u32>& list = byCpu[e.Cpu & 1];
            u32 size = thumb ? 2u : 4u;
            u32 limit = addr + 0x800;
            bool open = false;
            for (u32 a = addr; ; )
            {
                if (a >= limit) { open = true; break; }
                if (a != addr && std::binary_search(list.begin(), list.end(), a)) break;
                u32 word = 0;
                if (!WB.Peek((u32)e.Cpu, a, 4, word)) break;
                DisasmInsn in;
                Disassemble(a, word, thumb, in);
                u32 step = in.Size >= 2 ? in.Size : (thumb ? 2u : 4u);
                size = (a + step) - addr;
                if (in.IsReturn && !in.Conditional)
                {
                    // a trailing literal pool belongs to the function: it is read, never executed
                    u32 p = a + step;
                    while (true)
                    {
                        const ByteRecord* r = WB.Rec.Get(WB.Translate((u32)e.Cpu, p));
                        if (!r || r->exec_count || !(r->flags & ByteFlag::LiteralPool)) break;
                        p += 4;
                    }
                    size = p - addr;
                    break;
                }
                a += step;
            }
            int ov = -1;
            if ((addr >> 24) == 0x02)
            {
                int o = WB.Rec.OverlayAt(addr & mask);
                if (o >= 0 && !(o >> 15)) ov = o;
            }
            out.push_back({(int)e.Cpu, addr, size, thumb, open, ov});
        }
        if (out.empty()) out.push_back({0, 0, 0, false, false, -1});
        return out;
    }, this, [this](std::vector<Cand> cands)
    {
        IdentBusy = false;
        if (cands.empty()) return;
        std::set<quint64> open, notchAt, notched;
        std::vector<std::pair<qint64, u32>> resizes;
        std::vector<WBKB::KnowledgeStore::AutoFunction> batch;
        for (int cpu = 0; cpu < 2; cpu++)
        {
            std::vector<WBSpan::Span> spans;
            for (const WBKB::Function& fn : Kb.Functions())
            {
                const WBKB::Module* m = Kb.ModuleById(fn.ModuleId);
                if ((m ? m->CpuNum() : 0) != cpu) continue;
                bool automatic = fn.AuthorKind == "system" && fn.Name.startsWith("sub_", Qt::CaseInsensitive);
                spans.push_back({fn.Addr, std::max<u32>(fn.Size, fn.Thumb() ? 2u : 4u), automatic});
            }
            std::vector<WBSpan::Entry> entries;
            std::map<u32, const Cand*> byAddr;
            for (const Cand& c : cands)
            {
                if (c.cpu != cpu || !c.addr) continue;
                entries.push_back({c.addr, c.size, c.open});
                byAddr[c.addr] = &c;
            }
            WBSpan::Decision d = WBSpan::Decide(spans, entries);
            for (const WBSpan::Resize& r : d.Resizes)
                if (const WBKB::Function* f = Kb.FunctionStarting(cpu, r.Addr))
                    resizes.push_back({f->Id, r.NewSize});
            for (const WBSpan::Entry& e : d.Create)
                if (const Cand* c = byAddr[e.Addr])
                    batch.push_back({c->cpu, e.Addr, e.Size, c->thumb, c->overlay});
            for (u32 a : d.Open) open.insert(MarkKey(cpu, a));
            for (const WBSpan::Notch& n : d.Notches)
            {
                notchAt.insert(MarkKey(cpu, n.EntryAddr));
                notched.insert(MarkKey(cpu, n.FuncAddr));
            }
        }
        bool marksChanged = open != OpenMarks || notchAt != NotchAt || notched != NotchedFunc;
        OpenMarks = std::move(open);
        NotchAt = std::move(notchAt);
        NotchedFunc = std::move(notched);
        QString err;
        int resized = Kb.ReviseAutoSizes(resizes, err);
        int n = Kb.DefineAutoFunctions(batch, err);
        if (n || resized || marksChanged) emit knowledgeChanged();
    });
}

void WBHost::Step(int cpu, u32 count)
{
    if (WB.JITActive()) return;
    WB.Dbg.RequestStep(cpu, count);
    if (WB.Dbg.InBreak()) WB.Dbg.Continue();
    else
    {
        EmuThread* t = Inst->getEmuThread();
        if (t && t->emuIsActive() && !t->emuIsRunning()) t->emuRun();
    }
}

void WBHost::RunFrames(u32 n, std::function<void(bool)> done)
{
    {
        std::lock_guard<std::mutex> l(InputLock);
        if (FramesDoneCb) { auto old = std::move(FramesDoneCb); FramesDoneCb = nullptr; old(true); }
        FramesToRun = std::max<u32>(n, 1);
        FramesActive = true;
        FramesInterrupted = false;
        FramesDoneCb = std::move(done);
    }
    Resume();
}

void WBHost::SendInput(u32 keymask, bool touch, u16 x, u16 y, bool lid, u32 frames, std::function<void(bool, bool)> done)
{
    EmuThread* t = Inst->getEmuThread();
    bool wasPaused = t && t->emuIsActive() && !t->emuIsRunning();
    {
        std::lock_guard<std::mutex> l(InputLock);
        if (InputDoneCb) { auto old = std::move(InputDoneCb); InputDoneCb = nullptr; old(AgentOverridden, true); }
        AgentKeys = keymask;
        AgentTouch = touch;
        AgentX = x;
        AgentY = y;
        AgentLid = lid;
        AgentFrames = std::max<u32>(frames, 1);
        AgentOverridden = false;
        AgentStarted = false;
        InputDoneCb = std::move(done);
        PauseAfterInput = wasPaused;
    }
    if (lid)
        RunOnEmu([this]() { if (NDS* nds = WB.GetNDS()) nds->SetLidClosed(true); });
    if (wasPaused) Resume();
}

bool WBHost::Boot(const QString& path, QString& error)
{
    EmuThread* t = Inst->getEmuThread();
    QString p = path.isEmpty() ? CurRomPath : path;
    if (p.isEmpty()) { error = "no ROM path"; return false; }
    if (!QFileInfo::exists(p)) { error = "file not found: " + p; return false; }
    if (WB.Dbg.InBreak()) WB.Dbg.Continue();
    int res = t->bootROM(QStringList{p}, error);
    if (!res) return false;
    if (Inst->getMainWindow()) Inst->getMainWindow()->onWorkbenchBooted();
    return true;
}

bool WBHost::Reset(QString& error)
{
    EmuThread* t = Inst->getEmuThread();
    if (!t->emuIsActive()) { error = "no game running"; return false; }
    if (WB.Dbg.InBreak()) WB.Dbg.Continue();
    t->emuReset();
    return true;
}

QString WBHost::SavestatePath(int slot)
{
    return QString::fromStdString(Inst->getSavestateName(slot));
}

bool WBHost::SaveState(const QString& path, QString& error)
{
    EmuThread* t = Inst->getEmuThread();
    if (!t->emuIsActive()) { error = "no game running"; return false; }
    if (!t->saveState(path)) { error = "savestate failed"; return false; }
    return true;
}

bool WBHost::LoadState(const QString& path, QString& error)
{
    EmuThread* t = Inst->getEmuThread();
    if (!t->emuIsActive()) { error = "no game running"; return false; }
    if (!QFileInfo::exists(path)) { error = "file not found: " + path; return false; }
    if (WB.Dbg.InBreak()) WB.Dbg.Continue();
    if (!t->loadState(path)) { error = "loading the savestate failed"; return false; }
    return true;
}

// ---------------------------------------------------------------- knowledge store

void WBHost::OpenKnowledgeStore()
{
    if (!WB.Rom.Valid()) return;
    QString path = Cfg.KnowledgeStoreOverride.isEmpty() ? Cfg.KnowledgeStorePath : Cfg.KnowledgeStoreOverride;
    if (path.isEmpty())
    {
        QString sha = QString::fromStdString(WB.Rom.Sha1Hex());
        QString code = QString::fromLatin1(WB.Rom.GameCode);
        path = QDir(emuDirectory).filePath(QString("workbench/%1_%2/knowledge.db").arg(code.isEmpty() ? "GAME" : code, sha.left(12)));
    }
    QString err;
    if (!Kb.Open(path, WB.Rom, Cfg.AuthorName, err))
    {
        Inst->osdAddMessage(0xFFA0A0, "Knowledge store: %s", err.toUtf8().constData());
        return;
    }
    SeedArchiveAssets();
    emit knowledgeChanged();
}

void WBHost::PollKB()
{
    Kb.CheckExternalChanges();
}

// ---------------------------------------------------------------- UI

WorkbenchWindow* WBHost::Window(bool create)
{
    if (!Win && create) Win = new WorkbenchWindow(this);
    return Win;
}

void WBHost::ShowWindow()
{
    WorkbenchWindow* w = Window(true);
    w->show();
    w->raise();
    w->activateWindow();
    // single-key shortcuts (N ; P D G) act on the disassembly, so start there
    if (w->Disasm()) w->Disasm()->setFocus();
}

QString WBHost::TitlePrefix() const
{
    WBStatus s = Status();
    QString p;
    if (s.Recording) p += "[REC] ";
    if (s.EmuState == WBStatus::Break) p += "[BREAK] ";
    return p;
}

void WBHost::StartApi(int port, bool memwrite)
{
    // the saved settings are the settings dialog's to change: --api options last one run
    Server->Start(port, memwrite);
}

int WBHost::OverlayLoadedAt(u32 addr) const
{
    if ((addr >> 24) != 0x02) return -1;
    NDS* nds = WB.GetNDS();
    u32 mask = nds ? nds->MainRAMMask : 0x3FFFFF;
    int o = WB.Rec.OverlayAt(addr & mask);
    if (o < 0 || (o >> 15)) return -1;
    return o;
}
