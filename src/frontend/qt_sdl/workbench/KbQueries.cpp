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
#include <QFile>
#include <QFileInfo>

#include "KbQueries.h"

namespace WBQ
{
using melonDS::u32;
using namespace WBKB;

static QString Hex8(u32 a) { return QString("%1").arg(a, 8, 16, QChar('0')); }
static QString CpuStr(int cpu) { return cpu ? "arm7" : "arm9"; }

static QString ModuleName(const KnowledgeStore& kb, int id)
{
    const Module* m = kb.ModuleById(id);
    return m ? m->Name : QString();
}

static int ModuleOverlay(const KnowledgeStore& kb, int id)
{
    const Module* m = kb.ModuleById(id);
    return m && m->Kind == "overlay" ? m->OverlayId : -1;
}

static int ModuleCpu(const KnowledgeStore& kb, int id)
{
    const Module* m = kb.ModuleById(id);
    return m ? m->CpuNum() : 0;
}

static u32 FunctionEnd(const Function& f)
{
    return f.Addr + std::max<u32>(f.Size, f.Thumb() ? 2 : 4);
}

WBApi::KbFunctionInfo KbFunction(const KnowledgeStore& kb, const Function& f)
{
    WBApi::KbFunctionInfo x;
    x.id = (u32)f.Id;
    x.addr = f.Addr;
    x.size = f.Size;
    x.mode = f.Mode;
    x.cpu = CpuStr(ModuleCpu(kb, f.ModuleId));
    x.module = ModuleName(kb, f.ModuleId);
    x.overlay = ModuleOverlay(kb, f.ModuleId);
    x.name = f.Name;
    x.signature = f.Signature;
    x.subsystem = f.Subsystem;
    x.source_file = f.SourceFile;
    x.status = f.Status;
    x.last_score = f.LastScore;
    x.author = f.Author;
    x.author_kind = f.AuthorKind;
    return x;
}

// the function an annotation is on (function and register targets)
static const Function* AnnotationFunction(const KnowledgeStore& kb, const Annotation& a)
{
    return a.FunctionId ? kb.FunctionById(a.FunctionId) : nullptr;
}

static int AnnotationModule(const KnowledgeStore& kb, const Annotation& a)
{
    if (a.ModuleId >= 0) return a.ModuleId;
    if (const Function* f = AnnotationFunction(kb, a)) return f->ModuleId;
    if (a.DataObjectId)
        for (const DataObject& d : kb.DataObjects())
            if (d.Id == a.DataObjectId) return d.ModuleId;
    return -1;
}

WBApi::KbAnnotationInfo KbAnnotation(const KnowledgeStore& kb, const Annotation& a)
{
    WBApi::KbAnnotationInfo x;
    const Function* f = AnnotationFunction(kb, a);
    x.id = (u32)a.Id;
    x.target_kind = a.TargetKind;
    x.has_addr = a.HasAddr;
    if (a.HasAddr) { x.addr = a.Addr; x.end = a.EndAddr; }
    if (a.HasAddr || f) x.cpu = CpuStr(a.Cpu);
    x.module = ModuleName(kb, AnnotationModule(kb, a));
    x.function = f ? f->Addr : 0;
    x.function_id = (u32)a.FunctionId;
    x.data_object_id = (u32)a.DataObjectId;
    x.type_name = a.TypeName;
    x.field_name = a.FieldName;
    x.reg = a.Register;
    x.overlay_id = a.OverlayId;
    x.savestate = a.Savestate;
    x.comment = a.Comment;
    for (const QString& t : a.TagList()) x.tags.push_back(t);
    x.author = a.Author;
    x.author_kind = a.AuthorKind;
    x.created = a.Created;
    x.updated = a.Updated;
    return x;
}

std::vector<const Annotation*> FunctionAnnotations(const KnowledgeStore& kb, const Function& f)
{
    std::vector<const Annotation*> out;
    int cpu = ModuleCpu(kb, f.ModuleId);
    u32 end = FunctionEnd(f);
    for (const Annotation& a : kb.Annotations())
    {
        if (a.FunctionId == f.Id) out.push_back(&a);
        else if (!a.FunctionId && a.HasAddr && a.Cpu == cpu && a.Addr >= f.Addr && a.Addr < end
                 && (a.ModuleId < 0 || a.ModuleId == f.ModuleId))
            out.push_back(&a);
    }
    return out;
}

namespace
{
// the filters of a kb.list call, checked once
struct ListFilter
{
    const KnowledgeStore& Kb;
    const WBApi::KbListParams& P;
    int Cpu = -1;               // -1 = any
    QString Author;             // "", people, agents, or a name
    QString Text, Module;
    bool Ranged = false;
    u32 Start = 0, End = 0;

