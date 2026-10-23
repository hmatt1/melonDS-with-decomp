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

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include "ApiServer.h"
#include "WBHost.h"
#include "WorkbenchWindow.h"
#include "WBQueries.h"
#include "KbQueries.h"
#include "Dialogs.h"
#include "WBApi.h"
#include "WBAssets.h"
#include "NDS.h"
#include "main.h"

using namespace melonDS;
using namespace melonDS::WB;

enum
{
    ErrParse = -32700,
    ErrInvalidRequest = -32600,
    ErrMethod = -32601,
    ErrParams = -32602,
    ErrServer = -32000,
    ErrUnauthorized = -32001,
    ErrUnavailable = -32002,
    ErrDisabled = -32003,
    ErrNoGame = -32004,
};

ApiServer::ApiServer(WBHost* host, QObject* parent) : QObject(parent), Host(host)
{
    connect(Host, &WBHost::breakpointHit, this, [this](int id, int cpu, quint32 pc)
    {
        if (id < 0) return;   // steps and pause requests are reported as "paused"
        WBStatus s = Host->Status();
        WBApi::EventBreakpointHit ev;
        ev.hit.id = id;
        ev.hit.cpu = WBQ::CpuName((u32)cpu);
        ev.hit.pc = pc;
        ev.hit.frame = s.BreakInfo.Frame;
        ev.hit.access_addr = s.BreakInfo.AccessAddr;
        ev.hit.access_value = s.BreakInfo.AccessValue;
        ev.hit.access_size = s.BreakInfo.AccessSize;
        ev.hit.write = s.BreakInfo.Reason.find("watch") != std::string::npos;
        ev.break_info.cpu = WBQ::CpuName(s.BreakInfo.Cpu);
        ev.break_info.pc = s.BreakInfo.PC;
        ev.break_info.reason = QString::fromStdString(s.BreakInfo.Reason);
        ev.break_info.breakpoint_id = s.BreakInfo.BreakpointId;
        ev.break_info.access_addr = s.BreakInfo.AccessAddr;
        ev.break_info.access_value = s.BreakInfo.AccessValue;
        ev.break_info.access_size = s.BreakInfo.AccessSize;
        ev.break_info.frame = s.BreakInfo.Frame;
        Broadcast("breakpoint_hit", ev.toJson());
    });
    connect(Host, &WBHost::breakChanged, this, [this](bool inBreak)
    {
        WBApi::EventState ev;
        ev.state = inBreak ? "break" : "running";
        ev.frame = Host->Status().Frame;
        ev.reason = inBreak ? QString::fromStdString(Host->Status().BreakInfo.Reason) : "continue";
        Broadcast(inBreak ? "paused" : "resumed", ev.toJson());
    });
    connect(Host, &WBHost::pausedChanged, this, [this](bool paused)
    {
        WBApi::EventState ev;
        ev.state = paused ? "paused" : "running";
        ev.frame = Host->Status().Frame;
        ev.reason = paused ? "pause" : "resume";
        Broadcast(paused ? "paused" : "resumed", ev.toJson());
    });
    connect(Host, &WBHost::recordingChanged, this, [this]()
    {
        Host->Query<WBApi::RecordingState>([this]() { return WBQ::Recording(Host->Core()); }, this, [this](WBApi::RecordingState r)
        {
            r.history_keep_mb = Host->Settings().HistoryKeepMB;
            WBApi::EventRecording ev;
            ev.recording = r;
            ev.frame = Host->Status().Frame;
            Broadcast("recording_changed", ev.toJson());
        });
    });
    connect(Host, &WBHost::framesDone, this, [this](bool interrupted)
    {
        WBApi::EventState ev;
        ev.state = interrupted ? "interrupted" : "paused";
        ev.frame = Host->Status().Frame;
        ev.reason = "run_frames";
        Broadcast("frames_done", ev.toJson());
    });
    connect(Host, &WBHost::inputDone, this, [this](bool overridden)
    {
        WBApi::EventState ev;
        ev.state = Host->EmuRunning() ? "running" : "paused";
        ev.frame = Host->Status().Frame;
        ev.reason = overridden ? "overridden_by_person" : "input";
        Broadcast("input_done", ev.toJson());
    });
}

ApiServer::~ApiServer()
{
    Stop();
}

bool ApiServer::Start(int port, bool memoryWrite)
{
    Stop();
    MemWrite = memoryWrite;
    ReadServer = new QTcpServer(this);
    ControlServer = new QTcpServer(this);
    if (!ReadServer->listen(QHostAddress::LocalHost, (quint16)port) || !ControlServer->listen(QHostAddress::LocalHost, (quint16)(port ? port + 1 : 0)))
    {
        printf("Workbench API: cannot listen on 127.0.0.1:%d/%d\n", port, port + 1);
        Stop();
        return false;
    }
    RPort = ReadServer->serverPort();
    CPort = ControlServer->serverPort();
    connect(ReadServer, &QTcpServer::newConnection, this, [this]() { OnNewConnection(false); });
    connect(ControlServer, &QTcpServer::newConnection, this, [this]() { OnNewConnection(true); });

    QByteArray tok(16, 0);
    for (int i = 0; i < 16; i++) tok[i] = (char)QRandomGenerator::system()->bounded(256);
    Tok = QString::fromLatin1(tok.toHex());

    QDir().mkpath(QDir(emuDirectory).filePath("workbench"));
    InfoPath = QDir(emuDirectory).filePath("workbench/api.json");
    QJsonObject info;
    info["read_port"] = RPort;
    info["control_port"] = CPort;
    info["token"] = Tok;
    info["pid"] = (double)QCoreApplication::applicationPid();
    info["api_version"] = WBApi::ApiVersion;
    info["memory_write"] = MemWrite;
    QFile f(InfoPath);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        f.write(QJsonDocument(info).toJson());
        f.close();
    }
    printf("Workbench API: read 127.0.0.1:%d, control 127.0.0.1:%d, token in %s\n", RPort, CPort, InfoPath.toUtf8().constData());
    fflush(stdout);
    return true;
}

void ApiServer::Stop()
{
    Conns.CloseAll();
    delete ReadServer;
    delete ControlServer;
    ReadServer = ControlServer = nullptr;
    if (!InfoPath.isEmpty()) QFile::remove(InfoPath);
    RPort = CPort = 0;
}

void ApiServer::OnNewConnection(bool control)
{
    QTcpServer* srv = control ? ControlServer : ReadServer;
    while (QTcpSocket* s = srv->nextPendingConnection())
    {
        Conns.Add(s, control);
        connect(s, &QTcpSocket::readyRead, this, [this, s]() { OnReadyRead(s); });
    }
}

void ApiServer::OnReadyRead(QTcpSocket* s)
{
    Conn* c = Conns.Find(s);
    if (!c) return;
    c->Buffer += s->readAll();
    if (c->Buffer.size() > 16 * 1024 * 1024)
    {
        Error(s, QJsonValue(), ErrInvalidRequest, "request too large");
        s->disconnectFromHost();
        return;
    }
    int nl;
    while ((nl = c->Buffer.indexOf('\n')) >= 0)
    {
        QByteArray line = c->Buffer.left(nl).trimmed();
        c->Buffer.remove(0, nl + 1);
        if (line.isEmpty()) continue;
        QJsonParseError perr;
        QJsonDocument doc = QJsonDocument::fromJson(line, &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject())
        {
            Error(s, QJsonValue(), ErrParse, "parse error: " + perr.errorString());
            continue;
        }
        QJsonObject req = doc.object();
        QJsonValue id = req.value("id");
        QString method = req.value("method").toString();
        if (method.isEmpty()) { Error(s, id, ErrInvalidRequest, "missing method"); continue; }
        QJsonObject params = req.value("params").toObject();
        Handle(c, id, method, params);
        c = Conns.Find(s);
        if (!c) return;
    }
}

void ApiServer::Reply(QPointer<QTcpSocket> s, const QJsonValue& id, const QJsonObject& result)
{
    if (!s || id.isUndefined() || id.isNull()) return;
    QJsonObject o;
    o["jsonrpc"] = "2.0";
    o["id"] = id;
    o["result"] = result;
    s->write(QJsonDocument(o).toJson(QJsonDocument::Compact) + "\n");
}

void ApiServer::Error(QPointer<QTcpSocket> s, const QJsonValue& id, int code, const QString& msg)
{
    if (!s) return;
    QJsonObject e;
    e["code"] = code;
    e["message"] = msg;
    QJsonObject o;
    o["jsonrpc"] = "2.0";
    o["id"] = id.isUndefined() ? QJsonValue() : id;
    o["error"] = e;
    s->write(QJsonDocument(o).toJson(QJsonDocument::Compact) + "\n");
}

void ApiServer::Broadcast(const QString& event, const QJsonObject& params)
{
    QJsonObject o;
    o["jsonrpc"] = "2.0";
    o["method"] = "event." + event;
    o["params"] = params;
    QByteArray line = QJsonDocument(o).toJson(QJsonDocument::Compact) + "\n";
    for (Conn* c : Conns.List())
        if (c->Sock && c->Authed && (c->AllEvents || c->Events.contains(event)))
            c->Sock->write(line);
}

// a value scan an API connection keeps between search.scan calls
struct ApiScan
{
    unsigned Id = 0;
    WBQ::ScanState State;
    bool Wide = false;
    bool Busy = false;      // a scan of it is running on the emulation thread
};

static WBApi::BreakpointInfo BreakpointJson(const Breakpoint& bp)
{
    WBApi::BreakpointInfo i;
    i.id = bp.Id;
    i.kind = BreakKindName((u8)bp.Kind);
    i.addr = bp.Addr;
    i.size = bp.Size;
    i.cpu = bp.Cpu < 0 ? "any" : WBQ::CpuName((u32)bp.Cpu);
    i.condition = QString::fromStdString(bp.Condition);
    i.enabled = bp.Enabled;
    i.temporary = bp.Temporary;
    i.hit_count = bp.HitCount;
    return i;
}

static u32 ButtonMask(const std::vector<QString>& buttons, bool& lid, QString& err)
{
    // active low, bit order of melonDS' input mask: A B Select Start Right Left Up Down R L X Y
    static const char* names[12] = {"a", "b", "select", "start", "right", "left", "up", "down", "r", "l", "x", "y"};
    u32 mask = 0xFFF;
    lid = false;
    for (const QString& b : buttons)
    {
        QString l = b.toLower();
        if (l == "lid") { lid = true; continue; }
        int i = 0;
        for (; i < 12; i++) if (l == names[i]) break;
        if (i == 12) { err = "unknown button '" + b + "'"; return 0; }
        mask &= ~(1u << i);
    }
    return mask;
}

