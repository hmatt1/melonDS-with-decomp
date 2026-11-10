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
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <sqlite3.h>

#include "KnowledgeStore.h"
#include "NitroROM.h"

namespace WBKB
{

QStringList Annotation::TagList() const
{
    QStringList out;
    for (const QString& t : Tags.split(',', Qt::SkipEmptyParts)) out << t.trimmed();
    return out;
}

// ---------------------------------------------------------------- statement helper

struct KnowledgeStore::Stmt
{
    sqlite3_stmt* S = nullptr;
    sqlite3* D;
    int Rc = SQLITE_OK;
    Stmt(sqlite3* db, const char* sql) : D(db) { Rc = sqlite3_prepare_v2(db, sql, -1, &S, nullptr); }
    ~Stmt() { if (S) sqlite3_finalize(S); }
    bool Ok() const { return S != nullptr; }
    Stmt& Bind(int i, qint64 v) { sqlite3_bind_int64(S, i, v); return *this; }
    Stmt& Bind(int i, double v) { sqlite3_bind_double(S, i, v); return *this; }
    Stmt& Bind(int i, const QString& v)
    {
        QByteArray b = v.toUtf8();
        sqlite3_bind_text(S, i, b.constData(), b.size(), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& BindNull(int i) { sqlite3_bind_null(S, i); return *this; }
    Stmt& BindOpt(int i, const QString& v) { return v.isEmpty() ? BindNull(i) : Bind(i, v); }
    bool Step() { Rc = sqlite3_step(S); return Rc == SQLITE_ROW; }
    bool Run() { Rc = sqlite3_step(S); return Rc == SQLITE_DONE || Rc == SQLITE_ROW; }
    qint64 Int(int c) const { return sqlite3_column_int64(S, c); }
    double Dbl(int c) const { return sqlite3_column_double(S, c); }
    bool Null(int c) const { return sqlite3_column_type(S, c) == SQLITE_NULL; }
    QString Str(int c) const
    {
        const unsigned char* t = sqlite3_column_text(S, c);
        return t ? QString::fromUtf8((const char*)t) : QString();
    }
    QString Err() const { return QString::fromUtf8(sqlite3_errmsg(D)); }
};

KnowledgeStore::KnowledgeStore(QObject* parent) : QObject(parent) {}

KnowledgeStore::~KnowledgeStore() { Close(); }

void KnowledgeStore::Close()
{
    if (Db) sqlite3_close(Db);
    Db = nullptr;
    ModuleList.clear();
    FunctionList.clear();
    LabelList.clear();
    DataList.clear();
    TypeMap.clear();
    AnnotationList.clear();
    BookmarkList.clear();
    OverrideList.clear();
    FunctionIndex.clear();
    History.clear();
    DataVersion = -1;
}

bool KnowledgeStore::Exec(const char* sql, QString& error)
{
    char* err = nullptr;
    if (sqlite3_exec(Db, sql, nullptr, nullptr, &err) != SQLITE_OK)
    {
        error = QString::fromUtf8(err ? err : "sqlite error");
        sqlite3_free(err);
        return false;
    }
    return true;
}

bool KnowledgeStore::Open(const QString& path, const melonDS::WB::NitroROM& rom, const QString& personName, QString& error)
{
    Close();
    QDir().mkpath(QFileInfo(path).absolutePath());
    if (sqlite3_open_v2(path.toUtf8().constData(), &Db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK)
    {
        error = Db ? QString::fromUtf8(sqlite3_errmsg(Db)) : "cannot open database";
        Close();
        return false;
    }
    DbPath = path;
    sqlite3_busy_timeout(Db, 3000);
    if (!Exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;", error)) { Close(); return false; }
    // migrations run before foreign keys are enforced so they can rebuild tables
    if (!Migrate(error)) { Close(); return false; }
    if (!Exec("PRAGMA foreign_keys=ON;", error)) { Close(); return false; }

    Person = personName.isEmpty() ? QString("person") : personName;
    PersonId = AuthorId(Person, "person");
    ActiveAuthor = PersonId;
    ActiveSession.clear();

    {
        Stmt s(Db, "SELECT value FROM meta WHERE key='rom_sha1'");
        QString sha = QString::fromStdString(rom.Sha1Hex());
        if (s.Step())
        {
            if (s.Str(0) != sha && rom.Valid())
            {
                error = "knowledge store belongs to a different ROM (" + s.Str(0) + ")";
                Close();
                return false;
            }
        }
        else if (rom.Valid())
        {
            Stmt i(Db, "INSERT INTO meta(key, value) VALUES ('rom_sha1', ?), ('rom_title', ?), ('rom_gamecode', ?)");
            i.Bind(1, sha).Bind(2, QString::fromLatin1(rom.Title)).Bind(3, QString::fromLatin1(rom.GameCode));
            i.Run();
        }
    }

    if (rom.Valid() && !EnsureModules(rom, error)) { Close(); return false; }
    LoadCaches();
    CheckExternalChanges();
    return true;
}

bool KnowledgeStore::Migrate(QString& error)
{
    if (!Exec("PRAGMA foreign_keys=OFF", error)) return false;
    if (!Exec("CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, name TEXT NOT NULL, "
              "applied_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')))", error))
        return false;
    int current = 0;
    {
        Stmt s(Db, "SELECT ifnull(max(version), 0) FROM schema_migrations");
        if (s.Step()) current = (int)s.Int(0);
    }
    QDir dir(":/kb");
    QStringList files = dir.entryList({"*.sql"}, QDir::Files, QDir::Name);
    for (const QString& f : files)
    {
        int v = f.section('_', 0, 0).toInt();
        if (v <= current) continue;
        QFile file(dir.filePath(f));
        if (!file.open(QIODevice::ReadOnly)) { error = "cannot read migration " + f; return false; }
        QByteArray sql = file.readAll();
        if (!Exec("BEGIN", error)) return false;
        if (!Exec(sql.constData(), error)) { QString e2; Exec("ROLLBACK", e2); error = f + ": " + error; return false; }
        Stmt ins(Db, "INSERT INTO schema_migrations(version, name) VALUES (?, ?)");
        ins.Bind(1, (qint64)v).Bind(2, f);
        if (!ins.Run()) { error = ins.Err(); QString e2; Exec("ROLLBACK", e2); return false; }
        if (!Exec("COMMIT", error)) return false;
        current = v;
    }
    Version = current;
    return true;
}

qint64 KnowledgeStore::AuthorId(const QString& name, const QString& kind)
{
    {
        Stmt s(Db, "SELECT id FROM author WHERE name=?");
        s.Bind(1, name);
        if (s.Step()) return s.Int(0);
    }
    Stmt i(Db, "INSERT INTO author(name, kind) VALUES (?, ?)");
    i.Bind(1, name).Bind(2, kind);
    i.Run();
    return LastInsertId();
}

qint64 KnowledgeStore::LastInsertId() const { return sqlite3_last_insert_rowid(Db); }

bool KnowledgeStore::EnsureModules(const melonDS::WB::NitroROM& rom, QString& error)
{
    for (const melonDS::WB::ModuleInfo& m : rom.Modules)
    {
        Stmt s(Db, "INSERT INTO module(name, kind, cpu, overlay_id, load_address, size, bss_size, rom_file, compressed) "
                   "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT(name) DO UPDATE SET load_address=excluded.load_address, "
                   "size=excluded.size, bss_size=excluded.bss_size, compressed=excluded.compressed");
        s.Bind(1, QString::fromStdString(m.Name)).Bind(2, QString::fromStdString(m.Kind)).Bind(3, QString(m.Cpu ? "arm7" : "arm9"));
        if (m.OverlayId >= 0) s.Bind(4, (qint64)m.OverlayId); else s.BindNull(4);
        s.Bind(5, (qint64)m.LoadAddress).Bind(6, (qint64)m.Size).Bind(7, (qint64)m.BssSize);
        const melonDS::WB::RomFile* f = rom.FileById(m.FileId);
        if (f) s.Bind(8, QString::fromStdString(f->Path)); else s.BindNull(8);
        s.Bind(9, (qint64)(m.Compressed ? 1 : 0));
        if (!s.Run()) { error = s.Err(); return false; }
    }
    // region modules: a home for code and data outside the ROM's modules
    struct Reg { const char* name; const char* cpu; u32 load, size; };
    static const Reg regions[] = {
        {"ram9_main", "arm9", 0x02000000, 0x400000}, {"ram9_itcm", "arm9", 0x01FF8000, 0x8000},
        {"ram9_wram", "arm9", 0x03000000, 0x8000}, {"ram7_main", "arm7", 0x02000000, 0x400000},
        {"ram7_wram", "arm7", 0x03800000, 0x10000},
    };
    for (const Reg& r : regions)
    {
        Stmt s(Db, "INSERT OR IGNORE INTO module(name, kind, cpu, load_address, size) VALUES (?, 'region', ?, ?, ?)");
        s.Bind(1, QString(r.name)).Bind(2, QString(r.cpu)).Bind(3, (qint64)r.load).Bind(4, (qint64)r.size);
        if (!s.Run()) { error = s.Err(); return false; }
    }

    // assets (NitroFS files)
    Exec("BEGIN", error);
    for (const melonDS::WB::RomFile& f : rom.Files)
    {
        Stmt s(Db, "INSERT OR IGNORE INTO asset(file_id, path, rom_offset, size) VALUES (?, ?, ?, ?)");
        s.Bind(1, (qint64)f.Id).Bind(2, QString::fromStdString(f.Path)).Bind(3, (qint64)f.Start).Bind(4, (qint64)(f.End - f.Start));
        s.Run();
    }
    Exec("COMMIT", error);
    return true;
}

bool KnowledgeStore::CheckExternalChanges()
{
    if (!Db) return false;
    Stmt s(Db, "PRAGMA data_version");
    if (!s.Step()) return false;
    long long v = s.Int(0);
    if (v == DataVersion) return false;
    bool first = DataVersion < 0;
    DataVersion = v;
    if (first) return false;
    LoadCaches();
    emit changed();
    return true;
}

void KnowledgeStore::Reload()
{
    LoadCaches();
    emit changed();
}

namespace { bool BasicType(const QString& n, u32& size); }

// bytes of a data object's type: a defined type, a basic type, a pointer, or an array of one
// ("u32[4]"); 0 when unknown
static u32 DataTypeSize(const QString& typeName, const std::map<QString, Type>& types)
{
    static const QRegularExpression array("^(.*)\\[(\\d+)\\]$");
    QString t = typeName.trimmed();
    u32 count = 1;
    QRegularExpressionMatch m = array.match(t);
    if (m.hasMatch()) { t = m.captured(1).trimmed(); count = m.captured(2).toUInt(); }
    u32 size = 0;
    auto it = types.find(t);
    if (it != types.end() && it->second.Size > 0) size = (u32)it->second.Size;
    else if (t.endsWith('*')) size = 4;
    else if (!BasicType(t, size)) return 0;
    return size * count;
}

void KnowledgeStore::LoadCaches()
{
    ModuleList.clear();
    FunctionList.clear();
    LabelList.clear();
    DataList.clear();
    TypeMap.clear();
    AnnotationList.clear();
    BookmarkList.clear();
    OverrideList.clear();
    FunctionIndex.clear();
    if (!Db) return;

    {
        Stmt s(Db, "SELECT id, name, kind, cpu, ifnull(overlay_id, -1), load_address, size, bss_size, ifnull(rom_file, ''), compressed FROM module ORDER BY id");
        while (s.Step())
        {
            Module m;
            m.Id = (int)s.Int(0); m.Name = s.Str(1); m.Kind = s.Str(2); m.Cpu = s.Str(3); m.OverlayId = (int)s.Int(4);
            m.Load = (u32)s.Int(5); m.Size = (u32)s.Int(6); m.Bss = (u32)s.Int(7); m.RomFile = s.Str(8); m.Compressed = s.Int(9) != 0;
            ModuleList.push_back(m);
        }
    }
    {
        Stmt s(Db, "SELECT f.id, f.module_id, f.offset, f.size, f.mode, ifnull(f.name,''), ifnull(f.signature,''), f.subsystem, ifnull(f.source_file,''), "
                   "a.name, a.kind, st.status, ifnull(st.last_score, -1) FROM function f JOIN author a ON a.id = f.author_id "
                   "LEFT JOIN function_status st ON st.function_id = f.id");
        while (s.Step())
        {
            Function f;
            f.Id = s.Int(0); f.ModuleId = (int)s.Int(1); f.Offset = (u32)s.Int(2); f.Size = (u32)s.Int(3); f.Mode = s.Str(4);
            f.Name = s.Str(5); f.Signature = s.Str(6); f.Subsystem = s.Str(7); f.SourceFile = s.Str(8);
            f.Author = s.Str(9); f.AuthorKind = s.Str(10); f.Status = s.Str(11); f.LastScore = s.Dbl(12);
            const Module* m = ModuleById(f.ModuleId);
            f.Addr = (m ? m->Load : 0) + f.Offset;
            FunctionList.push_back(f);
        }
        std::sort(FunctionList.begin(), FunctionList.end(), [&](const Function& a, const Function& b)
        {
            return a.Addr < b.Addr;
        });
        for (size_t i = 0; i < FunctionList.size(); i++) FunctionIndex[FunctionList[i].Id] = i;
    }
    {
        Stmt s(Db, "SELECT l.id, l.module_id, l.offset, l.name, a.name, a.kind FROM label l JOIN author a ON a.id = l.author_id");
        while (s.Step())
        {
            Label l;
            l.Id = s.Int(0); l.ModuleId = (int)s.Int(1); l.Offset = (u32)s.Int(2); l.Name = s.Str(3); l.Author = s.Str(4); l.AuthorKind = s.Str(5);
            const Module* m = ModuleById(l.ModuleId);
            l.Addr = (m ? m->Load : 0) + l.Offset;
            LabelList.push_back(l);
        }
        std::sort(LabelList.begin(), LabelList.end(), [](const Label& a, const Label& b) { return a.Addr < b.Addr; });
    }
    {
        Stmt s(Db, "SELECT t.id, t.name, t.kind, t.definition, ifnull(t.size, -1), a.name, a.kind FROM type t JOIN author a ON a.id = t.author_id ORDER BY t.id");
        std::vector<Type> pending;
        while (s.Step())
        {
            Type t;
            t.Id = s.Int(0); t.Name = s.Str(1); t.Kind = s.Str(2); t.Definition = s.Str(3); t.Size = (int)s.Int(4); t.Author = s.Str(5); t.AuthorKind = s.Str(6);
            pending.push_back(t);
        }
        // resolve layouts in dependency order (repeat until no progress)
        for (int pass = 0; pass < 8 && !pending.empty(); pass++)
        {
            std::vector<Type> next;
            for (Type& t : pending)
            {
                Type parsed;
                QString err;
                if (ParseTypeDefinition(t.Definition, TypeMap, parsed, err))
                {
                    t.Fields = parsed.Fields;
                    if (parsed.Size >= 0) t.Size = parsed.Size;
                    TypeMap[t.Name] = t;
                }
                else next.push_back(t);
            }
            if (next.size() == pending.size())
            {
                for (Type& t : next) TypeMap[t.Name] = t;
                break;
            }
            pending.swap(next);
        }
    }
    {
        Stmt s(Db, "SELECT d.id, ifnull(d.module_id, -1), ifnull(d.region, ''), d.offset, ifnull(d.name, ''), ifnull(d.type_name, ''), d.size, "
                   "ifnull(d.owner_function_id, 0), a.name, a.kind FROM data_object d JOIN author a ON a.id = d.author_id");
        while (s.Step())
        {
            DataObject d;
            d.Id = s.Int(0); d.ModuleId = (int)s.Int(1); d.Region = s.Str(2); d.Offset = (u32)s.Int(3); d.Name = s.Str(4);
            d.TypeName = s.Str(5); d.Size = (u32)s.Int(6); d.OwnerFunction = s.Int(7); d.Author = s.Str(8); d.AuthorKind = s.Str(9);
            const Module* m = ModuleById(d.ModuleId);
            d.Addr = m ? m->Load + d.Offset : d.Offset;
            if (d.Size == 0) d.Size = DataTypeSize(d.TypeName, TypeMap);
            DataList.push_back(d);
        }
        std::sort(DataList.begin(), DataList.end(), [](const DataObject& a, const DataObject& b) { return a.Addr < b.Addr; });
    }
    {
        Stmt s(Db, "SELECT n.id, n.target_kind, ifnull(n.module_id, -1), ifnull(n.offset, -1), ifnull(n.end_offset, -1), ifnull(n.function_id, 0), "
                   "ifnull(n.data_object_id, 0), ifnull(n.type_name, ''), ifnull(n.field_name, ''), ifnull(n.register, ''), ifnull(n.overlay_id, -1), "
                   "ifnull(n.savestate, ''), n.comment, n.tags, a.name, a.kind, n.created_at, n.updated_at FROM annotation n JOIN author a ON a.id = n.author_id ORDER BY n.id");
        while (s.Step())
        {
            Annotation a;
            a.Id = s.Int(0); a.TargetKind = s.Str(1); a.ModuleId = (int)s.Int(2); a.Offset = s.Int(3); a.EndOffset = s.Int(4);
            a.FunctionId = s.Int(5); a.DataObjectId = s.Int(6); a.TypeName = s.Str(7); a.FieldName = s.Str(8); a.Register = s.Str(9);
            a.OverlayId = (int)s.Int(10); a.Savestate = s.Str(11); a.Comment = s.Str(12); a.Tags = s.Str(13); a.Author = s.Str(14);
            a.AuthorKind = s.Str(15); a.Created = s.Str(16); a.Updated = s.Str(17);
            const Module* m = ModuleById(a.ModuleId);
            if (a.Offset >= 0)
            {
                a.HasAddr = true;
                a.Addr = (m ? m->Load : 0) + (u32)a.Offset;
                a.EndAddr = a.EndOffset >= 0 ? (m ? m->Load : 0) + (u32)a.EndOffset : a.Addr + 1;
                a.Cpu = m ? m->CpuNum() : 0;
            }
            else if (a.FunctionId)
            {
                const Function* f = FunctionById(a.FunctionId);
                if (f)
                {
                    const Module* fm = ModuleById(f->ModuleId);
                    a.HasAddr = a.TargetKind == "function";
                    a.Addr = f->Addr;
                    a.EndAddr = f->Addr + std::max<u32>(f->Size, 1);
                    a.Cpu = fm ? fm->CpuNum() : 0;
                }
            }
            else if (a.DataObjectId)
            {
                for (const DataObject& d : DataList)
                    if (d.Id == a.DataObjectId)
                    {
                        a.HasAddr = true;
                        a.Addr = d.Addr;
                        a.EndAddr = d.Addr + std::max<u32>(d.Size, 1);
                    }
            }
            AnnotationList.push_back(a);
        }
    }
    {
        Stmt s(Db, "SELECT b.id, b.address, b.cpu, b.name, a.name, a.kind FROM bookmark b JOIN author a ON a.id = b.author_id ORDER BY b.id");
        while (s.Step())
        {
            Bookmark b;
            b.Id = s.Int(0); b.Address = (u32)s.Int(1); b.Cpu = s.Str(2) == "arm7" ? 1 : 0; b.Name = s.Str(3); b.Author = s.Str(4); b.AuthorKind = s.Str(5);
            BookmarkList.push_back(b);
        }
    }
    if (Version >= 5)
    {
        Stmt s(Db, "SELECT o.id, o.cpu, ifnull(o.module_id, -1), o.start_addr, o.end_addr, o.mode, a.name, a.kind "
                   "FROM decode_override o JOIN author a ON a.id = o.author_id ORDER BY o.cpu, o.start_addr");
        while (s.Step())
        {
            DecodeOverride o;
            o.Id = s.Int(0); o.Cpu = s.Str(1) == "arm7" ? 1 : 0; o.ModuleId = (int)s.Int(2);
            o.Start = (u32)s.Int(3); o.End = (u32)s.Int(4); o.Mode = s.Str(5); o.Author = s.Str(6); o.AuthorKind = s.Str(7);
            OverrideList.push_back(o);
        }
    }
}

// ---------------------------------------------------------------- lookups

const Module* KnowledgeStore::ModuleById(int id) const
{
    for (const Module& m : ModuleList)
        if (m.Id == id) return &m;
    return nullptr;
}

const Module* KnowledgeStore::ModuleFor(int cpu, u32 addr, int overlay) const
{
    const Module* staticHit = nullptr;
    const Module* overlayHit = nullptr;
    const Module* firstOverlay = nullptr;
    const Module* regionHit = nullptr;
    for (const Module& m : ModuleList)
    {
        if (m.CpuNum() != cpu) continue;
        if (addr < m.Load || addr >= m.End()) continue;
        if (m.Kind == "autoload") return &m;
        if (m.Kind == "static") staticHit = &m;
        else if (m.Kind == "region") regionHit = &m;
        else
        {
            if (overlay >= 0 && m.OverlayId == overlay) overlayHit = &m;
            if (!firstOverlay) firstOverlay = &m;
        }
    }
    if (overlayHit) return overlayHit;
    if (staticHit) return staticHit;
    if (firstOverlay) return firstOverlay;
    return regionHit;
}

bool KnowledgeStore::ModuleOffset(int cpu, u32 addr, int overlay, int& moduleId, u32& offset) const
{
    const Module* m = ModuleFor(cpu, addr, overlay);
    if (!m) return false;
    moduleId = m->Id;
    offset = addr - m->Load;
    return true;
}

static bool ModuleMatches(const KnowledgeStore& kb, int moduleId, int cpu, u32 addr, int overlay)
{
    const Module* m = kb.ModuleById(moduleId);
    if (!m || m->CpuNum() != cpu) return false;
    if (m->Kind == "overlay" && overlay >= 0 && m->OverlayId != overlay) return false;
    if (m->Kind == "overlay" && overlay < 0)
    {
        // without overlay context, accept only when no other module claims the address
        const Module* best = kb.ModuleFor(cpu, addr, -1);
        return best && best->Id == moduleId;
    }
    return true;
}

const Function* KnowledgeStore::FunctionAt(int cpu, u32 addr, int overlay) const
{
    auto it = std::upper_bound(FunctionList.begin(), FunctionList.end(), addr, [](u32 a, const Function& f) { return a < f.Addr; });
    while (it != FunctionList.begin())
    {
        --it;
        if (addr - it->Addr > 0x100000) break;
        u32 size = std::max<u32>(it->Size, it->Thumb() ? 2 : 4);
        if (addr < it->Addr + size && ModuleMatches(*this, it->ModuleId, cpu, it->Addr, overlay)) return &*it;
        if (it->Size && addr >= it->Addr + it->Size && addr - it->Addr > 0x10000) break;
    }
    return nullptr;
}

const Function* KnowledgeStore::FunctionStarting(int cpu, u32 addr, int overlay) const
{
    auto it = std::lower_bound(FunctionList.begin(), FunctionList.end(), addr, [](const Function& f, u32 a) { return f.Addr < a; });
    for (; it != FunctionList.end() && it->Addr == addr; ++it)
        if (ModuleMatches(*this, it->ModuleId, cpu, addr, overlay)) return &*it;
    return nullptr;
}

const Function* KnowledgeStore::FunctionById(qint64 id) const
{
    auto it = FunctionIndex.find(id);
    return it == FunctionIndex.end() ? nullptr : &FunctionList[it->second];
}

const Label* KnowledgeStore::LabelAt(int cpu, u32 addr, int overlay) const
{
    auto it = std::lower_bound(LabelList.begin(), LabelList.end(), addr, [](const Label& l, u32 a) { return l.Addr < a; });
    for (; it != LabelList.end() && it->Addr == addr; ++it)
        if (ModuleMatches(*this, it->ModuleId, cpu, addr, overlay)) return &*it;
    return nullptr;
}

const DataObject* KnowledgeStore::DataObjectAt(int cpu, u32 addr, int overlay) const
{
    auto it = std::upper_bound(DataList.begin(), DataList.end(), addr, [](u32 a, const DataObject& d) { return a < d.Addr; });
    while (it != DataList.begin())
    {
        --it;
        // objects are visited from the nearest start. One that ends before addr must not hide
        // an earlier object that is large enough to still cover it.
        u32 size = std::max<u32>(it->Size, 1);
        if (addr - it->Addr < size)
        {
            if (it->ModuleId < 0 || ModuleMatches(*this, it->ModuleId, cpu, it->Addr, overlay)) return &*it;
        }
    }
    return nullptr;
}

const DecodeOverride* KnowledgeStore::DecodeOverrideAt(int cpu, u32 addr, int overlay) const
{
    for (const DecodeOverride& o : OverrideList)
    {
        if (o.Cpu != cpu || addr < o.Start || addr >= o.End) continue;
        if (o.ModuleId < 0 || ModuleMatches(*this, o.ModuleId, cpu, o.Start, overlay)) return &o;
    }
    return nullptr;
}

bool KnowledgeStore::SetDecodeOverride(int cpu, u32 start, u32 end, const QString& mode, int overlay, QString& error)
{
    if (!Db) { error = "no knowledge store open"; return false; }
    QString m = mode.toLower();
    if (m != "arm" && m != "thumb" && m != "data" && m != "clear") { error = "mode must be arm, thumb, data or clear"; return false; }
    if (end <= start) { error = "the range is empty"; return false; }
    int mid = -1; u32 off;
    bool inModule = ModuleOffset(cpu, start, overlay, mid, off);
    const Module* mod = inModule ? ModuleById(mid) : nullptr;
    // only an overlay's overrides need its module: the others apply whatever is loaded
    int keyModule = mod && mod->Kind == "overlay" ? mid : -1;
    QString cpuName = cpu ? "arm7" : "arm9";

    auto update = [&](qint64 id, u32 s, u32 e)
    {
        QJsonObject before = RowJson("decode_override", id);
        Stmt u(Db, "UPDATE decode_override SET start_addr = ?, end_addr = ? WHERE id = ?");
        u.Bind(1, (qint64)s).Bind(2, (qint64)e).Bind(3, id);
        if (!u.Run()) { error = u.Err(); return false; }
        return LogChange("decode_override", id, "update", before, RowJson("decode_override", id));
    };
    auto insert = [&](u32 s, u32 e, const QString& md, int module)
    {
        Stmt i(Db, "INSERT INTO decode_override(cpu, module_id, start_addr, end_addr, mode, author_id) VALUES (?, ?, ?, ?, ?, ?)");
        i.Bind(1, cpuName);
        if (module >= 0) i.Bind(2, (qint64)module); else i.BindNull(2);
        i.Bind(3, (qint64)s).Bind(4, (qint64)e).Bind(5, md).Bind(6, ActiveAuthor);
        if (!i.Run()) { error = i.Err(); return false; }
        qint64 id = LastInsertId();
        return LogChange("decode_override", id, "insert", {}, RowJson("decode_override", id));
    };

    BeginGroup();
    bool ok = true;
    // overlapping overrides on the same code (same overlay, or none) are trimmed around the new range
    std::vector<DecodeOverride> old = OverrideList;
    for (const DecodeOverride& o : old)
    {
        if (!ok) break;
        if (o.Cpu != cpu || o.End <= start || o.Start >= end || o.ModuleId != keyModule) continue;
        if (o.Start >= start && o.End <= end)
        {
            QJsonObject before = RowJson("decode_override", o.Id);
            ok = DeleteRow("decode_override", o.Id, error) && LogChange("decode_override", o.Id, "delete", before, {});
        }
        else if (o.Start < start && o.End > end)
            ok = update(o.Id, o.Start, start) && insert(end, o.End, o.Mode, o.ModuleId);
        else if (o.Start < start)
            ok = update(o.Id, o.Start, start);
        else
            ok = update(o.Id, end, o.End);
    }
    if (ok && m != "clear") ok = insert(start, end, m, keyModule);
    EndGroup();
    Reload();
    return ok;
}

const Type* KnowledgeStore::TypeByName(const QString& name) const
{
    auto it = TypeMap.find(name);
    return it == TypeMap.end() ? nullptr : &it->second;
}

QString KnowledgeStore::NameAt(int cpu, u32 addr, int overlay, bool withOffset) const
{
    if (const Function* f = FunctionStarting(cpu, addr & ~1u, overlay))
        return f->Name.isEmpty() ? QString("func_%1").arg(f->Addr, 8, 16, QChar('0')) : f->Name;
    if (const Label* l = LabelAt(cpu, addr, overlay)) return l->Name;
    if (const DataObject* d = DataObjectAt(cpu, addr, overlay))
    {
        QString n = d->Name.isEmpty() ? QString("data_%1").arg(d->Addr, 8, 16, QChar('0')) : d->Name;
        if (d->Addr == addr) return n;
        if (!withOffset) return QString();
        // name a struct field when the type is known
        if (const Type* t = TypeByName(d->TypeName))
        {
            u32 off = addr - d->Addr;
            for (const TypeField& f : t->Fields)
                if (off >= f.Offset && off < f.Offset + f.Size * f.Count)
                {
                    if (off == f.Offset) return n + "." + f.Name;
                    return n + "." + f.Name + QString("+0x%1").arg(off - f.Offset, 0, 16);
                }
        }
        return n + QString("+0x%1").arg(addr - d->Addr, 0, 16);
    }
    if (withOffset)
        if (const Function* f = FunctionAt(cpu, addr, overlay))
        {
            QString n = f->Name.isEmpty() ? QString("func_%1").arg(f->Addr, 8, 16, QChar('0')) : f->Name;
            return n + QString("+0x%1").arg(addr - f->Addr, 0, 16);
        }
    return QString();
}

bool KnowledgeStore::LookupName(const QString& name, u32& addr, int& cpu) const
{
    for (const Function& f : FunctionList)
        if (f.Name == name) { addr = f.Addr; const Module* m = ModuleById(f.ModuleId); cpu = m ? m->CpuNum() : 0; return true; }
    for (const Label& l : LabelList)
        if (l.Name == name) { addr = l.Addr; const Module* m = ModuleById(l.ModuleId); cpu = m ? m->CpuNum() : 0; return true; }
    for (const DataObject& d : DataList)
        if (d.Name == name) { addr = d.Addr; const Module* m = ModuleById(d.ModuleId); cpu = m ? m->CpuNum() : 0; return true; }
    static const QRegularExpression rx("^(?:func|sub|FUN|data|DAT|lbl)_([0-9A-Fa-f]{7,8})$");
    auto mt = rx.match(name);
    if (mt.hasMatch()) { addr = mt.captured(1).toUInt(nullptr, 16); cpu = 0; return true; }
    return false;
}

std::vector<const Annotation*> KnowledgeStore::AnnotationsAt(int cpu, u32 addr, int overlay) const
{
    std::vector<const Annotation*> out;
    for (const Annotation& a : AnnotationList)
    {
        if (!a.HasAddr || a.TargetKind == "register") continue;
        if (a.Cpu != cpu) continue;
        if (a.ModuleId >= 0 && !ModuleMatches(*this, a.ModuleId, cpu, a.Addr, overlay)) continue;
        if (a.TargetKind == "function" || a.TargetKind == "data_object")
        {
            if (addr == a.Addr) out.push_back(&a);
        }
        else if (addr >= a.Addr && addr < a.EndAddr) out.push_back(&a);
    }
    return out;
}

std::vector<const Annotation*> KnowledgeStore::RegisterAnnotations(const Function* f, u32 pc) const
{
    std::vector<const Annotation*> out;
    for (const Annotation& a : AnnotationList)
    {
        if (a.TargetKind != "register") continue;
        if (f && a.FunctionId == f->Id)
        {
            if (a.HasAddr && a.Offset >= 0 && (pc < a.Addr || pc >= a.EndAddr)) continue;
            out.push_back(&a);
        }
        else if (!a.FunctionId && a.HasAddr && pc >= a.Addr && pc < a.EndAddr)
            out.push_back(&a);
    }
    return out;
}

std::vector<BuildResult> KnowledgeStore::BuildResults(qint64 functionId, int limit)
{
    std::vector<BuildResult> out;
    if (!Db) return out;
    Stmt s(Db, "SELECT b.id, b.compiler, b.flags, b.score, b.matched, b.diff, b.commit_hash, ifnull(b.source_file,''), a.name, b.created_at "
               "FROM build_result b JOIN author a ON a.id = b.author_id WHERE b.function_id = ? ORDER BY b.id DESC LIMIT ?");
    s.Bind(1, functionId).Bind(2, (qint64)limit);
    while (s.Step())
    {
        BuildResult r;
        r.Id = s.Int(0); r.FunctionId = functionId; r.Compiler = s.Str(1); r.Flags = s.Str(2); r.Score = s.Dbl(3); r.Matched = s.Int(4) != 0;
        r.Diff = s.Str(5); r.CommitHash = s.Str(6); r.SourceFile = s.Str(7); r.Author = s.Str(8); r.Created = s.Str(9);
        out.push_back(r);
    }
    return out;
}

std::vector<Xref> KnowledgeStore::XrefsTo(int cpu, u32 addr, int overlay)
{
    std::vector<Xref> out;
    if (!Db) return out;
    int mid = -1;
    u32 off = addr;
    ModuleOffset(cpu, addr, overlay, mid, off);
    Stmt s(Db, "SELECT ifnull(x.from_module, -1), x.from_offset, x.kind, x.source FROM xref x WHERE "
               "(x.to_module = ? AND x.to_offset = ?) OR (x.to_module IS NULL AND x.to_offset = ?) LIMIT 2000");
    s.Bind(1, (qint64)mid).Bind(2, (qint64)off).Bind(3, (qint64)addr);
    while (s.Step())
    {
        Xref x;
        const Module* m = ModuleById((int)s.Int(0));
        x.From = (m ? m->Load : 0) + (u32)s.Int(1);
        x.To = addr;
        x.Kind = s.Str(2);
        x.Source = s.Str(3);
        out.push_back(x);
    }
    return out;
}

QString KnowledgeStore::DecompilerText(qint64 functionId, QString* tool)
{
    if (!Db) return QString();
    Stmt s(Db, "SELECT text, tool FROM decompiler_output WHERE function_id = ?");
    s.Bind(1, functionId);
    if (!s.Step()) return QString();
    if (tool) *tool = s.Str(1);
    return s.Str(0);
}

QString KnowledgeStore::NameDecompiled(const QString& text, int cpu, const std::function<int(u32)>& overlayAt) const
{
    static const QRegularExpression rx("\\b(?:FUN|sub|func|DAT|data|LAB|PTR_DAT|PTR_FUN)_([0-9a-fA-F]{7,8})\\b|\\b0x(0[12][0-9a-fA-F]{6})\\b");
    QString out;
    int last = 0;
    auto it = rx.globalMatch(text);
    while (it.hasNext())
    {
        auto m = it.next();
        u32 a = (m.captured(1).isEmpty() ? m.captured(2) : m.captured(1)).toUInt(nullptr, 16);
        QString n = NameAt(cpu, a, overlayAt(a), true);
        out += text.mid(last, m.capturedStart() - last);
        out += n.isEmpty() ? m.captured(0) : n;
        last = m.capturedEnd();
    }
    out += text.mid(last);
    return out;
}

// ---------------------------------------------------------------- change log / undo

const char* KnowledgeStore::KeyColumn(const QString& table)
{
    return table == "decompiler_output" ? "function_id" : "id";
}

QJsonObject KnowledgeStore::RowJson(const QString& table, qint64 id)
{
    QString sql = QString("SELECT * FROM \"%1\" WHERE %2 = ?").arg(table, KeyColumn(table));
    QByteArray b = sql.toUtf8();
    Stmt s(Db, b.constData());
    s.Bind(1, id);
    if (!s.Step()) return QJsonObject();
    return ColumnsJson(s);
}

QJsonObject KnowledgeStore::ColumnsJson(Stmt& s)
{
    QJsonObject o;
    int n = sqlite3_column_count(s.S);
    for (int c = 0; c < n; c++)
    {
        QString name = QString::fromUtf8(sqlite3_column_name(s.S, c));
        switch (sqlite3_column_type(s.S, c))
        {
        case SQLITE_INTEGER: o.insert(name, (double)s.Int(c)); break;
        case SQLITE_FLOAT: o.insert(name, s.Dbl(c)); break;
        case SQLITE_NULL: o.insert(name, QJsonValue::Null); break;
        default: o.insert(name, s.Str(c)); break;
        }
    }
    return o;
}

bool KnowledgeStore::LogChange(const QString& table, qint64 id, const QString& action, const QJsonObject& before, const QJsonObject& after)
{
    Stmt s(Db, "INSERT INTO change_log(author_id, agent_session, table_name, row_id, action, before, after) VALUES (?, ?, ?, ?, ?, ?, ?)");
    s.Bind(1, ActiveAuthor).BindOpt(2, ActiveSession).Bind(3, table).Bind(4, id).Bind(5, action);
    if (before.isEmpty()) s.BindNull(6); else s.Bind(6, QString::fromUtf8(QJsonDocument(before).toJson(QJsonDocument::Compact)));
    if (after.isEmpty()) s.BindNull(7); else s.Bind(7, QString::fromUtf8(QJsonDocument(after).toJson(QJsonDocument::Compact)));
    if (!s.Run()) return false;
    ChangeRec r { LastInsertId(), table, id, action, before, after };
    if (GroupDepth > 0)
    {
        if (GroupId < 0) GroupId = r.LogId;
        r.Group = GroupId;
    }
    else r.Group = r.LogId;
    Stacks& h = History[ActiveAuthor];
    h.Undo.push_back(r);
    if (h.Undo.size() > 500) h.Undo.erase(h.Undo.begin());
    h.Redo.clear();
    return true;
}

KnowledgeStore::AuthorScope::AuthorScope(KnowledgeStore& kb, qint64 author, const QString& agentSession)
    : K(kb), PrevAuthor(kb.ActiveAuthor), PrevSession(kb.ActiveSession)
{
    K.ActiveAuthor = author ? author : K.PersonId;
    K.ActiveSession = agentSession;
}

KnowledgeStore::AuthorScope::~AuthorScope()
{
    K.ActiveAuthor = PrevAuthor;
    K.ActiveSession = PrevSession;
}

qint64 KnowledgeStore::AgentAuthor(const QString& name)
{
    if (!Db) return 0;
    QString n = name.trimmed().isEmpty() ? QString("agent") : name.trimmed();
    {
        Stmt s(Db, "SELECT id, kind FROM author WHERE name = ?");
        s.Bind(1, n);
        // a person with the same name keeps theirs: the agent gets a distinct one
        if (s.Step() && s.Str(1) != "agent") n += " (agent)";
    }
    return AuthorId(n, "agent");
}

bool KnowledgeStore::CanUndo(qint64 author) const
{
    auto it = History.find(author ? author : PersonId);
    return it != History.end() && !it->second.Undo.empty();
}

bool KnowledgeStore::CanRedo(qint64 author) const
{
    auto it = History.find(author ? author : PersonId);
    return it != History.end() && !it->second.Redo.empty();
}

std::vector<KnowledgeStore::ChangeEntry> KnowledgeStore::RecentChanges(qint64 afterId, int limit)
{
    std::vector<ChangeEntry> out;
    if (!Db) return out;
    Stmt s(Db, "SELECT c.id, c.row_id, c.table_name, c.action, a.name, a.kind, ifnull(c.agent_session, ''), c.created_at, c.undone "
               "FROM change_log c JOIN author a ON a.id = c.author_id WHERE c.id > ? ORDER BY c.id DESC LIMIT ?");
    s.Bind(1, afterId).Bind(2, (qint64)std::max(1, std::min(limit, 5000)));
    while (s.Step())
        out.push_back({s.Int(0), s.Int(1), s.Str(2), s.Str(3), s.Str(4), s.Str(5), s.Str(6), s.Str(7), s.Int(8) != 0});
    return out;
}

bool KnowledgeStore::ApplyRow(const QString& table, qint64 id, const QJsonObject& row, bool insert, QString& error)
{
    QString key = KeyColumn(table);
    QStringList cols;
    for (auto it = row.begin(); it != row.end(); ++it)
        if (it.key() != key) cols << it.key();
    QString sql;
    if (insert)
    {
        QStringList q;
        for (int i = 0; i < cols.size() + 1; i++) q << "?";
        QStringList quoted;
        for (const QString& c : cols) quoted << "\"" + c + "\"";
        sql = QString("INSERT INTO \"%1\"(%4, %2) VALUES (%3)").arg(table, quoted.join(", "), q.join(", "), key);
    }
    else
    {
        QStringList sets;
        for (const QString& c : cols) sets << "\"" + c + "\" = ?";
        sql = QString("UPDATE \"%1\" SET %2 WHERE %3 = ?").arg(table, sets.join(", "), key);
    }
    QByteArray b = sql.toUtf8();
    Stmt s(Db, b.constData());
    if (!s.Ok()) { error = s.Err(); return false; }
    int idx = 1;
    if (insert) { s.Bind(idx++, id); for (const QString& c : cols) BindJson(s, idx++, row.value(c)); }
    else { for (const QString& c : cols) BindJson(s, idx++, row.value(c)); s.Bind(idx++, id); }
    if (!s.Run()) { error = s.Err(); LastRc = s.Rc; return false; }
    return true;
}

void KnowledgeStore::BindJson(Stmt& s, int idx, const QJsonValue& v)
{
    if (v.isNull() || v.isUndefined()) s.BindNull(idx);
    else if (v.isDouble())
    {
        double d = v.toDouble();
        if (d == (double)(qint64)d) s.Bind(idx, (qint64)d); else s.Bind(idx, d);
    }
    else if (v.isBool()) s.Bind(idx, (qint64)(v.toBool() ? 1 : 0));
    else s.Bind(idx, v.toString());
}

bool KnowledgeStore::InsertRow(const QString& table, const QJsonObject& row, QString& error)
{
    QStringList cols, q;
    for (auto it = row.begin(); it != row.end(); ++it) { cols << "\"" + it.key() + "\""; q << "?"; }
    QByteArray b = QString("INSERT INTO \"%1\"(%2) VALUES (%3)").arg(table, cols.join(", "), q.join(", ")).toUtf8();
    Stmt s(Db, b.constData());
    if (!s.Ok()) { error = s.Err(); return false; }
    int idx = 1;
    for (auto it = row.begin(); it != row.end(); ++it) BindJson(s, idx++, it.value());
    if (!s.Run()) { error = s.Err(); LastRc = s.Rc; return false; }
    return true;
}

bool KnowledgeStore::DeleteRow(const QString& table, qint64 id, QString& error)
{
    QString sql = QString("DELETE FROM \"%1\" WHERE %2 = ?").arg(table, KeyColumn(table));
    QByteArray b = sql.toUtf8();
    Stmt s(Db, b.constData());
    s.Bind(1, id);
    if (!s.Run()) { error = s.Err(); LastRc = s.Rc; return false; }
    return true;
}

std::vector<std::pair<QString, QJsonObject>> KnowledgeStore::CascadedRows(const QString& table, qint64 id)
{
    // the change log doesn't hold these: they were added outside it (build results, imported sessions ...)
    static const char* const functionCascades[] = {"runtime_evidence", "build_result", "decompiler_output"};
    std::vector<std::pair<QString, QJsonObject>> out;
    if (table != "function") return out;
    for (const char* t : functionCascades)
    {
        QByteArray q = QString("SELECT * FROM \"%1\" WHERE function_id = ?").arg(t).toUtf8();
        Stmt s(Db, q.constData());
        s.Bind(1, id);
        while (s.Step()) out.emplace_back(t, ColumnsJson(s));
    }
    return out;
}

bool KnowledgeStore::Undo(QString& error, qint64 author) { return Replay(true, error, author); }
bool KnowledgeStore::Redo(QString& error, qint64 author) { return Replay(false, error, author); }

bool KnowledgeStore::Replay(bool undo, QString& error, qint64 author)
{
    Stacks& h = History[author ? author : PersonId];
    std::vector<ChangeRec>& from = undo ? h.Undo : h.Redo;
    std::vector<ChangeRec>& to = undo ? h.Redo : h.Undo;
    if (from.empty()) { error = undo ? "nothing to undo" : "nothing to redo"; return false; }
    // the group's changes, newest first: a deleted row comes back before the rows that referenced it
    std::vector<ChangeRec> group;
    for (auto it = from.rbegin(); it != from.rend() && it->Group == from.back().Group; ++it) group.push_back(*it);
    QString e;
    Exec("BEGIN", e);
    for (ChangeRec& r : group)
    {
        bool ok = true;
        LastRc = SQLITE_OK;
        if (r.Action == (undo ? "insert" : "delete"))
        {
            if (undo) r.Cascaded = CascadedRows(r.Table, r.Row);
            ok = DeleteRow(r.Table, r.Row, error);
        }
        else
        {
            ok = ApplyRow(r.Table, r.Row, undo ? r.Before : r.After, r.Action != "update", error);
            if (!undo)
                for (const auto& [table, row] : r.Cascaded)
                    if (ok) ok = InsertRow(table, row, error);
        }
        if (!ok)
        {
            Exec("ROLLBACK", e);
            if ((LastRc & 0xFF) == SQLITE_CONSTRAINT)
            {
                // other changes depend on the row now (an annotation on an inserted function, say): this one
                // can never apply, so it leaves the list instead of blocking the changes before it
                from.resize(from.size() - group.size());
                error = QString("cannot %1 this change, other changes depend on it (%2). It was taken off the %1 list")
                            .arg(undo ? "undo" : "redo", error);
            }
            Reload();
            return false;
        }
        Stmt s(Db, "UPDATE change_log SET undone = ? WHERE id = ?");
        s.Bind(1, (qint64)(undo ? 1 : 0)).Bind(2, r.LogId);
        s.Run();
    }
    Exec("COMMIT", e);
    from.resize(from.size() - group.size());
    to.insert(to.end(), group.begin(), group.end());
    Reload();
    return true;
}

// ---------------------------------------------------------------- writes

qint64 KnowledgeStore::SystemAuthor()
{
    return AuthorId("workbench", "system");
}

int KnowledgeStore::DefineAutoFunctions(const std::vector<AutoFunction>& fns, QString& error)
{
    if (!Db) { error = "no knowledge store open"; return 0; }
    if (fns.empty()) return 0;
    qint64 aid = SystemAuthor();
    if (aid <= 0) { error = "cannot create the system author"; return 0; }
    struct Span { int mid; u32 off, end; };
    std::vector<Span> added;
    auto covered = [&](int cpu, u32 addr, int overlay, int mid, u32 off)
    {
        if (FunctionAt(cpu, addr, overlay) || FunctionStarting(cpu, addr, overlay)) return true;
        for (const Span& s : added)
            if (s.mid == mid && off >= s.off && off < s.end) return true;
        return false;
    };
    QString e;
    if (!Exec("BEGIN", e)) { error = e; return 0; }
    int n = 0;
    for (const AutoFunction& a : fns)
    {
        u32 addr = a.Addr & ~1u;
        if (!addr) continue;
        if (!a.Thumb && (addr & 3)) continue;
        u32 size = a.Size ? a.Size : (a.Thumb ? 2u : 4u);
        int mid; u32 off;
        if (!ModuleOffset(a.Cpu, addr, a.Overlay, mid, off)) continue;
        if (covered(a.Cpu, addr, a.Overlay, mid, off)) continue;
        QString name = QString("sub_%1").arg(addr, 8, 16, QChar('0'));
        Stmt s(Db, "INSERT INTO function(module_id, offset, size, mode, name, author_id) VALUES (?, ?, ?, ?, ?, ?)");
        s.Bind(1, (qint64)mid).Bind(2, (qint64)off).Bind(3, (qint64)size).Bind(4, a.Thumb ? QString("thumb") : QString("arm"));
        s.Bind(5, name).Bind(6, aid);
        if (!s.Run()) continue;
        added.push_back({mid, off, off + size});
        n++;
    }
    if (!Exec("COMMIT", e)) { error = e; QString e2; Exec("ROLLBACK", e2); return 0; }
    if (n) Reload();
    return n;
}

int KnowledgeStore::ReviseAutoSizes(const std::vector<std::pair<qint64, u32>>& sizes, QString& error)
{
    if (!Db) { error = "no knowledge store open"; return 0; }
    if (sizes.empty()) return 0;
    qint64 aid = SystemAuthor();
    if (aid <= 0) { error = "cannot create the system author"; return 0; }
    QString e;
    if (!Exec("BEGIN", e)) { error = e; return 0; }
    int n = 0;
    for (const auto& s : sizes)
    {
        Stmt st(Db, "UPDATE function SET size = ? WHERE id = ? AND author_id = ?");
        st.Bind(1, (qint64)s.second).Bind(2, s.first).Bind(3, aid);
        if (!st.Run()) { error = st.Err(); Exec("ROLLBACK", e); return 0; }
        n += sqlite3_changes(Db);
    }
    if (!Exec("COMMIT", e)) { error = e; Exec("ROLLBACK", e); return 0; }
    if (n) Reload();
    return n;
}

bool KnowledgeStore::UpsertFunction(int cpu, u32 addr, u32 size, const QString& mode, const QString& name, int overlay, QString& error, qint64* idOut)
{
    if (!Db) { error = "no knowledge store open"; return false; }
    int mid; u32 off;
    if (!ModuleOffset(cpu, addr & ~1u, overlay, mid, off)) { error = "address is outside every module"; return false; }
    QString m = mode;
    if (m != "arm" && m != "thumb") { error = "mode must be arm or thumb"; return false; }
    if (m == "arm" && (addr & 3)) { error = "ARM functions must be 4-byte aligned"; return false; }

    qint64 existing = 0;
    {
        Stmt s(Db, "SELECT id FROM function WHERE module_id = ? AND offset = ?");
        s.Bind(1, (qint64)mid).Bind(2, (qint64)off);
        if (s.Step()) existing = s.Int(0);
    }
    if (existing)
    {
        QJsonObject before = RowJson("function", existing);
        Stmt s(Db, "UPDATE function SET size = ?, mode = ?, name = ?, author_id = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') WHERE id = ?");
        s.Bind(1, (qint64)size).Bind(2, m).BindOpt(3, name).Bind(4, ActiveAuthor).Bind(5, existing);
        if (!s.Run()) { error = s.Err(); return false; }
        LogChange("function", existing, "update", before, RowJson("function", existing));
        if (idOut) *idOut = existing;
    }
    else
    {
        Stmt s(Db, "INSERT INTO function(module_id, offset, size, mode, name, author_id) VALUES (?, ?, ?, ?, ?, ?)");
        s.Bind(1, (qint64)mid).Bind(2, (qint64)off).Bind(3, (qint64)size).Bind(4, m).BindOpt(5, name).Bind(6, ActiveAuthor);
        if (!s.Run()) { error = s.Err(); return false; }
        qint64 id = LastInsertId();
        LogChange("function", id, "insert", {}, RowJson("function", id));
        if (idOut) *idOut = id;
    }
    Reload();
    return true;
}

bool KnowledgeStore::UpdateFunction(qint64 id, const QJsonObject& fields, QString& error)
{
    QJsonObject before = RowJson("function", id);
    if (before.isEmpty()) { error = "no such function"; return false; }
    static const QStringList allowed = {"size", "mode", "name", "signature", "subsystem", "source_file"};
    QJsonObject row;
    for (auto it = fields.begin(); it != fields.end(); ++it)
    {
        if (!allowed.contains(it.key())) { error = "field '" + it.key() + "' cannot be set"; return false; }
        row.insert(it.key(), it.value());
    }
    row.insert("author_id", (double)ActiveAuthor);
    if (!ApplyRow("function", id, row, false, error)) return false;
    Stmt s(Db, "UPDATE function SET updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') WHERE id = ?");
    s.Bind(1, id);
    s.Run();
    LogChange("function", id, "update", before, RowJson("function", id));
    Reload();
    return true;
}

bool KnowledgeStore::DetachRows(const QString& table, const QString& column, qint64 id, bool remove, QString& error)
{
    std::vector<qint64> rows;
    {
        QByteArray q = QString("SELECT %1 FROM \"%2\" WHERE \"%3\" = ?").arg(KeyColumn(table), table, column).toUtf8();
        Stmt s(Db, q.constData());
        s.Bind(1, id);
        while (s.Step()) rows.push_back(s.Int(0));
    }
    for (qint64 r : rows)
    {
        QJsonObject before = RowJson(table, r);
        if (remove)
        {
            if (!DeleteRow(table, r, error)) return false;
            LogChange(table, r, "delete", before, {});
        }
        else
        {
            QByteArray q = QString("UPDATE \"%1\" SET \"%2\" = NULL WHERE %3 = ?").arg(table, column, KeyColumn(table)).toUtf8();
            Stmt s(Db, q.constData());
            s.Bind(1, r);
            if (!s.Run()) { error = s.Err(); return false; }
            LogChange(table, r, "update", before, RowJson(table, r));
        }
    }
    return true;
}

bool KnowledgeStore::DeleteFunction(qint64 id, QString& error)
{
    QJsonObject before = RowJson("function", id);
    if (before.isEmpty()) { error = "no such function"; return false; }
    // its evidence, builds, decompiler text and annotations go with it; other references are
    // cleared. All of it is one change, so undo brings everything back
    BeginGroup();
    QString e;
    Exec("BEGIN", e);
    bool ok = DetachRows("runtime_evidence", "function_id", id, true, error) &&
              DetachRows("build_result", "function_id", id, true, error) &&
              DetachRows("decompiler_output", "function_id", id, true, error) &&
              DetachRows("annotation", "function_id", id, true, error) &&
              DetachRows("data_object", "owner_function_id", id, false, error) &&
              DetachRows("asset_link", "loader_function_id", id, false, error) &&
              DeleteRow("function", id, error);
    if (ok) LogChange("function", id, "delete", before, {});
    Exec(ok ? "COMMIT" : "ROLLBACK", e);
    EndGroup();
    Reload();
    return ok;
}

bool KnowledgeStore::Rename(int cpu, u32 addr, const QString& name, int overlay, QString& error)
{
    if (const Function* f = FunctionStarting(cpu, addr & ~1u, overlay))
    {
        QJsonObject o;
        o.insert("name", name.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(name));
        return UpdateFunction(f->Id, o, error);
    }
    if (const DataObject* d = DataObjectAt(cpu, addr, overlay))
    {
        if (d->Addr == addr)
        {
            QJsonObject before = RowJson("data_object", d->Id);
            QJsonObject row;
            row.insert("name", name);
            row.insert("author_id", (double)ActiveAuthor);
            if (!ApplyRow("data_object", d->Id, row, false, error)) return false;
            LogChange("data_object", d->Id, "update", before, RowJson("data_object", d->Id));
            Reload();
            return true;
        }
    }
    return SetLabel(cpu, addr, name, overlay, error);
}

bool KnowledgeStore::SetLabel(int cpu, u32 addr, const QString& name, int overlay, QString& error)
{
    if (!Db) { error = "no knowledge store open"; return false; }
    int mid; u32 off;
    if (!ModuleOffset(cpu, addr, overlay, mid, off)) { error = "address is outside every module"; return false; }
    qint64 existing = 0;
    {
        Stmt s(Db, "SELECT id FROM label WHERE module_id = ? AND offset = ?");
        s.Bind(1, (qint64)mid).Bind(2, (qint64)off);
        if (s.Step()) existing = s.Int(0);
    }
    if (name.isEmpty())
    {
        if (!existing) return true;
        return DeleteLabel(existing, error);
    }
    if (existing)
    {
        QJsonObject before = RowJson("label", existing);
        Stmt s(Db, "UPDATE label SET name = ?, author_id = ? WHERE id = ?");
        s.Bind(1, name).Bind(2, ActiveAuthor).Bind(3, existing);
        if (!s.Run()) { error = s.Err(); return false; }
        LogChange("label", existing, "update", before, RowJson("label", existing));
    }
    else
    {
        Stmt s(Db, "INSERT INTO label(module_id, offset, name, author_id) VALUES (?, ?, ?, ?)");
        s.Bind(1, (qint64)mid).Bind(2, (qint64)off).Bind(3, name).Bind(4, ActiveAuthor);
        if (!s.Run()) { error = s.Err(); return false; }
        qint64 id = LastInsertId();
        LogChange("label", id, "insert", {}, RowJson("label", id));
    }
    Reload();
    return true;
}

bool KnowledgeStore::DeleteLabel(qint64 id, QString& error)
{
    QJsonObject before = RowJson("label", id);
    if (before.isEmpty()) { error = "no such label"; return false; }
    if (!DeleteRow("label", id, error)) return false;
    LogChange("label", id, "delete", before, {});
    Reload();
    return true;
}

static QString RegionForAddr(u32 addr)
{
    switch (addr >> 24)
    {
    case 0x01: case 0x00: return "itcm";
    case 0x02: return "main_ram";
    case 0x03: return "wram";
    case 0x04: return "io";
    case 0x05: return "palette";
    case 0x06: return "vram";
    case 0x07: return "oam";
    default: return "other";
    }
}

bool KnowledgeStore::DefineData(int cpu, u32 addr, u32 size, const QString& typeName, const QString& name, int overlay, QString& error)
{
    if (!Db) { error = "no knowledge store open"; return false; }
    int mid = -1; u32 off = addr;
    bool inModule = ModuleOffset(cpu, addr, overlay, mid, off);
    if (!typeName.isEmpty() && !TypeByName(typeName))
    {
        static const QStringList basic = {"u8", "s8", "u16", "s16", "u32", "s32", "u64", "s64", "fx16", "fx32", "fx64", "char", "int", "short", "float", "void*", "BOOL"};
        if (!basic.contains(typeName) && !typeName.endsWith('*') && !typeName.contains('[')) { error = "unknown type '" + typeName + "'"; return false; }
    }
    qint64 existing = 0;
    {
        Stmt s(Db, inModule ? "SELECT id FROM data_object WHERE module_id = ? AND offset = ?" : "SELECT id FROM data_object WHERE module_id IS NULL AND region = ? AND offset = ?");
        if (inModule) s.Bind(1, (qint64)mid); else s.Bind(1, RegionForAddr(addr));
        s.Bind(2, (qint64)off);
        if (s.Step()) existing = s.Int(0);
    }
    if (existing)
    {
        QJsonObject before = RowJson("data_object", existing);
        QJsonObject row;
        row.insert("size", (double)size);
        row.insert("type_name", typeName.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(typeName));
        if (!name.isEmpty()) row.insert("name", name);
        row.insert("author_id", (double)ActiveAuthor);
        if (!ApplyRow("data_object", existing, row, false, error)) return false;
        LogChange("data_object", existing, "update", before, RowJson("data_object", existing));
    }
    else
    {
        Stmt s(Db, "INSERT INTO data_object(module_id, region, offset, name, type_name, size, author_id) VALUES (?, ?, ?, ?, ?, ?, ?)");
        if (inModule) { s.Bind(1, (qint64)mid); s.BindNull(2); }
        else { s.BindNull(1); s.Bind(2, RegionForAddr(addr)); }
        s.Bind(3, (qint64)off).BindOpt(4, name).BindOpt(5, typeName).Bind(6, (qint64)size).Bind(7, ActiveAuthor);
        if (!s.Run()) { error = s.Err(); return false; }
        qint64 id = LastInsertId();
        LogChange("data_object", id, "insert", {}, RowJson("data_object", id));
    }
    Reload();
    return true;
}

bool KnowledgeStore::DeleteDataObject(qint64 id, QString& error)
{
    QJsonObject before = RowJson("data_object", id);
    if (before.isEmpty()) { error = "no such data object"; return false; }
    BeginGroup();
    QString e;
    Exec("BEGIN", e);
    bool ok = DetachRows("annotation", "data_object_id", id, true, error) && DeleteRow("data_object", id, error);
    if (ok) LogChange("data_object", id, "delete", before, {});
    Exec(ok ? "COMMIT" : "ROLLBACK", e);
    EndGroup();
    Reload();
    return ok;
}

bool KnowledgeStore::DefineType(const QString& definition, QString& error)
{
    if (!Db) { error = "no knowledge store open"; return false; }
    Type t;
    if (!ParseTypeDefinition(definition, TypeMap, t, error)) return false;
    qint64 existing = 0;
    {
        Stmt s(Db, "SELECT id FROM type WHERE name = ?");
        s.Bind(1, t.Name);
        if (s.Step()) existing = s.Int(0);
    }
    if (existing)
    {
        QJsonObject before = RowJson("type", existing);
        Stmt s(Db, "UPDATE type SET kind = ?, definition = ?, size = ?, author_id = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') WHERE id = ?");
        s.Bind(1, t.Kind).Bind(2, definition.trimmed()).Bind(3, (qint64)t.Size).Bind(4, ActiveAuthor).Bind(5, existing);
        if (!s.Run()) { error = s.Err(); return false; }
        LogChange("type", existing, "update", before, RowJson("type", existing));
    }
    else
    {
        Stmt s(Db, "INSERT INTO type(name, kind, definition, size, author_id) VALUES (?, ?, ?, ?, ?)");
        s.Bind(1, t.Name).Bind(2, t.Kind).Bind(3, definition.trimmed()).Bind(4, (qint64)t.Size).Bind(5, ActiveAuthor);
        if (!s.Run()) { error = s.Err(); return false; }
        qint64 id = LastInsertId();
        LogChange("type", id, "insert", {}, RowJson("type", id));
    }
    Reload();
    return true;
}

bool KnowledgeStore::DeleteType(const QString& name, QString& error)
{
    const Type* t = TypeByName(name);
    if (!t) { error = "no such type"; return false; }
    qint64 id = t->Id;
    QJsonObject before = RowJson("type", id);
    if (!DeleteRow("type", id, error)) return false;
    LogChange("type", id, "delete", before, {});
    Reload();
    return true;
}

bool KnowledgeStore::ResolveAnnotationTarget(Annotation& a, int cpu, u32 addr, bool haveAddr, u32 end, int ov, bool wholeFunction, QString& error)
{
    const QString& k = a.TargetKind;
    if (k == "address" || k == "range")
    {
        if (!haveAddr) { error = "bad address"; return false; }
        int mid; u32 off;
        if (ModuleOffset(cpu, addr, ov, mid, off)) { a.ModuleId = mid; a.Offset = off; }
        else { a.ModuleId = -1; a.Offset = addr; }
        if (k == "range")
        {
            if (end <= addr) { error = "the end must be after the start"; return false; }
            a.EndOffset = a.Offset + (end - addr);
        }
    }
    else if (k == "function")
    {
        const Function* f = haveAddr ? FunctionAt(cpu, addr, ov) : nullptr;
        if (!f) { error = "no function at this address; define one first"; return false; }
        a.FunctionId = f->Id;
    }
    else if (k == "data_object")
    {
        const DataObject* d = haveAddr ? DataObjectAt(cpu, addr, ov) : nullptr;
        if (!d) { error = "no data object at this address; define one first"; return false; }
        a.DataObjectId = d->Id;
    }
    else if (k == "struct_field")
    {
        if (a.TypeName.isEmpty() || a.FieldName.isEmpty()) { error = "choose a type and a field"; return false; }
    }
    else if (k == "type")
    {
        if (a.TypeName.isEmpty()) { error = "choose a type"; return false; }
    }
    else if (k == "register")
    {
        if (a.Register.isEmpty()) { error = "choose a register"; return false; }
        const Function* f = haveAddr ? FunctionAt(cpu, addr, ov) : nullptr;
        if (f) a.FunctionId = f->Id;
        if (!wholeFunction || !f)
        {
            if (!haveAddr) { error = "bad address"; return false; }
            int mid; u32 off;
            if (!ModuleOffset(cpu, addr, ov, mid, off)) { error = "address is outside every module"; return false; }
            a.ModuleId = mid;
            a.Offset = off;
            a.EndOffset = end > addr ? off + (end - addr) : (f ? (qint64)(f->Addr + f->Size - addr) + off : off + 4);
        }
    }
    else if (k == "savestate")
    {
        if (a.Savestate.isEmpty()) { error = "enter the savestate path"; return false; }
    }
    else if (k != "overlay") { error = "bad target kind"; return false; }
    return true;
}

bool KnowledgeStore::SaveAnnotation(Annotation& a, QString& error)
{
    if (!Db) { error = "no knowledge store open"; return false; }
    if (a.Comment.trimmed().isEmpty()) { error = "annotation comment is empty"; return false; }
    static const QStringList kinds = {"address", "range", "function", "data_object", "struct_field", "type", "register", "overlay", "savestate"};
    if (!kinds.contains(a.TargetKind)) { error = "bad target kind"; return false; }
    QJsonObject row;
    auto optInt = [](qint64 v, qint64 none) { return v == none ? QJsonValue(QJsonValue::Null) : QJsonValue((double)v); };
    auto optStr = [](const QString& v) { return v.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(v); };
    row.insert("target_kind", a.TargetKind);
    row.insert("module_id", optInt(a.ModuleId, -1));
    row.insert("offset", optInt(a.Offset, -1));
    row.insert("end_offset", optInt(a.EndOffset, -1));
    row.insert("function_id", optInt(a.FunctionId, 0));
    row.insert("data_object_id", optInt(a.DataObjectId, 0));
    row.insert("type_name", optStr(a.TypeName));
    row.insert("field_name", optStr(a.FieldName));
    row.insert("register", optStr(a.Register));
    row.insert("overlay_id", optInt(a.OverlayId, -1));
    row.insert("savestate", optStr(a.Savestate));
    row.insert("comment", a.Comment);
    row.insert("tags", a.Tags);
    row.insert("author_id", (double)ActiveAuthor);
    if (a.Id)
    {
        QJsonObject before = RowJson("annotation", a.Id);
        if (before.isEmpty()) { error = "no such annotation"; return false; }
        row.insert("created_at", before.value("created_at"));
        if (!ApplyRow("annotation", a.Id, row, false, error)) return false;
        Stmt s(Db, "UPDATE annotation SET updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') WHERE id = ?");
        s.Bind(1, a.Id);
        s.Run();
        LogChange("annotation", a.Id, "update", before, RowJson("annotation", a.Id));
    }
    else
    {
        QStringList cols, q;
        for (auto it = row.begin(); it != row.end(); ++it) { cols << "\"" + it.key() + "\""; q << "?"; }
        QString sql = QString("INSERT INTO annotation(%1) VALUES (%2)").arg(cols.join(", "), q.join(", "));
        QByteArray b = sql.toUtf8();
        Stmt s(Db, b.constData());
        int idx = 1;
        for (auto it = row.begin(); it != row.end(); ++it, idx++)
        {
            const QJsonValue& v = it.value();
            if (v.isNull()) s.BindNull(idx);
            else if (v.isDouble()) s.Bind(idx, (qint64)v.toDouble());
            else s.Bind(idx, v.toString());
        }
        if (!s.Run()) { error = s.Err(); return false; }
        a.Id = LastInsertId();
        LogChange("annotation", a.Id, "insert", {}, RowJson("annotation", a.Id));
    }
    Reload();
    return true;
}

bool KnowledgeStore::DeleteAnnotation(qint64 id, QString& error)
{
    QJsonObject before = RowJson("annotation", id);
    if (before.isEmpty()) { error = "no such annotation"; return false; }
    if (!DeleteRow("annotation", id, error)) return false;
    LogChange("annotation", id, "delete", before, {});
    Reload();
    return true;
}

bool KnowledgeStore::AddBookmark(int cpu, u32 addr, const QString& name, QString& error, qint64* idOut)
{
    if (!Db) { error = "no knowledge store open"; return false; }
    int mid = -1; u32 off;
    bool inModule = ModuleOffset(cpu, addr, -1, mid, off);
    Stmt s(Db, "INSERT INTO bookmark(module_id, address, cpu, name, author_id) VALUES (?, ?, ?, ?, ?)");
    if (inModule) s.Bind(1, (qint64)mid); else s.BindNull(1);
    s.Bind(2, (qint64)addr).Bind(3, QString(cpu ? "arm7" : "arm9")).Bind(4, name).Bind(5, ActiveAuthor);
    if (!s.Run()) { error = s.Err(); return false; }
    qint64 id = LastInsertId();
    LogChange("bookmark", id, "insert", {}, RowJson("bookmark", id));
    if (idOut) *idOut = id;
    Reload();
    return true;
}

bool KnowledgeStore::DeleteBookmark(qint64 id, QString& error)
{
    QJsonObject before = RowJson("bookmark", id);
    if (before.isEmpty()) { error = "no such bookmark"; return false; }
    if (!DeleteRow("bookmark", id, error)) return false;
    LogChange("bookmark", id, "delete", before, {});
    Reload();
    return true;
}

bool KnowledgeStore::RecordBuildResult(const BuildResult& r, QString& error)
{
    if (!Db) { error = "no knowledge store open"; return false; }
    Stmt s(Db, "INSERT INTO build_result(function_id, compiler, flags, score, matched, diff, commit_hash, source_file, author_id) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    s.Bind(1, r.FunctionId).Bind(2, r.Compiler).Bind(3, r.Flags).Bind(4, r.Score).Bind(5, (qint64)(r.Matched ? 1 : 0)).Bind(6, r.Diff)
     .Bind(7, r.CommitHash).BindOpt(8, r.SourceFile).Bind(9, ActiveAuthor);
    if (!s.Run()) { error = s.Err(); return false; }
    Reload();
    return true;
}

bool KnowledgeStore::AddXref(int cpu, u32 from, u32 to, const QString& kind, const QString& source, QString& error)
{
    if (!Db) { error = "no knowledge store open"; return false; }
    int fm = -1, tm = -1;
    u32 fo = from, to2 = to;
    bool hasFrom = ModuleOffset(cpu, from, -1, fm, fo);
    bool hasTo = ModuleOffset(cpu, to, -1, tm, to2);
    Stmt s(Db, "INSERT OR IGNORE INTO xref(from_module, from_offset, to_module, to_offset, kind, source) VALUES (?, ?, ?, ?, ?, ?)");
    if (hasFrom) s.Bind(1, (qint64)fm); else s.BindNull(1);
    s.Bind(2, (qint64)(hasFrom ? fo : from));
    if (hasTo) s.Bind(3, (qint64)tm); else s.BindNull(3);
    s.Bind(4, (qint64)(hasTo ? to2 : to)).Bind(5, kind).Bind(6, source);
    if (!s.Run()) { error = s.Err(); return false; }
    return true;
}

bool KnowledgeStore::SetDecompiled(qint64 functionId, const QString& tool, const QString& text, QString& error)
{
    if (!Db) { error = "no knowledge store open"; return false; }
    Stmt s(Db, "INSERT OR REPLACE INTO decompiler_output(function_id, tool, text) VALUES (?, ?, ?)");
    s.Bind(1, functionId).Bind(2, tool).Bind(3, text);
    if (!s.Run()) { error = s.Err(); return false; }
    return true;
}

static QString FormatOf(const QString& path)
{
    int dot = path.lastIndexOf('.');
    return dot > path.lastIndexOf('/') && dot >= 0 ? path.mid(dot + 1).toLower() : QString();
}

bool KnowledgeStore::SeedArchiveAssets(u32 archiveFileId, const QString& archivePath, const std::vector<ArchiveAssetRow>& rows, QString& error)
{
    if (!Db) { error = "no knowledge store open"; return false; }
    {
        Stmt s(Db, "SELECT count(*) FROM asset WHERE file_id = ? AND member <> ''");
        s.Bind(1, (qint64)archiveFileId);
        if (s.Step() && s.Int(0) >= (qint64)rows.size()) return true;
    }
    if (!Exec("BEGIN", error)) return false;
    for (const ArchiveAssetRow& r : rows)
    {
        Stmt s(Db, "INSERT OR IGNORE INTO asset(file_id, member, path, format, rom_offset, size, packed_size) VALUES (?, ?, ?, ?, ?, ?, ?)");
        s.Bind(1, (qint64)archiveFileId).Bind(2, r.Member).Bind(3, archivePath + "/" + r.Member).Bind(4, FormatOf(r.Member))
         .Bind(5, (qint64)r.RomOffset).Bind(6, (qint64)r.Size).Bind(7, (qint64)r.PackedSize);
        if (!s.Run()) { error = s.Err(); QString e; Exec("ROLLBACK", e); return false; }
    }
    return Exec("COMMIT", error);
}

bool KnowledgeStore::ImportSession(const QString& file, const QString& sha1, u32 frameStart, u32 frameEnd, u32 flags,
                                   const std::vector<EvidenceRow>& evidence, const std::vector<AssetLinkRow>& assets, QString& error)
{
    if (!Db) { error = "no knowledge store open"; return false; }
    if (!Exec("BEGIN", error)) return false;
    qint64 sid;
    {
        Stmt s(Db, "INSERT INTO session(file, rom_sha1, frame_start, frame_end, flags, tool_version, author_id) VALUES (?, ?, ?, ?, ?, ?, ?)");
        s.Bind(1, file).Bind(2, sha1).Bind(3, (qint64)frameStart).Bind(4, (qint64)frameEnd).Bind(5, (qint64)flags)
         .Bind(6, QString("melonDS-workbench")).Bind(7, PersonId);
        if (!s.Run()) { error = s.Err(); QString e; Exec("ROLLBACK", e); return false; }
        sid = LastInsertId();
    }
    for (const EvidenceRow& e : evidence)
    {
        Stmt s(Db, "INSERT OR REPLACE INTO runtime_evidence(function_id, session_id, exec_count, instr_count, bytes_executed, callers, callees, data_touched) "
                   "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
        s.Bind(1, e.FunctionId).Bind(2, sid).Bind(3, (qint64)e.ExecCount).Bind(4, (qint64)e.InstrCount).Bind(5, (qint64)e.BytesExecuted)
         .Bind(6, e.Callers).Bind(7, e.Callees).Bind(8, e.DataTouched);
        if (!s.Run()) { error = s.Err(); QString er; Exec("ROLLBACK", er); return false; }
    }
    for (const AssetLinkRow& a : assets)
    {
        qint64 assetId = 0;
        {
            Stmt s(Db, "SELECT id FROM asset WHERE file_id = ? AND member = ?");
            s.Bind(1, (qint64)a.FileId).Bind(2, a.Member);
            if (s.Step()) assetId = s.Int(0);
        }
        if (!assetId)
        {
            Stmt s(Db, "INSERT INTO asset(file_id, member, path, format, rom_offset, size, packed_size) VALUES (?, ?, ?, ?, ?, ?, ?)");
            s.Bind(1, (qint64)a.FileId).Bind(2, a.Member).Bind(3, a.Path).Bind(4, FormatOf(a.Member.isEmpty() ? a.Path : a.Member))
             .Bind(5, (qint64)a.RomOffset).Bind(6, (qint64)a.Size);
            if (a.Member.isEmpty()) s.BindNull(7); else s.Bind(7, (qint64)a.PackedSize);
            s.Run();
            assetId = LastInsertId();
        }
        const Function* f = FunctionAt(0, a.LoaderPC & ~1u, -1);
        Stmt s(Db, "INSERT INTO asset_link(asset_id, kind, ram_start, ram_end, loader_function_id, loader_pc, session_id, frame) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
        s.Bind(1, assetId).Bind(2, a.Kind).Bind(3, (qint64)a.RamStart).Bind(4, (qint64)a.RamEnd);
        if (f) s.Bind(5, f->Id); else s.BindNull(5);
        s.Bind(6, (qint64)a.LoaderPC).Bind(7, sid).Bind(8, (qint64)a.Frame);
        s.Run();
    }
    if (!Exec("COMMIT", error)) return false;
    return true;
}

// ---------------------------------------------------------------- C type layout parser

namespace
{
struct Tok { QString t; };

struct TypeLexer
{
    QString S;
    int P = 0;
    QString Next()
    {
        for (;;)
        {
            while (P < S.size() && S[P].isSpace()) P++;
            if (P + 1 < S.size() && S[P] == '/' && S[P+1] == '/') { while (P < S.size() && S[P] != '\n') P++; continue; }
            if (P + 1 < S.size() && S[P] == '/' && S[P+1] == '*') { int e = S.indexOf("*/", P + 2); P = e < 0 ? S.size() : e + 2; continue; }
            break;
        }
        if (P >= S.size()) return QString();
        QChar c = S[P];
        if (c.isLetter() || c == '_')
        {
            int s = P;
            while (P < S.size() && (S[P].isLetterOrNumber() || S[P] == '_')) P++;
            return S.mid(s, P - s);
        }
        if (c.isDigit())
        {
            int s = P;
            while (P < S.size() && (S[P].isLetterOrNumber())) P++;
            return S.mid(s, P - s);
        }
        P++;
        return QString(c);
    }
    QString Peek() { int save = P; QString t = Next(); P = save; return t; }
};

bool BasicType(const QString& n, u32& size)
{
    static const std::map<QString, u32> sizes = {
        {"u8", 1}, {"s8", 1}, {"char", 1}, {"bool", 1}, {"vu8", 1}, {"vs8", 1},
        {"u16", 2}, {"s16", 2}, {"short", 2}, {"fx16", 2}, {"vu16", 2}, {"vs16", 2}, {"wchar_t", 2},
        {"u32", 4}, {"s32", 4}, {"int", 4}, {"long", 4}, {"fx32", 4}, {"BOOL", 4}, {"float", 4}, {"f32", 4}, {"vu32", 4}, {"vs32", 4}, {"size_t", 4},
        {"u64", 8}, {"s64", 8}, {"fx64", 8}, {"double", 8}, {"f64", 8},
        {"void", 0},
    };
    auto it = sizes.find(n);
    if (it == sizes.end()) return false;
    size = it->second;
    return true;
}
}

bool ParseTypeDefinition(const QString& def, const std::map<QString, Type>& known, Type& out, QString& error)
{
    TypeLexer lx;
    lx.S = def;
    out = Type();
    QString t = lx.Next();
    bool isTypedef = false;
    if (t == "typedef") { isTypedef = true; t = lx.Next(); }

    auto resolveSize = [&](const QString& name, u32& size, u32& align) -> bool
    {
        if (BasicType(name, size)) { align = std::max<u32>(1, std::min<u32>(size, 4)); if (size == 8) align = 4; return true; }
        auto it = known.find(name);
        if (it != known.end() && it->second.Size >= 0)
        {
            size = (u32)it->second.Size;
            align = 1;
            for (const TypeField& f : it->second.Fields) align = std::max(align, std::min<u32>(f.Size, 4));
            if (it->second.Kind == "enum") align = 4;
            return true;
        }
        return false;
    };

    if (t == "enum")
    {
        QString name = lx.Next();
        if (name == "{") name.clear();
        else if (lx.Next() != "{") { error = "expected '{' after enum name"; return false; }
        int depth = 1;
        while (depth > 0)
        {
            QString x = lx.Next();
            if (x.isEmpty()) { error = "unterminated enum"; return false; }
            if (x == "{") depth++;
            if (x == "}") depth--;
        }
        if (isTypedef) { QString alias = lx.Next(); if (!alias.isEmpty() && alias != ";") name = alias; }
        if (name.isEmpty()) { error = "enum needs a name"; return false; }
        out.Name = name;
        out.Kind = "enum";
        out.Size = 4;
        return true;
    }

    if (t == "struct" || t == "union")
    {
        bool isUnion = t == "union";
        QString name = lx.Next();
        if (name == "{") name.clear();
        else
        {
            QString b = lx.Next();
            if (b != "{")
            {
                // typedef struct Foo Bar;
                if (isTypedef)
                {
                    u32 size, align;
                    QString alias = b;
                    if (!resolveSize(name, size, align)) { error = "unknown type " + name; return false; }
                    out.Name = alias;
                    out.Kind = "typedef";
                    out.Size = (int)size;
                    auto it = known.find(name);
                    if (it != known.end()) out.Fields = it->second.Fields;
                    return true;
                }
                error = "expected '{'";
                return false;
            }
        }
        u32 offset = 0, maxAlign = 1, maxSize = 0;
        int bitUnitOff = -1, bitPos = 0, bitUnitSize = 0;
        for (;;)
        {
            QString ft = lx.Next();
            if (ft.isEmpty()) { error = "unterminated struct"; return false; }
            if (ft == "}") break;
            if (ft == ";") continue;
            while (ft == "const" || ft == "volatile" || ft == "struct" || ft == "union" || ft == "enum" || ft == "unsigned" || ft == "signed")
            {
                QString n = lx.Peek();
                if (ft == "unsigned" || ft == "signed")
                {
                    if (n == "char" || n == "short" || n == "int" || n == "long") { ft = lx.Next(); continue; }
                    ft = "int";
                    break;
                }
                ft = lx.Next();
            }
            // declarators: name, *name, name[N], name : bits ; separated by commas
            for (;;)
            {
                int ptr = 0;
                QString n = lx.Next();
                while (n == "*") { ptr++; n = lx.Next(); }
                if (n.isEmpty()) { error = "unexpected end"; return false; }
                u32 count = 1;
                int bits = -1;
                QString x = lx.Next();
                while (x == "[")
                {
                    QString num = lx.Next();
                    bool ok;
                    u32 v = num.startsWith("0x") ? num.mid(2).toUInt(&ok, 16) : num.toUInt(&ok);
                    if (!ok) { error = "array size must be a number"; return false; }
                    count *= v;
                    if (lx.Next() != "]") { error = "expected ']'"; return false; }
                    x = lx.Next();
                }
                if (x == ":")
                {
                    bits = lx.Next().toInt();
                    x = lx.Next();
                }
                u32 size, align;
                if (ptr) { size = 4; align = 4; }
                else if (!resolveSize(ft, size, align)) { error = "unknown type '" + ft + "'"; return false; }
                TypeField f;
                f.Name = n;
                f.CType = ft + QString(ptr, '*');
                f.Size = size;
                f.Count = count;
                if (bits >= 0)
                {
                    if (bitUnitOff < 0 || bitUnitSize != (int)size || bitPos + bits > (int)size * 8)
                    {
                        offset = (offset + align - 1) & ~(align - 1);
                        bitUnitOff = (int)offset;
                        bitUnitSize = (int)size;
                        bitPos = 0;
                        offset += size;
                    }
                    f.Offset = (u32)bitUnitOff;
                    f.BitOffset = bitPos;
                    f.BitWidth = bits;
                    bitPos += bits;
                }
                else
                {
                    bitUnitOff = -1;
                    if (isUnion) f.Offset = 0;
                    else
                    {
                        offset = (offset + align - 1) & ~(align - 1);
                        f.Offset = offset;
                        offset += size * count;
                    }
                }
                maxAlign = std::max(maxAlign, align);
                maxSize = std::max(maxSize, size * count);
                out.Fields.push_back(f);
                if (x == ",") continue;
                if (x == ";") break;
                error = "expected ';' after field " + n;
                return false;
            }
        }
        u32 total = isUnion ? maxSize : offset;
        total = (total + maxAlign - 1) & ~(maxAlign - 1);
        if (isTypedef)
        {
            QString alias = lx.Next();
            if (!alias.isEmpty() && alias != ";") name = alias;
        }
        if (name.isEmpty()) { error = "struct needs a name"; return false; }
        out.Name = name;
        out.Kind = isUnion ? "union" : "struct";
        out.Size = (int)total;
        return true;
    }

    if (isTypedef)
    {
        // typedef <type> [*]name[N];
        QString base = t;
        while (base == "unsigned" || base == "signed" || base == "const" || base == "volatile") base = lx.Next();
        int ptr = 0;
        QString n = lx.Next();
        while (n == "*") { ptr++; n = lx.Next(); }
        u32 count = 1;
        QString x = lx.Next();
        while (x == "[")
        {
            count *= lx.Next().toUInt();
            lx.Next();
            x = lx.Next();
        }
        u32 size, align;
        if (ptr) size = 4;
        else if (!resolveSize(base, size, align)) { error = "unknown type '" + base + "'"; return false; }
        out.Name = n;
        out.Kind = "typedef";
        out.Size = (int)(size * count);
        TypeField f;
        f.Name = "value";
        f.CType = base + QString(ptr, '*');
        f.Size = size;
        f.Count = count;
        out.Fields.push_back(f);
        return true;
    }

    error = "expected struct, union, enum or typedef";
    return false;
}

}