    ListFilter(const KnowledgeStore& kb, const WBApi::KbListParams& p) : Kb(kb), P(p) {}

    bool AuthorOk(const QString& name, const QString& kind) const
    {
        if (Author.isEmpty()) return true;
        if (Author == "people") return kind == "person";
        if (Author == "agents") return kind == "agent";
        return name == Author;
    }
    bool CpuOk(int cpu) const { return Cpu < 0 || cpu == Cpu; }
    bool IdOk(qint64 id) const { return !P.id || id == (qint64)P.id; }
    bool ModuleOk(int id) const
    {
        if (Module.isEmpty()) return true;
        const WBKB::Module* m = Kb.ModuleById(id);
        return m && m->Name.compare(Module, Qt::CaseInsensitive) == 0;
    }
    // the module a bare address is in, for entries that don't store one
    bool ModuleOkAt(int cpu, u32 addr) const
    {
        if (Module.isEmpty()) return true;
        const WBKB::Module* m = Kb.ModuleFor(cpu, addr, -1);
        return m && ModuleOk(m->Id);
    }
    bool RangeOk(u32 start, u32 end) const { return !Ranged || (start < End && end > Start); }
    bool TextOk(std::initializer_list<QString> fields) const
    {
        if (Text.isEmpty()) return true;
        for (const QString& f : fields)
            if (f.contains(Text, Qt::CaseInsensitive)) return true;
        return false;
    }
};

// keeps the matches in [offset, offset + limit) and counts all of them
template <typename Info, typename Range, typename Match, typename Conv>
void Page(const Range& src, Match match, Conv conv, const WBApi::KbListParams& p, std::optional<std::vector<Info>>& out, WBApi::KbListResult& r)
{
    u32 limit = std::min<u32>(p.limit, 100000);
    std::vector<Info> v;
    u32 n = 0;
    for (const auto& x : src)
    {
        if (!match(x)) continue;
        if (n >= p.offset && v.size() < limit) v.push_back(conv(x));
        n++;
    }
    r.total = n;
    r.next_offset = p.offset + (u32)v.size();
    r.truncated = r.next_offset < n;
    out = std::move(v);
}
}

bool KbList(const KnowledgeStore& kb, const WBApi::KbListParams& p, WBApi::KbListResult& out, QString& err,
            const std::map<qint64, QString>* conflicts)
{
    static const QStringList kinds = {"functions", "labels", "data_objects", "types", "annotations", "bookmarks", "decode_overrides", "modules"};
    QString kind = p.kind.trimmed().toLower();
    if (!kinds.contains(kind)) { err = "kind must be one of " + kinds.join(", "); return false; }
    out.kind = kind;

    ListFilter f(kb, p);
    QString cpu = p.cpu.trimmed().toLower();
    if (cpu == "arm9") f.Cpu = 0;
    else if (cpu == "arm7") f.Cpu = 1;
    else if (!cpu.isEmpty() && cpu != "any") { err = "cpu must be any, arm9 or arm7"; return false; }
    QString author = p.author.trimmed();
    if (author.compare("all", Qt::CaseInsensitive) == 0) author.clear();
    else if (author.compare("people", Qt::CaseInsensitive) == 0) author = "people";
    else if (author.compare("agents", Qt::CaseInsensitive) == 0) author = "agents";
    f.Author = author;
    f.Text = p.filter.trimmed();
    f.Module = p.module.trimmed();
    if (p.addr || p.end)
    {
        f.Ranged = true;
        f.Start = p.addr;
        f.End = p.end ? p.end : p.addr + 1;
        if (f.End <= f.Start) { err = "end must be above addr"; return false; }
    }
    QString status = p.status.trimmed().toLower();
    QString targetKind = p.target_kind.trimmed().toLower();
    QString tag = p.tag.trimmed();

    // filters that only some kinds have
    auto refuse = [&](bool given, const char* what, bool applies)
    {
        if (!given || applies) return false;
        err = QString("%1 does not apply to %2").arg(what, kind);
        return true;
    };
    if (refuse(!status.isEmpty(), "status", kind == "functions")
        || refuse(!targetKind.isEmpty(), "target_kind", kind == "annotations")
        || refuse(!tag.isEmpty(), "tag", kind == "annotations")
        || refuse(p.conflicts_only, "conflicts_only", kind == "decode_overrides")
        || refuse(!f.Author.isEmpty(), "author", kind != "modules")
        || refuse(f.Cpu >= 0, "cpu", kind != "types")
        || refuse(!f.Module.isEmpty(), "module", kind != "types")
        || refuse(f.Ranged, "an address range", kind != "types"))
        return false;
    static const QStringList statuses = {"not_started", "partially_matched", "matched"};
    if (!status.isEmpty() && !statuses.contains(status)) { err = "status must be one of " + statuses.join(", "); return false; }
    static const QStringList targets = {"address", "range", "function", "data_object", "struct_field", "type", "register", "overlay", "savestate"};
    if (!targetKind.isEmpty() && !targets.contains(targetKind)) { err = "target_kind must be one of " + targets.join(", "); return false; }

    if (kind == "functions")
    {
        Page(kb.Functions(), [&](const Function& x)
        {
            QString name = x.Name.isEmpty() ? "func_" + Hex8(x.Addr) : x.Name;
            return f.IdOk(x.Id) && f.CpuOk(ModuleCpu(kb, x.ModuleId)) && f.ModuleOk(x.ModuleId) && f.AuthorOk(x.Author, x.AuthorKind)
                && f.RangeOk(x.Addr, FunctionEnd(x)) && f.TextOk({name, Hex8(x.Addr)}) && (status.isEmpty() || x.Status == status);
        }, [&](const Function& x) { return KbFunction(kb, x); }, p, out.functions, out);
    }
    else if (kind == "labels")
    {
        Page(kb.Labels(), [&](const Label& x)
        {
            return f.IdOk(x.Id) && f.CpuOk(ModuleCpu(kb, x.ModuleId)) && f.ModuleOk(x.ModuleId) && f.AuthorOk(x.Author, x.AuthorKind)
                && f.RangeOk(x.Addr, x.Addr + 1) && f.TextOk({x.Name, Hex8(x.Addr)});
        }, [&](const Label& x)
        {
            WBApi::KbLabelInfo i;
            i.id = (u32)x.Id; i.addr = x.Addr; i.cpu = CpuStr(ModuleCpu(kb, x.ModuleId));
            i.module = ModuleName(kb, x.ModuleId); i.overlay = ModuleOverlay(kb, x.ModuleId);
            i.name = x.Name; i.author = x.Author; i.author_kind = x.AuthorKind;
            return i;
        }, p, out.labels, out);
    }
    else if (kind == "data_objects")
    {
        Page(kb.DataObjects(), [&](const DataObject& x)
        {
            // data outside the ROM's modules is keyed by region and found from either CPU
            bool anyCpu = x.ModuleId < 0;
            QString name = x.Name.isEmpty() ? "data_" + Hex8(x.Addr) : x.Name;
            return f.IdOk(x.Id) && (anyCpu || f.CpuOk(ModuleCpu(kb, x.ModuleId)))
                && (anyCpu ? f.ModuleOkAt(f.Cpu < 0 ? 0 : f.Cpu, x.Addr) : f.ModuleOk(x.ModuleId))
                && f.AuthorOk(x.Author, x.AuthorKind) && f.RangeOk(x.Addr, x.Addr + std::max<u32>(x.Size, 1))
                && f.TextOk({name, Hex8(x.Addr), x.TypeName});
        }, [&](const DataObject& x)
        {
            WBApi::KbDataObjectInfo i;
            i.id = (u32)x.Id; i.addr = x.Addr; i.size = x.Size;
            i.cpu = x.ModuleId < 0 ? "any" : CpuStr(ModuleCpu(kb, x.ModuleId));
            i.module = ModuleName(kb, x.ModuleId); i.overlay = ModuleOverlay(kb, x.ModuleId); i.region = x.Region;
            i.name = x.Name; i.type = x.TypeName;
            const Function* owner = x.OwnerFunction ? kb.FunctionById(x.OwnerFunction) : nullptr;
            i.owner_function = owner ? owner->Addr : 0;
            i.author = x.Author; i.author_kind = x.AuthorKind;
            return i;
        }, p, out.data_objects, out);
    }
    else if (kind == "types")
    {
        Page(kb.Types(), [&](const std::pair<const QString, Type>& kv)
        {
            const Type& x = kv.second;
            return f.IdOk(x.Id) && f.AuthorOk(x.Author, x.AuthorKind) && f.TextOk({x.Name});
        }, [&](const std::pair<const QString, Type>& kv)
        {
            const Type& x = kv.second;
            WBApi::KbTypeInfo i;
            i.id = (u32)x.Id; i.name = x.Name; i.kind = x.Kind; i.size = x.Size; i.definition = x.Definition;
            for (const TypeField& tf : x.Fields)
            {
                WBApi::KbTypeField o;
                o.name = tf.Name; o.ctype = tf.CType; o.offset = tf.Offset; o.size = tf.Size; o.count = tf.Count;
                o.bit_offset = tf.BitOffset; o.bit_width = (u32)std::max(tf.BitWidth, 0);
                i.fields.push_back(o);
            }
            i.author = x.Author; i.author_kind = x.AuthorKind;
            return i;
        }, p, out.types, out);
    }
    else if (kind == "annotations")
    {
        Page(kb.Annotations(), [&](const Annotation& x)
        {
            if (!f.IdOk(x.Id) || !f.AuthorOk(x.Author, x.AuthorKind)) return false;
            if (!targetKind.isEmpty() && x.TargetKind != targetKind) return false;
            if (!tag.isEmpty() && !x.TagList().contains(tag, Qt::CaseInsensitive)) return false;
            if (!f.TextOk({x.Comment, x.Tags})) return false;
            // where it is: its address range, or the function it is on
            const Function* fn = AnnotationFunction(kb, x);
            bool located = x.HasAddr || fn;
            if ((f.Cpu >= 0 || f.Ranged || !f.Module.isEmpty()) && !located) return false;
            if (!f.CpuOk(x.Cpu) || !f.ModuleOk(AnnotationModule(kb, x))) return false;
            if (f.Ranged && !(x.HasAddr ? f.RangeOk(x.Addr, x.EndAddr) : f.RangeOk(fn->Addr, FunctionEnd(*fn)))) return false;
            return true;
        }, [&](const Annotation& x) { return KbAnnotation(kb, x); }, p, out.annotations, out);
    }
    else if (kind == "bookmarks")
    {
        Page(kb.Bookmarks(), [&](const Bookmark& x)
        {
            return f.IdOk(x.Id) && f.CpuOk(x.Cpu) && f.ModuleOkAt(x.Cpu, x.Address) && f.AuthorOk(x.Author, x.AuthorKind)
                && f.RangeOk(x.Address, x.Address + 1) && f.TextOk({x.Name, Hex8(x.Address)});
        }, [&](const Bookmark& x)
        {
            WBApi::KbBookmarkInfo i;
            i.id = (u32)x.Id; i.addr = x.Address; i.cpu = CpuStr(x.Cpu); i.name = x.Name;
            i.author = x.Author; i.author_kind = x.AuthorKind;
            return i;
        }, p, out.bookmarks, out);
    }
    else if (kind == "decode_overrides")
    {
        auto conflictOf = [&](const DecodeOverride& x)
        {
            if (!conflicts) return QString();
            auto it = conflicts->find(x.Id);
            return it == conflicts->end() ? QString() : it->second;
        };
        Page(kb.DecodeOverrides(), [&](const DecodeOverride& x)
        {
            // an override without a module applies whatever is loaded: it is in the module there
            return f.IdOk(x.Id) && f.CpuOk(x.Cpu) && (x.ModuleId >= 0 ? f.ModuleOk(x.ModuleId) : f.ModuleOkAt(x.Cpu, x.Start))
                && f.AuthorOk(x.Author, x.AuthorKind) && f.RangeOk(x.Start, x.End) && f.TextOk({Hex8(x.Start), x.Mode})
                && (!p.conflicts_only || !conflictOf(x).isEmpty());
        }, [&](const DecodeOverride& x)
        {
            WBApi::KbDecodeOverrideInfo i;
            i.id = (u32)x.Id; i.start = x.Start; i.end = x.End; i.cpu = CpuStr(x.Cpu);
            i.module = ModuleName(kb, x.ModuleId); i.overlay = ModuleOverlay(kb, x.ModuleId);
            i.mode = x.Mode; i.author = x.Author; i.author_kind = x.AuthorKind;
            i.conflict = conflictOf(x);
            return i;
        }, p, out.decode_overrides, out);
    }
    else
    {
        Page(kb.Modules(), [&](const WBKB::Module& x)
        {
            return f.IdOk(x.Id) && f.CpuOk(x.CpuNum()) && f.ModuleOk(x.Id) && f.RangeOk(x.Load, std::max(x.End(), x.Load + 1))
                && f.TextOk({x.Name, x.RomFile});
        }, [&](const WBKB::Module& x)
        {
            WBApi::KbModuleInfo i;
            i.id = (u32)x.Id; i.name = x.Name; i.kind = x.Kind; i.cpu = x.Cpu;
            i.overlay_id = x.Kind == "overlay" ? x.OverlayId : -1;
            i.load = x.Load; i.size = x.Size; i.bss = x.Bss; i.end = x.End();
            i.rom_file = x.RomFile; i.compressed = x.Compressed;
            return i;
        }, p, out.modules, out);
    }
    return true;
}

// the source file's text, only from inside the source root: kb.function.update can point
// source_file anywhere, and this must not become a way to read any file over the API
static QString ReadSource(const QString& path, const QString& root, QString& text)
{
    if (path.isEmpty()) return "none";
    QString file = QFileInfo(path).canonicalFilePath();
    QString base = root.isEmpty() ? QString() : QFileInfo(root).canonicalFilePath();
    if (file.isEmpty()) return QFileInfo::exists(path) ? "unreadable" : "missing";
    if (base.isEmpty()) return "outside_source_root";
    if (!base.endsWith('/')) base += '/';
#ifdef Q_OS_WIN
    Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
    Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
    if (!file.startsWith(base, cs)) return "outside_source_root";
    QFile f(file);
    if (f.size() > 4 * 1024 * 1024) return "too_large";
    if (!f.open(QIODevice::ReadOnly)) return "unreadable";
    text = QString::fromUtf8(f.readAll());
    return "ok";
}

WBApi::KbFunctionDetail KbFunctionDetail(KnowledgeStore& kb, const Function& f, const QString& sourcePath,
                                         const QString& sourceRoot, bool readSource, int attempts,
                                         const std::function<int(u32)>& overlayAt)
{
    WBApi::KbFunctionDetail d;
    int cpu = ModuleCpu(kb, f.ModuleId);
    d.function = KbFunction(kb, f);
    d.display_name = kb.NameAt(cpu, f.Addr, ModuleOverlay(kb, f.ModuleId));
    d.source_path = sourcePath;
    if (!readSource) d.source_status = sourcePath.isEmpty() ? "none" : "not_requested";
    else d.source_status = ReadSource(sourcePath, sourceRoot, d.source_text);
    for (const BuildResult& r : kb.BuildResults(f.Id, std::max(attempts, 0)))
    {
        WBApi::KbBuildResultInfo b;
        b.id = (u32)r.Id; b.created = r.Created; b.compiler = r.Compiler; b.flags = r.Flags; b.score = r.Score; b.matched = r.Matched;
        b.diff = r.Diff; b.commit = r.CommitHash; b.source_file = r.SourceFile; b.author = r.Author;
        d.attempts.push_back(b);
    }
    d.decompiled = kb.DecompilerText(f.Id, &d.decompiler_tool);
    if (!d.decompiled.isEmpty()) d.decompiled_named = kb.NameDecompiled(d.decompiled, cpu, overlayAt);
    for (const Annotation* a : FunctionAnnotations(kb, f)) d.annotations.push_back(KbAnnotation(kb, *a));
    return d;
}

}