void ApiServer::Handle(Conn* c, const QJsonValue& id, const QString& method, const QJsonObject& params)
{
    QPointer<QTcpSocket> sock = c->Sock;
    const WBApi::MethodInfo* mi = WBApi::FindMethod(method);
    if (!mi) { Error(sock, id, ErrMethod, "method not found: " + method); return; }
    if (mi->endpoint == WBApi::Endpoint::Control && !c->Control) { Error(sock, id, ErrMethod, method + " is only available on the control endpoint"); return; }
    if (mi->endpoint == WBApi::Endpoint::Read && c->Control && method != "status.get")
    {
        // the control endpoint accepts read calls too, so a single control connection can drive a session
    }
    if (!c->Authed && method != "auth") { Error(sock, id, ErrUnauthorized, "authenticate first: call auth with the token from " + InfoPath); return; }

    QString err;
    auto bad = [&](const QString& e) { Error(sock, id, ErrParams, e); };
    WB::Workbench& wb = Host->Core();

    if (method == "auth")
    {
        WBApi::AuthParams p;
        if (!WBApi::AuthParams::fromJson(params, p, err)) return bad(err);
        if (p.token != Tok) { Error(sock, id, ErrUnauthorized, "bad token"); return; }
        c->Authed = true;
        c->AgentName = p.agent.trimmed().isEmpty() ? QString("agent") : p.agent.trimmed();
        c->AgentSession = QString("%1#%2").arg(c->AgentName).arg(++ConnCounter);
        WBApi::AuthResult r;
        r.ok = true;
        r.endpoint = c->Control ? "control" : "read";
        r.api_version = WBApi::ApiVersion;
        r.agent_session = c->AgentSession;
        Reply(sock, id, r.toJson());
        return;
    }
    if (method == "events.subscribe")
    {
        WBApi::SubscribeParams p;
        if (!WBApi::SubscribeParams::fromJson(params, p, err)) return bad(err);
        c->Events.clear();
        c->AllEvents = p.events.empty();
        for (const QString& e : p.events)
        {
            bool known = false;
            for (const char* k : WBApi::Events) if (e == QLatin1String(k)) known = true;
            if (!known) return bad("unknown event '" + e + "'");
            c->Events.insert(e);
        }
        WBApi::Ok r;
        r.ok = true;
        Reply(sock, id, r.toJson());
        return;
    }

    if (method == "status.get")
    {
        bool memw = MemWrite;
        u32 keep = Host->Settings().HistoryKeepMB;
        Host->Query<WBApi::Status>([this, memw, keep]()
        {
            WB::Workbench& w = Host->Core();
            WBStatus s = Host->Status();
            WBApi::Status st;
            st.api_version = WBApi::ApiVersion;
            static const char* states[] = {"no_game", "running", "paused", "break"};
            st.state = states[s.EmuState];
            if (w.Dbg.InBreak()) st.state = "break";
            st.frame = w.Frame();
            st.rom_sha1 = s.RomSha1;
            st.rom_title = s.RomTitle;
            st.rom_gamecode = s.RomCode;
            st.recording = WBQ::Recording(w);
            st.recording.history_keep_mb = keep;
            if (w.GetNDS())
            {
                st.registers.push_back(WBQ::Registers(w, 0));
                st.registers.push_back(WBQ::Registers(w, 1));
                st.tcm = WBQ::Tcm(w);
            }
            if (w.Dbg.InBreak())
            {
                BreakState b = w.Dbg.CurrentBreak();
                WBApi::BreakInfo bi;
                bi.cpu = WBQ::CpuName(b.Cpu);
                bi.pc = b.PC;
                bi.reason = QString::fromStdString(b.Reason);
                bi.breakpoint_id = b.BreakpointId;
                bi.access_addr = b.AccessAddr;
                bi.access_value = b.AccessValue;
                bi.access_size = b.AccessSize;
                bi.frame = b.Frame;
                st.break_info = bi;
            }
            st.fps = s.Fps;
            st.jit_enabled = w.JITActive();
            st.memory_write_enabled = memw;
            return st;
        }, this, [this, sock, id](WBApi::Status st) { Reply(sock, id, st.toJson()); });
        return;
    }

    if (method == "memory.read")
    {
        WBApi::MemoryReadParams p;
        if (!WBApi::MemoryReadParams::fromJson(params, p, err)) return bad(err);
        Host->Query<std::pair<bool, QJsonObject>>([this, p]()
        {
            WBApi::MemoryReadResult r;
            QString e;
            if (!WBQ::MemoryRead(Host->Core(), p, r, e)) { QJsonObject o; o["error"] = e; return std::make_pair(false, o); }
            return std::make_pair(true, r.toJson());
        }, this, [this, sock, id](std::pair<bool, QJsonObject> r)
        {
            if (!r.first) Error(sock, id, ErrParams, r.second.value("error").toString());
            else Reply(sock, id, r.second);
        });
        return;
    }

    if (method == "metadata.get")
    {
        WBApi::MetadataParams p;
        if (!WBApi::MetadataParams::fromJson(params, p, err)) return bad(err);
        if (p.scope.toLower() == "all") Host->LoadAllTimeSessions();
        u32 hcpu = 0;
        QString herr;
        WBQ::ParseCpu(p.cpu, hcpu, herr);
        WBQ::KbHints hints = WBQ::KbHints::Collect(Host->KB(), (int)hcpu, p.addr, p.addr + p.size, hcpu ? -1 : Host->OverlayLoadedAt(p.addr));
        WBQ::AnalysisHints guessed = Host->AnalysisNear((int)hcpu, p.addr, p.addr + std::max<u32>(p.size, 1));
        Host->Query<std::pair<bool, QJsonObject>>([this, p, hints, guessed]()
        {
            WBApi::MetadataResult r;
            QString e;
            if (!WBQ::Metadata(Host->Core(), Host->AllTime(), p, r, e, &hints, &guessed)) { QJsonObject o; o["error"] = e; return std::make_pair(false, o); }
            return std::make_pair(true, r.toJson());
        }, this, [this, sock, id](std::pair<bool, QJsonObject> r)
        {
            if (!r.first) Error(sock, id, ErrParams, r.second.value("error").toString());
            else Reply(sock, id, r.second);
        });
        return;
    }

    if (method == "sessions.list")
    {
        WBApi::SessionListResult r;
        r.sessions = Host->ListSessions();
        r.dir = Host->SessionsDir();
        r.history_keep_mb = Host->Settings().HistoryKeepMB;
        Reply(sock, id, r.toJson());
        return;
    }

    if (method == "writes.list")
    {
        WBApi::WritesListParams p;
        if (!WBApi::WritesListParams::fromJson(params, p, err)) return bad(err);
        if (!p.session.isEmpty())
        {
            // a saved session: read from its files, away from the emulation thread
            QString path = Host->ResolveSession(p.session);
            Host->Background<std::pair<bool, QJsonObject>>([this, p, path]()
            {
                WBApi::WritesListResult r;
                QString e;
                std::shared_ptr<const SavedHistory> h = Host->OpenSavedHistory(path, e);
                if (!h || !WBQ::WritesIn(h->View, p, r, e)) { QJsonObject o; o["error"] = e; return std::make_pair(false, o); }
                r.history_source = h->Source;
                return std::make_pair(true, r.toJson());
            }, this, [this, sock, id](std::pair<bool, QJsonObject> r)
            {
                if (!r.first) Error(sock, id, ErrParams, r.second.value("error").toString());
                else Reply(sock, id, r.second);
            });
            return;
        }
        Host->Query<std::pair<bool, QJsonObject>>([this, p]()
        {
            WBApi::WritesListResult r;
            QString e;
            if (!WBQ::Writes(Host->Core(), p, r, e)) { QJsonObject o; o["error"] = e; return std::make_pair(false, o); }
            return std::make_pair(true, r.toJson());
        }, this, [this, sock, id](std::pair<bool, QJsonObject> r)
        {
            if (!r.first) Error(sock, id, ErrParams, r.second.value("error").toString());
            else Reply(sock, id, r.second);
        });
        return;
    }

    if (method == "coverage.get")
    {
        WBApi::CoverageParams p;
        if (!WBApi::CoverageParams::fromJson(params, p, err)) return bad(err);
        if (p.scope.toLower() == "all") Host->LoadAllTimeSessions();
        std::vector<WBQ::FuncRange> known;
        for (const WBKB::Function& f : Host->KB().Functions())
        {
            const WBKB::Module* m = Host->KB().ModuleById(f.ModuleId);
            known.push_back({f.Addr, f.Addr + std::max<u32>(f.Size, f.Thumb() ? 2 : 4), m ? m->CpuNum() : 0});
        }
        Host->Query<std::pair<bool, QJsonObject>>([this, p, known]()
        {
            WBApi::CoverageResult r;
            QString e;
            if (!WBQ::Coverage(Host->Core(), Host->AllTime(), p, known, r, e)) { QJsonObject o; o["error"] = e; return std::make_pair(false, o); }
            return std::make_pair(true, r.toJson());
        }, this, [this, sock, id](std::pair<bool, QJsonObject> r)
        {
            if (!r.first) Error(sock, id, ErrParams, r.second.value("error").toString());
            else Reply(sock, id, r.second);
        });
        return;
    }

    if (method == "calls.get")
    {
        WBApi::CallsParams p;
        if (!WBApi::CallsParams::fromJson(params, p, err)) return bad(err);
        u32 cpu;
        if (!WBQ::ParseCpu(p.cpu, cpu, err)) return bad(err);
        u32 entry = p.addr & ~1u, size = 0;
        bool known = false;
        int ov = Host->OverlayLoadedAt(p.addr);
        if (const WBKB::Function* f = Host->KB().FunctionAt((int)cpu, p.addr & ~1u, ov)) { entry = f->Addr; size = f->Size; known = true; }
        u32 limit = std::max<u32>(p.limit, 1);
        Host->Query<QJsonObject>([this, cpu, entry, size, limit, known]()
        {
            WBApi::CallsResult r;
            u32 e = known ? entry : WBQ::InferFunctionEntry(Host->Core(), Host->AllTime(), cpu, entry);
            WBQ::Calls(Host->Core(), Host->AllTime(), cpu, e, size, limit, r);
            return r.toJson();
        }, this, [this, sock, id](QJsonObject r) { Reply(sock, id, r); });
        return;
    }

    if (method == "callstack.get")
    {
        WBApi::CallStackParams p;
        if (!WBApi::CallStackParams::fromJson(params, p, err)) return bad(err);
        u32 cpu;
        if (!WBQ::ParseCpu(p.cpu, cpu, err)) return bad(err);
        Host->Query<WBApi::CallStackResult>([this, cpu]() { return WBQ::CallStack(Host->Core(), cpu); }, this,
            [this, sock, id, cpu](WBApi::CallStackResult r)
        {
            for (auto& f : r.frames) f.name = Host->KB().NameAt((int)cpu, f.callee, Host->OverlayLoadedAt(f.callee));
            Reply(sock, id, r.toJson());
        });
        return;
    }

    if (method == "origin.get")
    {
        WBApi::OriginParams p;
        if (!WBApi::OriginParams::fromJson(params, p, err)) return bad(err);
        WBQ::ArchiveCtx arc {Host->Archive(), Host->ArchiveStatus()};
        WBQ::ContainerCtx cont {Host->Containers(), Host->ContainerStatus()};
        Host->Query<std::pair<bool, QJsonObject>>([this, p, arc, cont]()
        {
            WBApi::OriginResult r;
            QString e;
            if (!WBQ::Origin(Host->Core(), p, r, e, arc, cont)) { QJsonObject o; o["error"] = e; return std::make_pair(false, o); }
            return std::make_pair(true, r.toJson());
        }, this, [this, sock, id](std::pair<bool, QJsonObject> r)
        {
            if (!r.first) Error(sock, id, ErrParams, r.second.value("error").toString());
            else Reply(sock, id, r.second);
        });
        return;
    }

    if (method == "origin.label")
    {
        WBApi::OriginLabelParams p;
        if (!WBApi::OriginLabelParams::fromJson(params, p, err)) return bad(err);
        Host->Query<std::pair<bool, QJsonObject>>([this, p]()
        {
            WBApi::OriginLabelResult r;
            QString e;
            if (!WBQ::LabelTree(Host->Core(), p, r, e)) { QJsonObject o; o["error"] = e; return std::make_pair(false, o); }
            return std::make_pair(true, r.toJson());
        }, this, [this, sock, id](std::pair<bool, QJsonObject> r)
        {
            if (!r.first) Error(sock, id, ErrParams, r.second.value("error").toString());
            else Reply(sock, id, r.second);
        });
        return;
    }

    if (method == "origin.ranges")
    {
        WBApi::OriginRangesParams p;
        if (!WBApi::OriginRangesParams::fromJson(params, p, err)) return bad(err);
        Host->Query<std::pair<bool, QJsonObject>>([this, p]()
        {
            WBApi::OriginRangesResult r;
            QString e;
            if (!WBQ::OriginRanges(Host->Core(), p, r, e)) { QJsonObject o; o["error"] = e; return std::make_pair(false, o); }
            return std::make_pair(true, r.toJson());
        }, this, [this, sock, id](std::pair<bool, QJsonObject> r)
        {
            if (!r.first) Error(sock, id, ErrParams, r.second.value("error").toString());
            else Reply(sock, id, r.second);
        });
        return;
    }

    // ---- knowledge store writes: the same KnowledgeStore methods the window uses, attributed to the agent
    //      (and build.*, which compares functions against the knowledge store)
    if (method.startsWith("kb.") || method.startsWith("build."))
    {
        WBKB::KnowledgeStore& kb = Host->KB();
        if (!kb.IsOpen()) { Error(sock, id, ErrUnavailable, "no knowledge store open (load a ROM first)"); return; }
        QString agentName = c->AgentName, agentSession = c->AgentSession;
        auto reply = [this, sock, id](bool ok, const QString& e, qint64 row)
        {
            if (!ok) { Error(sock, id, ErrParams, e); return; }
            WBApi::KbWriteResult r;
            r.ok = true;
            r.id = (u32)row;
            Reply(sock, id, r.toJson());
        };
        auto cpuOf = [&](const QString& s, u32& cpu) { return WBQ::ParseCpu(s, cpu, err); };
        auto overlayAt = [this](u32 cpu, u32 addr, int given) { return given >= 0 ? given : (cpu == 0 ? Host->OverlayLoadedAt(addr) : -1); };
        auto agent = [&kb, agentName]() { return kb.AgentAuthor(agentName); };

        if (method == "kb.changes.list")
        {
            WBApi::KbChangesParams p;
            if (!WBApi::KbChangesParams::fromJson(params, p, err)) return bad(err);
            WBApi::KbChangesResult r;
            for (const auto& ch : kb.RecentChanges(p.after_id, (int)p.limit))
            {
                WBApi::KbChange x;
                x.id = (u32)ch.Id;
                x.table = ch.Table;
                x.row_id = (u32)ch.Row;
                x.action = ch.Action;
                x.author = ch.Author;
                x.author_kind = ch.AuthorKind;
                x.agent_session = ch.AgentSession;
                x.created_at = ch.CreatedAt;
                x.undone = ch.Undone;
                r.changes.push_back(x);
            }
            Reply(sock, id, r.toJson());
            return;
        }
        // ---- knowledge store reads: what the Functions, Annotations, Bookmarks and Types panels list
        if (method == "kb.list")
        {
            WBApi::KbListParams p;
            if (!WBApi::KbListParams::fromJson(params, p, err)) return bad(err);
            WBApi::KbListResult r;
            if (!WBQ::KbList(kb, p, r, err)) return bad(err);
            if (r.kind == "decode_overrides")
            {
                // conflicts with the recording (the Annotations panel's decode conflicts), then list again with them
                std::vector<WBKB::DecodeOverride> ovs = kb.DecodeOverrides();
                Host->Query<std::map<qint64, QString>>([this, ovs]()
                {
                    std::map<qint64, QString> m;
                    for (const WBKB::DecodeOverride& o : ovs)
                    {
                        QString c = WBQ::DecodeConflict(Host->Core(), (u32)o.Cpu, o.Start, o.End, o.Mode);
                        if (!c.isEmpty()) m[o.Id] = c;
                    }
                    return m;
                }, this, [this, sock, id, p](std::map<qint64, QString> conflicts)
                {
                    WBApi::KbListResult r;
                    QString e;
                    if (!WBQ::KbList(Host->KB(), p, r, e, &conflicts)) Error(sock, id, ErrParams, e);
                    else Reply(sock, id, r.toJson());
                });
                return;
            }
            if (r.functions && !r.functions->empty())
            {
                // the Calls column: how often each entry ran
                std::vector<std::pair<u32, u32>> entries;
                for (const WBApi::KbFunctionInfo& f : *r.functions) entries.push_back({f.cpu == "arm7" ? 1u : 0u, f.addr});
                Host->Query<std::vector<u32>>([this, entries]()
                {
                    std::vector<u32> n;
                    for (auto& e : entries) n.push_back(WBQ::ExecCount(Host->Core(), e.first, e.second));
                    return n;
                }, this, [this, sock, id, r](std::vector<u32> counts) mutable
                {
                    for (size_t i = 0; i < counts.size() && i < r.functions->size(); i++) (*r.functions)[i].exec_count = counts[i];
                    Reply(sock, id, r.toJson());
                });
                return;
            }
            Reply(sock, id, r.toJson());
            return;
        }
        if (method == "kb.function.get")
        {
            WBApi::KbFunctionGetParams p;
            if (!WBApi::KbFunctionGetParams::fromJson(params, p, err)) return bad(err);
            const WBKB::Function* f = nullptr;
            if (p.id)
            {
                f = kb.FunctionById(p.id);
                if (!f) return bad(QString("no function with id %1").arg(p.id));
            }
            else
            {
                u32 cpu;
                if (!cpuOf(p.cpu, cpu)) return bad(err);
                if (!p.addr) return bad("give addr or id");
                f = kb.FunctionAt((int)cpu, p.addr & ~1u, overlayAt(cpu, p.addr, p.overlay));
                if (!f) return bad(QString("no function at %1").arg(p.addr, 8, 16, QChar('0')));
            }
            const WBKB::Module* m = kb.ModuleById(f->ModuleId);
            u32 cpu = m ? (u32)m->CpuNum() : 0;
            // names in the decompiler output: the function's own overlay where it covers the address, as
            // the window does with that overlay's epoch selected; elsewhere the overlay loaded now
            const WBKB::Module* own = m && m->Kind == "overlay" ? m : nullptr;
            auto ovAt = [this, own, cpu](u32 a)
            {
                if (own && a >= own->Load && a < own->End()) return own->OverlayId;
                return cpu == 0 ? Host->OverlayLoadedAt(a) : -1;
            };
            WBApi::KbFunctionDetail d = WBQ::KbFunctionDetail(kb, *f, Host->FunctionSourcePath(*f), Host->Settings().SourceRoot,
                                                              p.source, (int)std::min<u32>(p.attempts, 1000), ovAt);
            u32 entry = f->Addr;
            Host->Query<u32>([this, cpu, entry]() { return WBQ::ExecCount(Host->Core(), cpu, entry); }, this,
                             [this, sock, id, d](u32 n) mutable
            {
                d.function.exec_count = n;
                Reply(sock, id, d.toJson());
            });
            return;
        }
        if (method == "kb.function.define")
        {
            WBApi::KbFunctionParams p;
            if (!WBApi::KbFunctionParams::fromJson(params, p, err)) return bad(err);
            u32 cpu;
            if (!cpuOf(p.cpu, cpu)) return bad(err);
            int ov = overlayAt(cpu, p.addr, p.overlay);
            auto write = [this, &kb, p, cpu, ov, agentName, agentSession, reply](QString mode)
            {
                WBKB::KnowledgeStore::AuthorScope scope(kb, kb.AgentAuthor(agentName), agentSession);
                u32 size = p.size;
                QString name = p.name;
                if (const WBKB::Function* f = kb.FunctionStarting((int)cpu, p.addr & ~1u, ov))
                {
                    if (!size) size = f->Size;                // keep what the caller leaves out
                    if (name.isEmpty()) name = f->Name;
                }
                QString e;
                qint64 row = 0;
                bool ok = kb.UpsertFunction((int)cpu, p.addr & ~1u, size, mode, name, ov, e, &row);
                reply(ok, e, row);
            };
            QString mode = p.mode.toLower();
            if (mode == "arm" || mode == "thumb") { write(mode); return; }
            if (mode != "auto") return bad("mode must be arm, thumb or auto");
            if (p.addr & 1) { write("thumb"); return; }
            // auto: the instruction mode the recording saw at the entry
            u32 addr = p.addr;
            Host->Query<int>([this, cpu, addr]() { return WBQ::RecordedMode(Host->Core(), cpu, addr); }, this,
                             [write](int m) { write(m == 1 ? "thumb" : "arm"); });
            return;
        }
        if (method == "kb.rename" || method == "kb.label.set")
        {
            WBApi::KbNameParams p;
            if (!WBApi::KbNameParams::fromJson(params, p, err)) return bad(err);
            u32 cpu;
            if (!cpuOf(p.cpu, cpu)) return bad(err);
            int ov = overlayAt(cpu, p.addr, p.overlay);
            WBKB::KnowledgeStore::AuthorScope scope(kb, agent(), agentSession);
            QString e;
            bool ok = method == "kb.rename" ? kb.Rename((int)cpu, p.addr, p.name, ov, e) : kb.SetLabel((int)cpu, p.addr, p.name, ov, e);
            reply(ok, e, 0);
            return;
        }
        if (method == "kb.data.define")
        {
            WBApi::KbDataParams p;
            if (!WBApi::KbDataParams::fromJson(params, p, err)) return bad(err);
            u32 cpu;
            if (!cpuOf(p.cpu, cpu)) return bad(err);
            WBKB::KnowledgeStore::AuthorScope scope(kb, agent(), agentSession);
            QString e;
            bool ok = kb.DefineData((int)cpu, p.addr, p.size, p.type, p.name, overlayAt(cpu, p.addr, p.overlay), e);
            const WBKB::DataObject* d = ok ? kb.DataObjectAt((int)cpu, p.addr, overlayAt(cpu, p.addr, p.overlay)) : nullptr;
            reply(ok, e, d ? d->Id : 0);
            return;
        }
        if (method == "kb.type.define")
        {
            WBApi::KbTypeParams p;
            if (!WBApi::KbTypeParams::fromJson(params, p, err)) return bad(err);
            WBKB::KnowledgeStore::AuthorScope scope(kb, agent(), agentSession);
            QString e;
            reply(kb.DefineType(p.definition, e), e, 0);
            return;
        }
        if (method == "kb.annotation.save")
        {
            WBApi::KbAnnotationParams p;
            if (!WBApi::KbAnnotationParams::fromJson(params, p, err)) return bad(err);
            u32 cpu;
            if (!cpuOf(p.cpu, cpu)) return bad(err);
            WBKB::Annotation a;
            a.Id = p.id;
            a.TargetKind = p.target_kind;
            a.Comment = p.comment.trimmed();
            a.Tags = p.tags.trimmed();
            a.TypeName = p.type_name.trimmed();
            a.FieldName = p.field_name.trimmed();
            a.Register = p.reg.toLower();
            a.OverlayId = p.overlay_id;
            a.Savestate = p.savestate.trimmed();
            QString e;
            if (!kb.ResolveAnnotationTarget(a, (int)cpu, p.addr, p.addr != 0, p.end, overlayAt(cpu, p.addr, -1), p.whole_function, e))
                return bad(e);
            WBKB::KnowledgeStore::AuthorScope scope(kb, agent(), agentSession);
            bool ok = kb.SaveAnnotation(a, e);
            reply(ok, e, a.Id);
            return;
        }
        if (method == "kb.annotation.delete")
        {
            WBApi::KbIdParams p;
            if (!WBApi::KbIdParams::fromJson(params, p, err)) return bad(err);
            WBKB::KnowledgeStore::AuthorScope scope(kb, agent(), agentSession);
            QString e;
            reply(kb.DeleteAnnotation(p.id, e), e, p.id);
            return;
        }
        if (method == "kb.bookmark.add")
        {
            WBApi::KbBookmarkParams p;
            if (!WBApi::KbBookmarkParams::fromJson(params, p, err)) return bad(err);
            u32 cpu;
            if (!cpuOf(p.cpu, cpu)) return bad(err);
            WBKB::KnowledgeStore::AuthorScope scope(kb, agent(), agentSession);
            QString e;
            qint64 bid = 0;
            bool ok = kb.AddBookmark((int)cpu, p.addr, p.name, e, &bid);
            reply(ok, e, bid);
            return;
        }
        // the function containing an address (as the panels resolve it)
        auto functionAt = [&](const WBApi::KbAddrParams& p, u32& cpu, QString& e) -> const WBKB::Function*
        {
            if (!WBQ::ParseCpu(p.cpu, cpu, e)) return nullptr;
            const WBKB::Function* f = kb.FunctionAt((int)cpu, p.addr & ~1u, overlayAt(cpu, p.addr, p.overlay));
            if (!f) e = QString("no function at %1").arg(p.addr, 8, 16, QChar('0'));
            return f;
        };
        if (method == "kb.decode.set")
        {
            WBApi::KbDecodeParams p;
            if (!WBApi::KbDecodeParams::fromJson(params, p, err)) return bad(err);
            u32 cpu;
            if (!cpuOf(p.cpu, cpu)) return bad(err);
            WBKB::KnowledgeStore::AuthorScope scope(kb, agent(), agentSession);
            QString e;
            reply(kb.SetDecodeOverride((int)cpu, p.start, p.end, p.mode, overlayAt(cpu, p.start, p.overlay), e), e, 0);
            return;
        }
        if (method == "kb.bookmark.delete")
        {
            WBApi::KbIdParams p;
            if (!WBApi::KbIdParams::fromJson(params, p, err)) return bad(err);
            WBKB::KnowledgeStore::AuthorScope scope(kb, agent(), agentSession);
            QString e;
            reply(kb.DeleteBookmark(p.id, e), e, p.id);
            return;
        }
        if (method == "kb.function.update")
        {
            WBApi::KbFunctionUpdateParams p;
            if (!WBApi::KbFunctionUpdateParams::fromJson(params, p, err)) return bad(err);
            WBApi::KbAddrParams a;
            a.addr = p.addr; a.cpu = p.cpu; a.overlay = p.overlay;
            u32 cpu;
            QString e;
            const WBKB::Function* f = functionAt(a, cpu, e);
            if (!f) return bad(e);
            QJsonObject fields;
            auto opt = [&](const char* key, const std::optional<QString>& v)
            {
                if (v) fields[key] = v->isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(*v);
            };
            opt("name", p.name);
            opt("signature", p.signature);
            opt("subsystem", p.subsystem);
            opt("source_file", p.source_file);
            if (fields.isEmpty()) return bad("nothing to change: give name, signature, subsystem or source_file");
            qint64 id = f->Id;
            WBKB::KnowledgeStore::AuthorScope scope(kb, agent(), agentSession);
            reply(kb.UpdateFunction(id, fields, e), e, id);
            return;
        }
        if (method == "kb.function.delete" || method == "kb.data.delete" || method == "kb.label.delete")
        {
            WBApi::KbAddrParams p;
            if (!WBApi::KbAddrParams::fromJson(params, p, err)) return bad(err);
            u32 cpu;
            QString e;
            if (!WBQ::ParseCpu(p.cpu, cpu, e)) return bad(e);
            int ov = overlayAt(cpu, p.addr, p.overlay);
            WBKB::KnowledgeStore::AuthorScope scope(kb, agent(), agentSession);
            if (method == "kb.function.delete")
            {
                const WBKB::Function* f = kb.FunctionAt((int)cpu, p.addr & ~1u, ov);
                if (!f) return bad(QString("no function at %1").arg(p.addr, 8, 16, QChar('0')));
                qint64 id = f->Id;
                reply(kb.DeleteFunction(id, e), e, id);
            }
            else if (method == "kb.data.delete")
            {
                const WBKB::DataObject* d = kb.DataObjectAt((int)cpu, p.addr, ov);
                if (!d) return bad(QString("no data object at %1").arg(p.addr, 8, 16, QChar('0')));
                qint64 id = d->Id;
                reply(kb.DeleteDataObject(id, e), e, id);
            }
            else
            {
                if (!kb.LabelAt((int)cpu, p.addr, ov)) return bad(QString("no label at %1").arg(p.addr, 8, 16, QChar('0')));
                reply(kb.SetLabel((int)cpu, p.addr, QString(), ov, e), e, 0);
            }
            return;
        }
        if (method == "kb.type.delete")
        {
            WBApi::KbTypeNameParams p;
            if (!WBApi::KbTypeNameParams::fromJson(params, p, err)) return bad(err);
            WBKB::KnowledgeStore::AuthorScope scope(kb, agent(), agentSession);
            QString e;
            reply(kb.DeleteType(p.name, e), e, 0);
            return;
        }
        if (method == "kb.build.record")
        {
            WBApi::KbBuildRecordParams p;
            if (!WBApi::KbBuildRecordParams::fromJson(params, p, err)) return bad(err);
            WBApi::KbAddrParams a;
            a.addr = p.addr; a.cpu = p.cpu; a.overlay = p.overlay;
            u32 cpu;
            QString e;
            const WBKB::Function* f = functionAt(a, cpu, e);
            if (!f) return bad(e);
            WBKB::BuildResult r;
            r.FunctionId = f->Id;
            r.Compiler = p.compiler.isEmpty() ? QString("unknown") : p.compiler;
            r.Flags = p.flags;
            r.Score = p.score;
            r.Matched = p.matched;
            r.Diff = p.diff;
            r.CommitHash = p.commit;
            r.SourceFile = p.source_file;
            WBKB::KnowledgeStore::AuthorScope scope(kb, agent(), agentSession);
            reply(kb.RecordBuildResult(r, e), e, f->Id);
            return;
        }
        if (method == "kb.build.compare" || method == "build.selftest")
        {
            bool selftest = method == "build.selftest";
            WBApi::KbAddrParams a;
            bool record = false;
            if (selftest)
            {
                WBApi::BuildSetupParams p;
                if (!WBApi::BuildSetupParams::fromJson(params, p, err)) return bad(err);
                a.addr = p.addr; a.cpu = p.cpu; a.overlay = p.overlay;
            }
            else
            {
                WBApi::KbCompareParams p;
                if (!WBApi::KbCompareParams::fromJson(params, p, err)) return bad(err);
                a.addr = p.addr; a.cpu = p.cpu; a.overlay = p.overlay;
                record = p.record;
            }
            u32 cpu;
            QString e;
            const WBKB::Function* f = nullptr;
            if (!selftest || a.addr)
            {
                f = functionAt(a, cpu, e);
                if (!f) return bad(e);
            }
            auto answer = [this, sock, id](const WBHost::CompareRun& run)
            {
                const QJsonObject& o = run.Result;
                if (!run.Ok && o.isEmpty() && run.ContractErrors.isEmpty() && run.Stdout.isEmpty())
                {
                    Error(sock, id, ErrUnavailable, run.Explanation.isEmpty() ? run.Error : run.Error + "\n" + run.Explanation);
                    return;
                }
                WBApi::KbCompareResult r;
                r.ok = run.Ok;
                r.score = o.value("score").toDouble();
                r.matched = o.value("matched").toBool(r.score >= 100.0);
                r.compiler = o.value("compiler").toString();
                r.flags = o.value("flags").toString();
                r.diff = QString::fromUtf8(QJsonDocument(o.value("diff").toArray()).toJson(QJsonDocument::Compact));
                r.recorded = run.Recorded;
                r.recorded_by_tool = o.value("recorded").toBool();
                r.stage = o.value("stage").toString(run.Ok ? "done" : "");
                r.engine = o.value("engine").toString();
                r.log = o.value("log").toString();
                r.explanation = run.Explanation;
                for (const QString& c : run.ContractErrors) r.contract_errors.push_back(c);
                r.message = run.Error;
                Reply(sock, id, r.toJson());
            };
            if (!selftest)
            {
                Host->CompareFunction(f->Id, record, agent(), agentSession, answer);
                return;
            }
            WBHost::CompareJob job;
            if (!Host->EnsureCompareProject(e) || !Host->PrepareSelfTest(f ? f->Id : 0, job, e)) { Error(sock, id, ErrUnavailable, e); return; }
            if (!Host->CompareApproved(job))
            {
                Error(sock, id, ErrUnavailable, "The compare script has not been approved yet. Approve it in Build > Compare setup.");
                return;
            }
            Host->RunCompare(job, false, agent(), agentSession, answer);
            return;
        }
        if (method == "build.setup.check")
        {
            WBApi::BuildSetupParams p;
            if (!WBApi::BuildSetupParams::fromJson(params, p, err)) return bad(err);
            qint64 fid = 0;
            if (p.addr)
            {
                WBApi::KbAddrParams a;
                a.addr = p.addr; a.cpu = p.cpu; a.overlay = p.overlay;
                u32 cpu;
                QString e;
                const WBKB::Function* f = functionAt(a, cpu, e);
                if (!f) return bad(e);
                fid = f->Id;
            }
            Host->CheckCompareSetup(fid, this, [this, sock, id](std::vector<WBHost::SetupRow> rows)
            {
                WBApi::BuildSetupResult r;
                r.ready = true;
                for (const WBHost::SetupRow& s : rows)
                {
                    WBApi::BuildSetupRow o;
                    o.id = s.Id; o.label = s.Label; o.ok = s.Ok; o.optional = s.Optional; o.message = s.Message; o.fix = s.Fix;
                    if (!s.Ok && !s.Optional && s.Id != "self_test") r.ready = false;
                    r.rows.push_back(o);
                }
                Reply(sock, id, r.toJson());
            });
            return;
        }
        if (method == "kb.import.static_analysis")
        {
            WBApi::KbImportParams p;
            if (!WBApi::KbImportParams::fromJson(params, p, err)) return bad(err);
            WBKB::KnowledgeStore::AuthorScope scope(kb, agent(), agentSession);
            QString e;
            int n = ImportAnalysisJson(kb, p.path, e);
            if (n < 0) return bad(e);
            WBApi::KbImportResult r;
            r.ok = true;
            r.count = (u32)n;
            Reply(sock, id, r.toJson());
            return;
        }
        if (method == "kb.undo" || method == "kb.redo")
        {
            qint64 author = agent();
            WBKB::KnowledgeStore::AuthorScope scope(kb, author, agentSession);
            QString e;
            bool ok = method == "kb.undo" ? kb.Undo(e, author) : kb.Redo(e, author);
            reply(ok, e, 0);
            return;
        }
    }

    if (method == "archive.list")
    {
        WBApi::ArchiveListParams p;
        if (!WBApi::ArchiveListParams::fromJson(params, p, err)) return bad(err);
        WBQ::ArchiveCtx arc {Host->Archive(), Host->ArchiveStatus()};
        WBApi::ArchiveListResult r;
        QString e;
        WBQ::ArchiveList(arc, p, r, e);
        Reply(sock, id, r.toJson());
        return;
    }

    if (method == "container.list")
    {
        WBApi::ContainerListParams p;
        if (!WBApi::ContainerListParams::fromJson(params, p, err)) return bad(err);
        WBQ::ContainerCtx cont {Host->Containers(), Host->ContainerStatus()};
        WBApi::ContainerListResult r;
        QString e;
        if (!WBQ::ContainerList(cont, p, r, e)) return Error(sock, id, ErrParams, e);
        Reply(sock, id, r.toJson());
        return;
    }

    if (method == "container.locate")
    {
        WBApi::ContainerLocateParams p;
        if (!WBApi::ContainerLocateParams::fromJson(params, p, err)) return bad(err);
        WBQ::ContainerCtx cont {Host->Containers(), Host->ContainerStatus()};
        Host->Query<std::pair<bool, QJsonObject>>([this, p, cont]()
        {
            WBApi::ContainerLocateResult r;
            QString e;
            if (!WBQ::ContainerLocate(Host->Core(), cont, p, r, e)) { QJsonObject o; o["error"] = e; return std::make_pair(false, o); }
            return std::make_pair(true, r.toJson());
        }, this, [this, sock, id](std::pair<bool, QJsonObject> r)
        {
            if (!r.first) Error(sock, id, ErrParams, r.second.value("error").toString());
            else Reply(sock, id, r.second);
        });
        return;
    }

    if (method == "archive.locate")
    {
        WBApi::ArchiveLocateParams p;
        if (!WBApi::ArchiveLocateParams::fromJson(params, p, err)) return bad(err);
        WBQ::ArchiveCtx arc {Host->Archive(), Host->ArchiveStatus()};
        Host->Query<std::pair<bool, QJsonObject>>([this, p, arc]()
        {
            WBApi::ArchiveLocateResult r;
            QString e;
            if (!WBQ::ArchiveLocate(Host->Core(), arc, p, r, e)) { QJsonObject o; o["error"] = e; return std::make_pair(false, o); }
            return std::make_pair(true, r.toJson());
        }, this, [this, sock, id](std::pair<bool, QJsonObject> r)
        {
            if (!r.first) Error(sock, id, ErrParams, r.second.value("error").toString());
            else Reply(sock, id, r.second);
        });
        return;
    }

    // ---- graphics files (WBAssets.h)
    if (method == "asset.list")
    {
        WBApi::AssetListParams p;
        if (!WBApi::AssetListParams::fromJson(params, p, err)) return bad(err);
        auto cat = Host->Graphics();
        WBApi::AssetListResult r;
        r.status = Host->GraphicsStatus();
        if (cat)
        {
            r.summary = QString::fromStdString(cat->Summary);
            QString f = p.filter.toLower();
            u32 limit = std::min<u32>(p.limit ? p.limit : 200, 5000);
            for (const WBAsset::File& x : cat->Files())
            {
                QString path = QString::fromStdString(x.Path);
                if (!f.isEmpty() && !path.toLower().contains(f) && QString::fromStdString(x.Kind).toLower() != f) continue;
                r.matched++;
                if (r.files.size() >= limit) continue;
                WBApi::AssetFileInfo i;
                i.path = path;
                i.kind = QString::fromStdString(x.Kind);
                i.size = x.Size;
                i.source = x.From == WBAsset::File::RomFile ? "rom_file" : x.From == WBAsset::File::Container ? "container" : "archive";
                i.form = QString::fromStdString(x.Form);
                r.files.push_back(i);
            }
        }
        Reply(sock, id, r.toJson());
        return;
    }
    if (method == "asset.describe" || method == "asset.render")
    {
        bool render = method == "asset.render";
        WBApi::AssetRenderParams p;
        if (render) { if (!WBApi::AssetRenderParams::fromJson(params, p, err)) return bad(err); }
        else
        {
            WBApi::AssetParams a;
            if (!WBApi::AssetParams::fromJson(params, a, err)) return bad(err);
            p.path = a.path; p.addr = a.addr; p.size = a.size;
        }
        auto cat = Host->Graphics();
        Host->Query<std::pair<bool, QJsonObject>>([this, p, cat, render]()
        {
            QJsonObject fail;
            WBAsset::Asset a;
            std::string e;
            if (!WBAsset::Load(Host->Core(), cat.get(), p.path.toStdString(), p.addr, p.size, a, e))
            {
                fail["error"] = QString::fromStdString(e);
                return std::make_pair(false, fail);
            }
            if (render)
            {
                WBAsset::RenderOptions o;
                o.Part = p.part.toStdString();
                o.Index = p.index;
                o.Palette = p.palette;
                o.PalettePath = p.palette_path.toStdString();
                o.Bpp = (int)p.bpp;
                o.Tiled = p.tiled;
                o.Width = (int)p.width;
                o.PaletteOffset = (int)p.palette_offset;
                WBAsset::RenderInfo info;
                QImage img = WBAsset::Render(cat.get(), a, o, WBAsset::ReadPaletteRam(Host->Core()), info, e);
                if (img.isNull())
                {
                    fail["error"] = QString::fromStdString(e.empty() ? std::string("nothing to draw") : e);
                    return std::make_pair(false, fail);
                }
                int scale = std::clamp<int>((int)p.scale, 1, 8);
                if (scale > 1) img = img.scaled(img.width() * scale, img.height() * scale);
                QByteArray ba;
                QBuffer buf(&ba);
                buf.open(QIODevice::WriteOnly);
                img.save(&buf, "PNG");
                WBApi::AssetRenderResult r;
                r.png = QString::fromLatin1(ba.toBase64());
                r.width = (u32)img.width();
                r.height = (u32)img.height();
                r.palette = QString::fromStdString(info.Palette);
                r.tiles = QString::fromStdString(info.Tiles);
                return std::make_pair(true, r.toJson());
            }
            const WBGfx::Description& d = a.Desc;
            WBApi::AssetDescribeResult r;
            r.path = QString::fromStdString(a.Path);
            r.format = QString::fromStdString(d.Format);
            r.size = (u32)a.Bytes.size();
            r.ram_addr = a.RamAddr;
            r.unreadable_from = d.UnreadableFrom;
            r.problem = QString::fromStdString(d.Problem);
            for (const WBGfx::Part& x : d.Parts)
            {
                WBApi::AssetPartInfo i;
                i.offset = x.Offset; i.size = x.Size; i.kind = QString::fromStdString(x.Kind); i.index = x.Index;
                i.label = QString::fromStdString(x.Label);
                r.parts.push_back(i);
            }
            if (d.Format == "NCLR")
            {
                r.colors_per_palette = d.PalBpp == 4 ? 16 : 256;
                r.palettes = (d.PalSize / 2 + r.colors_per_palette - 1) / r.colors_per_palette;
            }
            if (d.Format == "NCGR")
            {
                r.bpp = (u32)d.CharBpp;
                r.tiles = d.CharSize / (8u * d.CharBpp);
                r.tiles_x = (u32)d.TilesX;
                r.tiles_y = (u32)d.TilesY;
            }
            r.map_width = (u32)d.MapWidth;
            r.map_height = (u32)d.MapHeight;
            r.cells = (u32)d.Cells.size();
            for (size_t i = 0; i < d.Textures.size(); i++)
            {
                const WBGfx::Texture& t = d.Textures[i];
                WBApi::AssetTextureInfo ti;
                ti.name = QString::fromStdString(t.Name);
                ti.format = WBGfx::TextureFormatName(t.Format);
                ti.width = (u32)t.Width; ti.height = (u32)t.Height; ti.offset = t.Offset; ti.size = t.Size;
                int pi = WBGfx::TexturePaletteFor(d, (int)i);
                if (pi >= 0) ti.palette = QString::fromStdString(d.TexPalettes[pi].Name);
                r.textures.push_back(ti);
            }
            for (const WBGfx::TexPalette& tp : d.TexPalettes) r.texture_palettes.push_back(QString::fromStdString(tp.Name));
            if (cat && a.File >= 0)
            {
                const WBAsset::File& f = cat->Files()[a.File];
                NDS* nds = Host->Core().GetNDS();
                if (nds && nds->MainRAM)
                    for (const WBArc::Match& m : cat->Locate(a.File, nds->MainRAM, 0x02000000, nds->MainRAMMask + 1, 8))
                        r.ram_copies.push_back(m.Addr - m.MemberOffset);
                for (const std::string& v : WBAsset::FindInVideoMemory(Host->Core(), f, a.Bytes.data(), (u32)a.Bytes.size()))
                    r.video_memory.push_back(QString::fromStdString(v));
            }
            return std::make_pair(true, r.toJson());
        }, this, [this, sock, id](std::pair<bool, QJsonObject> r)
        {
            if (!r.first) Error(sock, id, ErrParams, r.second.value("error").toString());
            else Reply(sock, id, r.second);
        });
        return;
    }

    if (method == "screen.capture")
    {
        WBApi::ScreenParams p;
        if (!WBApi::ScreenParams::fromJson(params, p, err)) return bad(err);
        QString which = p.screen.toLower();
        Host->Query<std::tuple<bool, QImage, QImage, QString, u32>>([this]()
        {
            QImage t, b;
            QString e;
            bool ok = WBQ::CaptureScreens(Host->Core(), t, b, e);
            return std::make_tuple(ok, t, b, e, Host->Core().Frame());
        }, this, [this, sock, id, which](std::tuple<bool, QImage, QImage, QString, u32> r)
        {
            if (!std::get<0>(r)) { Error(sock, id, ErrUnavailable, std::get<3>(r)); return; }
            auto png = [](const QImage& img)
            {
                QByteArray ba;
                QBuffer buf(&ba);
                buf.open(QIODevice::WriteOnly);
                img.save(&buf, "PNG");
                return QString::fromLatin1(ba.toBase64());
            };
            WBApi::ScreenResult res;
            res.frame = std::get<4>(r);
            if (which != "bottom") res.top_png = png(std::get<1>(r));
            if (which != "top") res.bottom_png = png(std::get<2>(r));
            Reply(sock, id, res.toJson());
        });
        return;
    }

    if (method == "disasm.get")
    {
        WBApi::DisasmParams p;
        if (!WBApi::DisasmParams::fromJson(params, p, err)) return bad(err);
        u32 hcpu = 0;
        QString herr;
        WBQ::ParseCpu(p.cpu, hcpu, herr);
        u32 hend = p.addr + std::min<u32>(std::max<u32>(p.count, 1), 4096) * 4 + 4;
        // an overlay epoch names what that overlay has there
        int hov = p.overlay >= 0 ? p.overlay : hcpu ? -1 : Host->OverlayLoadedAt(p.addr);
        WBQ::KbHints hints = WBQ::KbHints::Collect(Host->KB(), (int)hcpu, p.addr, hend, hov);
        std::vector<WBQ::DataUnit> units = WBQ::DataUnits(Host->KB(), (int)hcpu, p.addr, hend, hov);
        WBQ::AnalysisHints guessed = Host->AnalysisNear((int)hcpu, p.addr > 8 ? p.addr - 8 : 0, hend);
        Host->Query<std::pair<bool, QJsonObject>>([this, p, hints, units, guessed]()
        {
            WBApi::DisasmResult r;
            QString e;
            if (!WBQ::Disasm(Host->Core(), p, r, e, &hints, &units, &guessed)) { QJsonObject o; o["error"] = e; return std::make_pair(false, o); }
            return std::make_pair(true, r.toJson());
        }, this, [this, sock, id, p](std::pair<bool, QJsonObject> r)
        {
            if (!r.first) { Error(sock, id, ErrParams, r.second.value("error").toString()); return; }
            // add knowledge store names for branch targets and references
            u32 cpu = 0;
            QString e;
            WBQ::ParseCpu(p.cpu, cpu, e);
            QJsonArray lines = r.second.value("lines").toArray();
            for (int i = 0; i < lines.size(); i++)
            {
                QJsonObject l = lines[i].toObject();
                u32 t = (u32)l.value("target").toDouble();
                if (t)
                {
                    // in the viewed overlay's range, its names; elsewhere what is loaded
                    const WBKB::Module* tm = p.overlay >= 0 ? Host->KB().ModuleFor((int)cpu, t, p.overlay) : nullptr;
                    int tov = tm && tm->Kind == "overlay" && tm->OverlayId == p.overlay ? p.overlay : Host->OverlayLoadedAt(t);
                    QString n = Host->KB().NameAt((int)cpu, t, tov, true);
                    if (!n.isEmpty()) l["text"] = l.value("text").toString() + " <" + n + ">";
                }
                lines[i] = l;
            }
            QJsonObject out = r.second;
            out["lines"] = lines;
            Reply(sock, id, out);
        });
        return;
    }

    if (method == "xrefs.get")
    {
        WBApi::XrefsParams p;
        if (!WBApi::XrefsParams::fromJson(params, p, err)) return bad(err);
        u32 cpu;
        if (!WBQ::ParseCpu(p.cpu, cpu, err)) return bad(err);
        WBKB::KnowledgeStore& kb = Host->KB();
        u32 addr = p.addr;
        int ov = p.overlay >= 0 ? p.overlay : (cpu == 0 ? Host->OverlayLoadedAt(addr) : -1);
        // imported references first, then what the recording saw (the Cross-references panel's rows)
        std::vector<WBQ::XrefRow> rows;
        for (const WBKB::Xref& x : kb.XrefsTo((int)cpu, addr, ov)) rows.push_back({x.Kind, x.From, x.Source, 0});
        const WBKB::Function* f = kb.FunctionStarting((int)cpu, addr, ov);
        u32 fsize = f ? f->Size : 0;
        u32 limit = std::min<u32>(p.limit, 100000);
        Host->Query<std::vector<WBQ::XrefRow>>([this, cpu, addr, fsize, rows]() mutable
        {
            WBQ::RuntimeXrefs(Host->Core(), Host->AllTime(), cpu, addr, fsize, rows);
            return rows;
        }, this, [this, sock, id, cpu, addr, ov, limit](std::vector<WBQ::XrefRow> rows)
        {
            WBKB::KnowledgeStore& kb = Host->KB();
            WBApi::XrefsResult r;
            r.addr = addr;
            r.name = kb.NameAt((int)cpu, addr, ov, true);
            for (const WBQ::XrefRow& x : rows)
            {
                if (r.refs.size() >= limit) { r.truncated = true; break; }
                WBApi::XrefInfo i;
                i.kind = x.Kind;
                i.from_addr = x.From;
                i.source = x.Source;
                i.count = x.Count;
                int fov = cpu == 0 ? Host->OverlayLoadedAt(x.From) : -1;
                if (const WBKB::Function* fn = kb.FunctionAt((int)cpu, x.From, fov))
                {
                    i.function = fn->Addr;
                    i.function_name = kb.NameAt((int)cpu, fn->Addr, fov);
                }
                r.refs.push_back(i);
            }
            Reply(sock, id, r.toJson());
        });
        return;
    }

    if (method == "overlays.list")
    {
        WBApi::OverlaysParams p;
        if (!WBApi::OverlaysParams::fromJson(params, p, err)) return bad(err);
        u32 maxEvents = p.events;
        Host->Query<WBApi::OverlaysResult>([this, maxEvents]()
        {
            WB::Workbench& w = Host->Core();
            WBApi::OverlaysResult r;
            NDS* nds = w.GetNDS();
            u32 mask = nds ? nds->MainRAMMask : 0x3FFFFF;
            for (int c = 0; c < 2; c++)
                for (const OverlayInfo& o : c ? w.Rom.Overlays7 : w.Rom.Overlays9)
                {
                    WBApi::OverlayTableEntry e;
                    e.id = o.Id;
                    e.cpu = c ? "arm7" : "arm9";
                    e.ram_start = o.RamStart;
                    e.ram_end = o.RamEnd();
                    e.ram_size = o.RamSize;
                    e.bss_size = o.BssSize;
                    e.file_id = o.FileId;
                    if (const RomFile* f = w.Rom.FileById(o.FileId)) e.file = QString::fromStdString(f->Path);
                    e.compressed = o.Compressed();
                    e.loaded = (o.RamStart >> 24) == 0x02 && w.Rec.OverlayAt(o.RamStart & mask) == (int)(((u32)c << 15) | o.Id);
                    r.overlays.push_back(e);
                }
            std::vector<OverlayEvent> ev = w.Rec.OverlayEvents();
            r.events_total = (u32)ev.size();
            for (auto it = ev.rbegin(); it != ev.rend() && r.events.size() < maxEvents; ++it)
            {
                WBApi::OverlayEventInfo e;
                e.frame = it->frame;
                e.kind = it->kind == 1 ? "evicted" : it->kind == 2 ? "load_ran" : "load";
                e.cpu = it->cpu ? "arm7" : "arm9";
                e.overlay_id = it->overlay_id;
                e.ram_start = it->ram_start;
                e.ram_end = it->ram_end;
                r.events.push_back(e);
            }
            return r;
        }, this, [this, sock, id](WBApi::OverlaysResult r)
        {
            for (WBApi::OverlayTableEntry& e : r.overlays)
                for (const WBKB::Module& m : Host->KB().Modules())
                    if (m.Kind == "overlay" && m.OverlayId == (int)e.id && m.Cpu == e.cpu) e.module = m.Name;
            Reply(sock, id, r.toJson());
        });
        return;
    }

    if (method == "search.bytes" || method == "search.string")
    {
        QByteArray pat, mask;
        QString region;
        u32 limit;
        if (method == "search.bytes")
        {
            WBApi::SearchBytesParams p;
            if (!WBApi::SearchBytesParams::fromJson(params, p, err)) return bad(err);
            if (!WBQ::ParseBytePattern(p.pattern, pat, mask, err)) return bad(err);
            region = p.region.toLower();
            limit = p.limit;
        }
        else
        {
            WBApi::SearchStringParams p;
            if (!WBApi::SearchStringParams::fromJson(params, p, err)) return bad(err);
            if (!WBQ::EncodeSearchText(p.text, p.encoding, pat, err)) return bad(err);
            mask = QByteArray(pat.size(), '\1');
            region = p.region.toLower();
            limit = p.limit;
        }
        if (region != "main_ram" && region != "all") return bad("region must be main_ram or all");
        bool wide = region == "all";
        limit = std::min<u32>(limit, 100000);
        Host->Query<std::vector<WBQ::SearchHit>>([this, pat, mask, wide, limit]()
        {
            return WBQ::FindPattern(Host->Core(), pat, mask, wide, (size_t)limit + 1);
        }, this, [this, sock, id, limit, size = (u32)pat.size()](std::vector<WBQ::SearchHit> hits)
        {
            WBApi::SearchResult r;
            r.size = size;
            r.truncated = hits.size() > limit;
            if (r.truncated) hits.resize(limit);
            for (const WBQ::SearchHit& h : hits)
            {
                WBApi::SearchMatch m;
                m.addr = h.Addr;
                m.preview = h.Preview;
                m.name = Host->KB().NameAt(0, h.Addr, Host->OverlayLoadedAt(h.Addr), true);
                r.matches.push_back(m);
            }
            Reply(sock, id, r.toJson());
        });
        return;
    }

    if (method == "search.scan")
    {
        WBApi::SearchScanParams p;
        if (!WBApi::SearchScanParams::fromJson(params, p, err)) return bad(err);
        static const QStringList conds = {"equal", "not_equal", "changed", "unchanged", "increased", "decreased", "greater", "less", "unknown"};
        static const QStringList types = {"u8", "u16", "u32", "s32", "fx32"};
        int ci = conds.indexOf(p.condition.toLower());
        if (ci < 0) return bad("condition must be one of " + conds.join(", "));
        WBQ::ScanCond cond = (WBQ::ScanCond)ci;
        std::shared_ptr<ApiScan> scan;
        bool first = p.scan_id == 0;
        if (first)
        {
            int ti = types.indexOf(p.type.toLower());
            if (ti < 0) return bad("type must be one of " + types.join(", "));
            if (cond != WBQ::ScanCond::Unknown && !WBQ::ScanNeedsValue(cond))
                return bad(p.condition + " compares with a previous scan: pass its scan_id");
            QString region = p.region.toLower();
            if (region != "main_ram" && region != "all") return bad("region must be main_ram or all");
            scan = std::make_shared<ApiScan>();
            scan->State.Type = (WBQ::ScanType)ti;
            scan->Wide = region == "all";
        }
        else
        {
            auto it = c->Scans.find(p.scan_id);
            if (it == c->Scans.end())
                return bad(QString("no scan %1 on this connection (scans last as long as the connection, which keeps the newest 8)").arg(p.scan_id));
            scan = it->second;
            if (scan->Busy) return bad(QString("scan %1 is still running").arg(p.scan_id));
            if (cond == WBQ::ScanCond::Unknown) return bad("unknown is for a first scan");
        }
        s64 value = 0;
        if (WBQ::ScanNeedsValue(cond))
        {
            if (!p.value) return bad(p.condition + " needs a value");
            const QJsonValue& v = *p.value;
            WBQ::ScanType t = scan->State.Type;
            if (v.isDouble()) value = t == WBQ::ScanType::FX32 ? (s64)std::llround(v.toDouble() * 4096.0) : (s64)v.toDouble();
            else if (!v.isString() || !WBQ::ParseScanValue(v.toString(), t, value)) return bad("value must be a number, or a string such as 100, 0x64 or 1.5");
            if (!WBQ::ScanValueInRange(t, value, err)) return bad(err);
        }
        if (first)
        {
            u32 sid = c->NextScan++;
            c->Scans[sid] = scan;
            while (c->Scans.size() > 8) c->Scans.erase(c->Scans.begin());
            scan->Id = sid;
        }
        scan->Busy = true;
        u32 limit = std::min<u32>(p.limit, 100000);
        WBQ::ScanState state = std::move(scan->State);
        Host->Query<std::pair<WBQ::ScanState, u32>>([this, state, cond, value, first, wide = scan->Wide]() mutable
        {
            if (first) WBQ::ScanFirst(Host->Core(), state, cond, value, wide);
            else WBQ::ScanNext(Host->Core(), state, cond, value);
            return std::make_pair(std::move(state), Host->Core().Frame());
        }, this, [this, sock, id, scan, limit](std::pair<WBQ::ScanState, u32> res)
        {
            static const char* typeNames[] = {"u8", "u16", "u32", "s32", "fx32"};
            scan->State = std::move(res.first);
            scan->Busy = false;
            const WBQ::ScanState& s = scan->State;
            WBApi::SearchScanResult r;
            r.scan_id = scan->Id;
            r.type = typeNames[(int)s.Type];
            r.candidates = (u32)s.Addrs.size();
            r.frame = res.second;
            r.truncated = s.Addrs.size() > limit;
            for (size_t i = 0; i < s.Addrs.size() && i < limit; i++)
            {
                WBApi::ScanMatch m;
                m.addr = s.Addrs[i];
                m.raw = s.Values[i];
                m.value = WBQ::ScanDecode(s.Type, s.Values[i]);
                m.name = Host->KB().NameAt(0, m.addr, Host->OverlayLoadedAt(m.addr), true);
                r.matches.push_back(m);
            }
            Reply(sock, id, r.toJson());
        });
        return;
    }

    if (method == "breakpoints.list")
    {
        Host->Query<QJsonObject>([this]()
        {
            WB::Workbench& w = Host->Core();
            WBApi::BreakpointListResult r;
            for (const Breakpoint& bp : w.Dbg.List()) r.breakpoints.push_back(BreakpointJson(bp));
            for (const BreakHit& h : w.Dbg.Hits())
            {
                WBApi::BreakpointHit i;
                i.id = h.Id;
                i.cpu = WBQ::CpuName(h.Cpu);
                i.pc = h.PC;
                i.frame = h.Frame;
                i.access_addr = h.AccessAddr;
                i.access_value = h.AccessValue;
                i.access_size = h.AccessSize;
                i.write = h.Write;
                r.hits.push_back(i);
            }
            return r.toJson();
        }, this, [this, sock, id](QJsonObject r) { Reply(sock, id, r); });
        return;
    }

    auto recordingResult = [this](const QString& sessionFile)
    {
        WBApi::RecordingResult r;
        WB::Workbench& w = Host->Core();
        Host->RunSync([&]()
        {
            r.recording = WBQ::Recording(w);
            for (const DataSource& s : w.Taint.Sources())
            {
                if (s.Kind == SourceKind::CodeConstant) continue;
                WBApi::OriginSourceInfo i;
                i.id = s.Id;
                i.kind = SourceKindName((u8)s.Kind);
                i.name = QString::fromStdString(s.Name);
                i.enabled = s.Enabled;
                r.sources.push_back(i);
            }
        });
        r.recording.history_keep_mb = Host->Settings().HistoryKeepMB;
        r.session_file = sessionFile;
        return r;
    };

    if (method == "recording.get")
    {
        Reply(sock, id, recordingResult(QString()).toJson());
        return;
    }

    // ---------------------------------------------------------------- control

    if (method == "game.boot")
    {
        WBApi::BootParams p;
        if (!WBApi::BootParams::fromJson(params, p, err)) return bad(err);
        QString e;
        if (!Host->Boot(p.path, e)) { Error(sock, id, ErrServer, e); return; }
        WBApi::Ok r;
        r.ok = true;
        r.message = "booted";
        Reply(sock, id, r.toJson());
        return;
    }

    if (method == "game.reset")
    {
        QString e;
        if (!Host->Reset(e)) { Error(sock, id, ErrNoGame, e); return; }
        WBApi::Ok r;
        r.ok = true;
        Reply(sock, id, r.toJson());
        return;
    }

    if (method == "execution.control")
    {
        WBApi::ExecutionParams p;
        if (!WBApi::ExecutionParams::fromJson(params, p, err)) return bad(err);
        u32 cpu;
        if (!WBQ::ParseCpu(p.cpu, cpu, err)) return bad(err);
        QString act = p.action.toLower();
        if (Host->Status().EmuState == WBStatus::NoGame) { Error(sock, id, ErrNoGame, "no game running"); return; }
        auto result = [this](bool interrupted, u32 cpu, const char* forced = nullptr)
        {
            WBApi::ExecutionResult r;
            WBStatus s = Host->Status();
            static const char* states[] = {"no_game", "running", "paused", "break"};
            r.state = forced ? forced : states[s.EmuState];
            if (Host->Core().Dbg.InBreak()) r.state = "break";
            r.frame = s.Frame;
            r.pc = Host->Core().Dbg.InBreak() ? Host->Core().Dbg.CurrentBreak().PC : s.PC[cpu & 1];
            r.interrupted = interrupted;
            return r.toJson();
        };
        if (act == "pause")
        {
            Host->Pause();
            QTimer::singleShot(30, this, [=]() { Reply(sock, id, result(false, cpu, Host->EmuRunning() ? nullptr : "paused")); });
            return;
        }
        if (act == "resume")
        {
            Host->Resume();
            QTimer::singleShot(30, this, [=]() { Reply(sock, id, result(false, cpu)); });
            return;
        }
        if (act == "step" || act == "break")
        {
            if (Host->Core().JITActive()) { Error(sock, id, ErrUnavailable, "stepping needs the interpreter: turn the JIT off"); return; }
            auto conn = std::make_shared<QMetaObject::Connection>();
            auto done = std::make_shared<bool>(false);
            if (p.wait)
            {
                *conn = connect(Host, &WBHost::breakChanged, this, [=](bool inBreak)
                {
                    if (!inBreak || *done) return;
                    *done = true;
                    disconnect(*conn);
                    Reply(sock, id, result(false, cpu));
                });
                QTimer::singleShot(10000, this, [=]()
                {
                    if (*done) return;
                    *done = true;
                    disconnect(*conn);
                    Reply(sock, id, result(true, cpu));
                });
            }
            if (act == "step") Host->Step((int)cpu, std::max<u32>(p.count, 1));
            else Host->BreakNow((int)cpu);
            if (!p.wait) Reply(sock, id, result(false, cpu));
            return;
        }
        if (act == "run_frames")
        {
            u32 n = std::max<u32>(p.count, 1);
            if (p.wait)
                Host->RunFrames(n, [=](bool interrupted) { Reply(sock, id, result(interrupted, cpu, "paused")); });
            else
            {
                Host->RunFrames(n, nullptr);
                Reply(sock, id, result(false, cpu));
            }
            return;
        }
        return bad("action must be pause, resume, step, break or run_frames");
    }

    if (method == "input.send")
    {
        WBApi::InputParams p;
        if (!WBApi::InputParams::fromJson(params, p, err)) return bad(err);
        bool lid;
        u32 mask = ButtonMask(p.buttons, lid, err);
        if (!err.isEmpty()) return bad(err);
        bool touch = p.touch.has_value();
        u16 tx = 0, ty = 0;
        if (touch)
        {
            if (p.touch->x > 255 || p.touch->y > 191) return bad("touch must be within 0..255 x 0..191");
            tx = (u16)p.touch->x;
            ty = (u16)p.touch->y;
        }
        if (Host->Status().EmuState == WBStatus::NoGame) { Error(sock, id, ErrNoGame, "no game running"); return; }
        bool capture = p.capture;
        Host->SendInput(mask, touch, tx, ty, lid, std::max<u32>(p.frames, 1), [=](bool overridden, bool interrupted)
        {
            if (!capture)
            {
                WBApi::InputResult r;
                r.frame = Host->Status().Frame;
                r.overridden = overridden;
                r.interrupted = interrupted;
                Reply(sock, id, r.toJson());
                return;
            }
            Host->Query<std::tuple<bool, QImage, QImage, u32>>([this]()
            {
                QImage t, b;
                QString e;
                bool ok = WBQ::CaptureScreens(Host->Core(), t, b, e);
                return std::make_tuple(ok, t, b, Host->Core().Frame());
            }, this, [=](std::tuple<bool, QImage, QImage, u32> r)
            {
                auto png = [](const QImage& img)
                {
                    QByteArray ba;
                    QBuffer buf(&ba);
                    buf.open(QIODevice::WriteOnly);
                    img.save(&buf, "PNG");
                    return QString::fromLatin1(ba.toBase64());
                };
                WBApi::InputResult res;
                res.frame = std::get<3>(r);
                res.overridden = overridden;
                res.interrupted = interrupted;
                if (std::get<0>(r))
                {
                    res.top_png = png(std::get<1>(r));
                    res.bottom_png = png(std::get<2>(r));
                }
                Reply(sock, id, res.toJson());
            });
        });
        return;
    }

    if (method == "state.save" || method == "state.load")
    {
        WBApi::StateParams p;
        if (!WBApi::StateParams::fromJson(params, p, err)) return bad(err);
        QString path = p.path;
        if (path.isEmpty())
        {
            if (p.slot < 1 || p.slot > 8) return bad("slot must be 1..8");
            path = Host->SavestatePath((int)p.slot);
        }
        QString e;
        bool ok = method == "state.save" ? Host->SaveState(path, e) : Host->LoadState(path, e);
        if (!ok) { Error(sock, id, ErrServer, e); return; }
        WBApi::Ok r;
        r.ok = true;
        r.message = path;
        Reply(sock, id, r.toJson());
        return;
    }

    if (method == "breakpoint.set")
    {
        WBApi::BreakpointParams p;
        if (!WBApi::BreakpointParams::fromJson(params, p, err)) return bad(err);
        if (Host->Core().JITActive()) { Error(sock, id, ErrUnavailable, "breakpoints need the interpreter: turn the JIT off"); return; }
        Breakpoint bp;
        QString k = p.kind.toLower();
        if (k == "exec") bp.Kind = BreakKind::Exec;
        else if (k == "read") bp.Kind = BreakKind::Read;
        else if (k == "write") bp.Kind = BreakKind::Write;
        else if (k == "access") bp.Kind = BreakKind::Access;
        else return bad("kind must be exec, read, write or access");
        QString c = p.cpu.toLower();
        if (c == "any") bp.Cpu = -1;
        else { u32 cpu; if (!WBQ::ParseCpu(c, cpu, err)) return bad(err); bp.Cpu = (int)cpu; }
        bp.Addr = bp.Kind == BreakKind::Exec ? (p.addr & ~1u) : p.addr;
        bp.Size = std::max<u32>(p.size, 1);
        bp.Condition = p.condition.toStdString();
        bp.Enabled = p.enabled;
        bp.Temporary = p.temporary;
        bp.Author = "api";
        std::string e;
        int newid = -1;
        Host->RunSync([&]() { newid = Host->Core().Dbg.AddBreakpoint(bp, e); });
        if (newid < 0) return bad(QString::fromStdString(e.empty() ? "could not add breakpoint" : e));
        emit Host->breakpointsChanged();
        WBApi::BreakpointInfo i;
        i.id = newid;
        i.kind = k;
        i.addr = bp.Addr;
        i.size = bp.Size;
        i.cpu = p.cpu.toLower();
        i.condition = p.condition;
        i.enabled = bp.Enabled;
        i.temporary = bp.Temporary;
        Reply(sock, id, i.toJson());
        return;
    }

    if (method == "breakpoint.update")
    {
        WBApi::BreakpointUpdateParams p;
        if (!WBApi::BreakpointUpdateParams::fromJson(params, p, err)) return bad(err);
        if (!p.enabled && !p.condition) return bad("nothing to change: give enabled or condition");
        bool found = false, ok = true;
        std::string e;
        Breakpoint out {};
        Host->RunSync([&]()
        {
            Debugger& d = Host->Core().Dbg;
            for (const Breakpoint& bp : d.List()) if (bp.Id == p.id) found = true;
            if (!found) return;
            // the condition first: a bad one changes nothing
            if (p.condition) ok = d.SetBreakpointCondition(p.id, p.condition->trimmed().toStdString(), e);
            if (ok && p.enabled) d.SetBreakpointEnabled(p.id, *p.enabled);
            for (const Breakpoint& bp : d.List()) if (bp.Id == p.id) out = bp;
        });
        if (!found) return bad(QString("no breakpoint %1").arg(p.id));
        if (!ok) return bad(QString::fromStdString(e));
        emit Host->breakpointsChanged();
        Reply(sock, id, BreakpointJson(out).toJson());
        return;
    }

    if (method == "breakpoint.clear")
    {
        WBApi::BreakpointClearParams p;
        if (!WBApi::BreakpointClearParams::fromJson(params, p, err)) return bad(err);
        bool ok = false;
        Host->RunSync([&]() { ok = Host->Core().Dbg.RemoveBreakpoint(p.id); });
        if (!ok) return bad(QString("no breakpoint %1").arg(p.id));
        emit Host->breakpointsChanged();
        WBApi::Ok r;
        r.ok = true;
        Reply(sock, id, r.toJson());
        return;
    }

    if (method == "recording.configure")
    {
        WBApi::RecordingParams p;
        if (!WBApi::RecordingParams::fromJson(params, p, err)) return bad(err);
        if (Host->Core().JITActive() && (p.enabled.value_or(false) || p.write_history.value_or(false) || p.data_origin.value_or(false)))
        {
            Error(sock, id, ErrUnavailable, "Recording needs the interpreter: turn the JIT recompiler off in Emu settings.");
            return;
        }
        QString sessionFile;
        if (p.history_file_mb)
        {
            QString e;
            if (!Host->SetHistoryFile(*p.history_file_mb, e, false)) return bad(e);
        }
        if (p.history_cap) Host->SetHistoryCap(*p.history_cap);
        if (p.history_keep_mb) Host->Settings().HistoryKeepMB = std::max<u32>(*p.history_keep_mb, 1);
        if (p.history_include || p.history_exclude_writers || p.history_bulk_threshold || p.history_skip_stack || p.history_skip_unchanged)
        {
            auto ranges = [](const std::vector<WBApi::AddrRange>& v)
            {
                std::vector<std::pair<u32, u32>> out;
                for (const WBApi::AddrRange& r : v) out.push_back({r.start, r.end});
                return out;
            };
            Host->RunSync([&]()
            {
                Recorder::HistoryFilter f = Host->Core().Rec.GetHistoryFilter();
                if (p.history_include) f.Include = ranges(*p.history_include);
                if (p.history_exclude_writers) f.ExcludeWriters = ranges(*p.history_exclude_writers);
                if (p.history_bulk_threshold) f.BulkThreshold = *p.history_bulk_threshold;
                if (p.history_skip_stack) f.SkipStack = *p.history_skip_stack;
                if (p.history_skip_unchanged) f.SkipUnchanged = *p.history_skip_unchanged;
                Host->Core().Rec.SetHistoryFilter(f);
            });
        }
        if (p.origin_all_rom_files)
        {
            Host->Settings().OriginAllRomFiles = *p.origin_all_rom_files;
            bool v = *p.origin_all_rom_files;
            Host->RunSync([&]() { Host->Core().Taint.SetAllRomFiles(v); });
        }
        if (p.origin_follow_addresses)
        {
            Host->Settings().OriginFollowAddresses = *p.origin_follow_addresses;
            bool v = *p.origin_follow_addresses;
            Host->RunSync([&]() { Host->Core().Taint.SetFollowAddresses(v); });
        }
        if (p.save_session.value_or(false) && Host->Status().Recording)
        {
            // File > Save recording session now: a session file, and recording goes on
            QMetaObject::Connection cn = connect(Host, &WBHost::sessionSaved, this, [&sessionFile](QString path) { sessionFile = path; });
            Host->SaveSessionNow();
            disconnect(cn);
        }
        if (p.enabled.has_value() && !*p.enabled)
        {
            QMetaObject::Connection cn = connect(Host, &WBHost::sessionSaved, this, [&sessionFile](QString path) { sessionFile = path; });
            Host->SetRecording(false);
            disconnect(cn);
        }
        if (p.enabled.value_or(false)) Host->SetRecording(true);
        bool recOn = Host->Status().Recording || p.enabled.value_or(false);
        if ((p.write_history.value_or(false) || p.data_origin.value_or(false)) && !recOn)
        {
            Host->SetRecording(true);
        }
        if (p.write_history) Host->SetWriteHistory(*p.write_history);
        if (p.data_origin) Host->SetDataOrigin(*p.data_origin);

        QString srcErr;
        // checked here, then added and removed in one trip to the emulation thread: a long list doesn't
        // hold the window for a frame per source
        std::vector<DataSource> adds;
        for (const WBApi::OriginSourceSpec& s : p.sources_add)
        {
            DataSource ds;
            QString kind = s.kind.toLower();
            u32 cpu = 0;
            if (!WBQ::ParseCpu(s.cpu, cpu, srcErr)) break;
            ds.Cpu = cpu;
            ds.Name = s.name.toStdString();
            if (kind == "rom_file")
            {
                ds.Kind = SourceKind::RomFile;
                const RomFile* f = Host->Core().Rom.FileByPath(s.file.toStdString());
                if (!f) { srcErr = "no ROM file '" + s.file + "'"; break; }
                ds.FileId = f->Id;
                if (ds.Name.empty()) ds.Name = f->Path;
            }
            else if (kind == "archive_file")
            {
                auto arc = Host->Archive();
                if (!arc) { srcErr = "no game archive (" + Host->ArchiveStatus() + ")"; break; }
                int i = arc->FindPath(s.file.toStdString());
                if (i < 0) { srcErr = "no archived file '" + s.file + "'"; break; }
                ds.Kind = SourceKind::RomFile;
                ds.FileId = arc->ArchiveFileId;
                ds.Addr = (u32)i + 1;
                if (ds.Name.empty()) ds.Name = arc->ArchivePath + ":" + arc->Members()[i].Path;
            }
            else if (kind == "container_file")
            {
                auto cont = Host->Containers();
                if (!cont) { srcErr = "no NARC or SDAT files (" + Host->ContainerStatus() + ")"; break; }
                int ci = -1, mi = -1;
                if (!cont->Find(s.file.toStdString(), ci, mi)) { srcErr = "no file '" + s.file + "' in a container (give it as /file.narc:name)"; break; }
                ds.Kind = SourceKind::RomFile;
                ds.FileId = cont->Containers()[ci].FileId;
                ds.Addr = (u32)mi + 1;
                if (ds.Name.empty()) ds.Name = cont->FullName(ci, mi);
            }
            else if (kind == "range")
            {
                if (!s.size) { srcErr = "range sources need a size"; break; }
                ds.Kind = SourceKind::Range;
                ds.Addr = s.addr;
                ds.Size = s.size;
            }
            else if (kind == "register")
            {
                if (s.reg > 15) { srcErr = "reg must be 0..15"; break; }
                ds.Kind = SourceKind::Register;
                ds.Reg = s.reg;
            }
            else if (kind == "keypad") ds.Kind = SourceKind::Keypad;
            else if (kind == "touch") ds.Kind = SourceKind::Touch;
            else if (kind == "rtc") ds.Kind = SourceKind::RTC;
            else { srcErr = "unknown source kind '" + s.kind + "'"; break; }
            adds.push_back(std::move(ds));
        }
        if (!adds.empty() || !p.sources_remove.empty())
            Host->RunSync([this, adds = std::move(adds), removes = p.sources_remove]() mutable
            {
                for (DataSource& ds : adds)
                {
                    if (ds.Kind == SourceKind::Register) ds.Addr = Host->Core().CurrentPC(ds.Cpu);
                    Host->Core().Taint.AddSource(ds);
                }
                for (u32 sid : removes) Host->Core().Taint.RemoveSource(sid);
            });
        if (!srcErr.isEmpty()) return bad(srcErr);
        Reply(sock, id, recordingResult(sessionFile).toJson());
        return;
    }

    if (method == "memory.write")
    {
        if (!MemWrite) { Error(sock, id, ErrDisabled, "memory writes are disabled; enable them in the workbench settings or start melonDS with --api-memory-write"); return; }
        WBApi::MemoryWriteParams p;
        if (!WBApi::MemoryWriteParams::fromJson(params, p, err)) return bad(err);
        u32 cpu;
        if (!WBQ::ParseCpu(p.cpu, cpu, err)) return bad(err);
        QByteArray bytes = QByteArray::fromHex(p.hex.toLatin1());
        if (bytes.isEmpty() || bytes.size() * 2 != p.hex.size()) return bad("hex must be an even number of hex digits");
        bool ok = true;
        u32 addr = p.addr;
        Host->RunSync([&]()
        {
            for (int i = 0; i < bytes.size(); i++)
                if (!Host->Core().Poke(cpu, addr + i, 1, (u8)bytes[i])) ok = false;
        });
        if (!ok) { Error(sock, id, ErrServer, "some bytes are not writable (IO, BIOS or unmapped)"); return; }
        WBApi::Ok r;
        r.ok = true;
        r.message = QString("%1 bytes written").arg(bytes.size());
        Reply(sock, id, r.toJson());
        return;
    }

    if (method == "ui.show")
    {
        WBApi::UiShowParams p;
        if (!WBApi::UiShowParams::fromJson(params, p, err)) return bad(err);
        WorkbenchWindow::UiPointRequest req;
        if (p.cpu && !p.cpu->trimmed().isEmpty())
        {
            u32 cpu;
            if (!WBQ::ParseCpu(p.cpu->trimmed(), cpu, err)) return bad(err);
            req.Cpu = (int)cpu;
        }
        if (p.addr) { req.HasAddr = true; req.Addr = *p.addr; }
        if (p.sel_addr) { req.HasSelAddr = true; req.SelAddr = *p.sel_addr; }
        if (p.sel_size) { req.HasSelSize = true; req.SelSize = *p.sel_size; }
        if (p.panel && !p.panel->trimmed().isEmpty())
            if (!WorkbenchWindow::ResolvePanel(p.panel->trimmed(), req.Panel, err)) return bad(err);
        if (p.note) req.Note = p.note->trimmed();
        bool any = req.Cpu >= 0 || req.HasAddr || req.HasSelAddr || req.HasSelSize || !req.Panel.isEmpty() || !req.Note.isEmpty();
        if (!any) return bad("say what to show: an address, a selection, a panel, a CPU or a note");
        // arguments are checked before the window exists, so a bad call does not open it
        WorkbenchWindow* w = Host->Window(true);
        WorkbenchWindow::UiPointState st;
        if (!w->PointAt(req, st, err)) return bad(err);
        WBApi::UiShowResult r;
        r.cpu = st.Cpu ? "arm7" : "arm9";
        r.addr = st.Addr;
        r.sel_addr = st.SelAddr;
        r.sel_size = st.SelSize;
        r.panel = st.Panel;
        r.follow_pc = st.FollowPC;
        Reply(sock, id, r.toJson());
        return;
    }

    if (method == "ui.capture")
    {
        WBApi::UiCaptureParams p;
        if (!WBApi::UiCaptureParams::fromJson(params, p, err)) return bad(err);
        QString dock;
        if (p.panel && !p.panel->trimmed().isEmpty())
            if (!WorkbenchWindow::ResolvePanel(p.panel->trimmed(), dock, err)) return bad(err);
        WorkbenchWindow* w = Host->Window(false);
        if (!w || !w->isVisible()) { Error(sock, id, ErrUnavailable, "the workbench is not open"); return; }
        if (w->isMinimized()) { Error(sock, id, ErrUnavailable, "the workbench is minimized"); return; }
        int maxWidth = (int)p.max_width;
        w->CaptureUi(dock, maxWidth, [this, sock, id](bool ok, QImage img, QString panel, bool stale, QString e)
        {
            if (!ok) { Error(sock, id, ErrUnavailable, e); return; }
            QByteArray ba;
            QBuffer buf(&ba);
            buf.open(QIODevice::WriteOnly);
            img.save(&buf, "PNG");
            WBApi::UiCaptureResult r;
            r.png = QString::fromLatin1(ba.toBase64());
            r.width = (u32)img.width();
            r.height = (u32)img.height();
            r.panel = panel;
            r.stale = stale;
            Reply(sock, id, r.toJson());
        });
        return;
    }

    Error(sock, id, ErrMethod, "method not implemented: " + method);
}
