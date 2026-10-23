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

#ifndef WB_KNOWLEDGESTORE_H
#define WB_KNOWLEDGESTORE_H

#include <functional>
#include <map>
#include <vector>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>

#include "types.h"

struct sqlite3;
struct sqlite3_stmt;

namespace melonDS::WB { class NitroROM; }

namespace WBKB
{
using melonDS::u32;
using melonDS::u8;

struct Module
{
    int Id = 0;
    QString Name, Kind, Cpu;
    int OverlayId = -1;
    u32 Load = 0, Size = 0, Bss = 0;
    QString RomFile;
    bool Compressed = false;
    u32 End() const { return Load + Size + Bss; }
    int CpuNum() const { return Cpu == "arm7" ? 1 : 0; }
};

struct Function
{
    qint64 Id = 0;
    int ModuleId = 0;
    u32 Offset = 0, Size = 0;
    u32 Addr = 0;                    // absolute
    QString Mode = "arm";
    QString Name, Signature, Subsystem, SourceFile, Author, AuthorKind;
    QString Status = "not_started";  // derived (function_status view)
    double LastScore = -1;
    bool Thumb() const { return Mode == "thumb"; }
};

struct Label
{
    qint64 Id = 0;
    int ModuleId = 0;
    u32 Offset = 0, Addr = 0;
    QString Name, Author, AuthorKind;
};

struct TypeField
{
    QString Name;
    QString CType;
    u32 Offset = 0;
    u32 Size = 0;
    u32 Count = 1;          // array length
    int BitOffset = -1, BitWidth = 0;
};

struct Type
{
    qint64 Id = 0;
    QString Name, Kind, Definition, Author, AuthorKind;
    int Size = -1;
    std::vector<TypeField> Fields;
};

struct DataObject
{
    qint64 Id = 0;
    int ModuleId = -1;
    QString Region;
    u32 Offset = 0, Addr = 0, Size = 0;
    QString Name, TypeName, Author, AuthorKind;
    qint64 OwnerFunction = 0;
};

// how a range is decoded when the recording can't tell: set by a person or an agent
struct DecodeOverride
{
    qint64 Id = 0;
    int Cpu = 0;
    int ModuleId = -1;
    u32 Start = 0, End = 0;          // absolute, End exclusive
    QString Mode;                    // arm, thumb, data
    QString Author, AuthorKind;
};

struct Annotation
{
    qint64 Id = 0;
    QString TargetKind = "address";
    int ModuleId = -1;
    qint64 Offset = -1, EndOffset = -1;
    qint64 FunctionId = 0, DataObjectId = 0;
    QString TypeName, FieldName, Register;
    int OverlayId = -1;
    QString Savestate;
    QString Comment, Tags;
    QString Author, AuthorKind;
    QString Created, Updated;
    // resolved absolute addresses (address/range/register targets)
    bool HasAddr = false;
    u32 Addr = 0, EndAddr = 0;
    int Cpu = 0;
    QStringList TagList() const;
};

struct Bookmark
{
    qint64 Id = 0;
    u32 Address = 0;
    int Cpu = 0;
    QString Name, Author, AuthorKind;
};

struct BuildResult
{
    qint64 Id = 0;
    qint64 FunctionId = 0;
    QString Compiler, Flags, Diff, CommitHash, SourceFile, Author, Created;
    double Score = 0;
    bool Matched = false;
};

struct Xref
{
    u32 From = 0, To = 0;
    QString Kind, Source;
};

struct EvidenceRow
{
    qint64 FunctionId;
    u32 ExecCount, InstrCount, BytesExecuted;
    QString Callers, Callees, DataTouched;
};

struct AssetLinkRow
{
    u32 FileId;
    QString Path;
    u32 RomOffset, Size;
    QString Kind;                 // card_read, decompress, dma, archive_read, content
    u32 RamStart, RamEnd, LoaderPC;
    u32 Frame;
    QString Member;               // path inside an archive (FileId is the archive), empty otherwise
    u32 PackedSize = 0;
};

// a file inside a game archive (TrackMania DS's vfs.zip)
struct ArchiveAssetRow
{
    QString Member;
    u32 RomOffset, Size, PackedSize;
};

// Parses C struct/union/enum/typedef definitions for layout (offsets and sizes).
bool ParseTypeDefinition(const QString& def, const std::map<QString, Type>& known, Type& out, QString& error);

class KnowledgeStore : public QObject
{
    Q_OBJECT
public:
    explicit KnowledgeStore(QObject* parent = nullptr);
    ~KnowledgeStore();

    bool Open(const QString& path, const melonDS::WB::NitroROM& rom, const QString& personName, QString& error);
    void Close();
    bool IsOpen() const { return Db != nullptr; }
    QString Path() const { return DbPath; }
    int SchemaVersion() const { return Version; }
    QString PersonName() const { return Person; }

    // reload caches when another connection (an MCP server) committed
    bool CheckExternalChanges();
    void Reload();

    // lookups (absolute addresses; overlay = overlay id loaded at that address, -1 = unknown)
    const std::vector<Module>& Modules() const { return ModuleList; }
    const Module* ModuleById(int id) const;
    const Module* ModuleFor(int cpu, u32 addr, int overlay = -1) const;
    const Function* FunctionAt(int cpu, u32 addr, int overlay = -1) const;      // containing addr
    const Function* FunctionStarting(int cpu, u32 addr, int overlay = -1) const;
    const Function* FunctionById(qint64 id) const;
    const Label* LabelAt(int cpu, u32 addr, int overlay = -1) const;
    const DataObject* DataObjectAt(int cpu, u32 addr, int overlay = -1) const;  // containing addr
    const Type* TypeByName(const QString& name) const;
    QString NameAt(int cpu, u32 addr, int overlay = -1, bool withOffset = false) const;
    bool LookupName(const QString& name, u32& addr, int& cpu) const;
    const std::vector<Function>& Functions() const { return FunctionList; }
    const std::vector<Label>& Labels() const { return LabelList; }
    const std::vector<DataObject>& DataObjects() const { return DataList; }
    const std::map<QString, Type>& Types() const { return TypeMap; }
    const std::vector<Annotation>& Annotations() const { return AnnotationList; }
    std::vector<const Annotation*> AnnotationsAt(int cpu, u32 addr, int overlay = -1) const;
    std::vector<const Annotation*> RegisterAnnotations(const Function* f, u32 pc) const;
    const std::vector<Bookmark>& Bookmarks() const { return BookmarkList; }
    const std::vector<DecodeOverride>& DecodeOverrides() const { return OverrideList; }
    const DecodeOverride* DecodeOverrideAt(int cpu, u32 addr, int overlay = -1) const;
    std::vector<BuildResult> BuildResults(qint64 functionId, int limit = 50);
    std::vector<Xref> XrefsTo(int cpu, u32 addr, int overlay = -1);
    QString DecompilerText(qint64 functionId, QString* tool = nullptr);
    // decompiler output with this store's names in place of the tool's placeholder names
    // (FUN_02001234, DAT_...) and raw addresses; overlayAt gives the overlay loaded at an address
    QString NameDecompiled(const QString& text, int cpu, const std::function<int(u32)>& overlayAt) const;

    // writes (all logged in change_log with the author, undoable)
    // writes made while one of these is alive undo and redo as one (a dialog that saves in several steps)
    struct Group
    {
        explicit Group(KnowledgeStore& k) : K(k) { K.BeginGroup(); }
        ~Group() { K.EndGroup(); }
        Group(const Group&) = delete;
        Group& operator=(const Group&) = delete;
        KnowledgeStore& K;
    };
    bool UpsertFunction(int cpu, u32 addr, u32 size, const QString& mode, const QString& name, int overlay, QString& error, qint64* idOut = nullptr);
    // Automatic functions (sub_XXXXXXXX). Inserted as the system author and not logged, so they
    // are not separate steps on the person's undo stack. Existing functions are left alone.
    struct AutoFunction
    {
        int Cpu = 0;
        melonDS::u32 Addr = 0;
        melonDS::u32 Size = 0;
        bool Thumb = false;
        int Overlay = -1;
    };
    int DefineAutoFunctions(const std::vector<AutoFunction>& fns, QString& error);
    // Shrink automatic functions when a later entry is found inside them. Not an undo step.
    // A function the person has renamed or resized is left as it is. Returns how many changed.
    int ReviseAutoSizes(const std::vector<std::pair<qint64, melonDS::u32>>& sizes, QString& error);
    qint64 SystemAuthor();
    bool UpdateFunction(qint64 id, const QJsonObject& fields, QString& error);
    bool DeleteFunction(qint64 id, QString& error);
    bool Rename(int cpu, u32 addr, const QString& name, int overlay, QString& error);
    bool SetLabel(int cpu, u32 addr, const QString& name, int overlay, QString& error);
    bool DeleteLabel(qint64 id, QString& error);
    bool DefineData(int cpu, u32 addr, u32 size, const QString& typeName, const QString& name, int overlay, QString& error);
    bool DeleteDataObject(qint64 id, QString& error);
    bool DefineType(const QString& definition, QString& error);
    bool DeleteType(const QString& name, QString& error);
    bool SaveAnnotation(Annotation& a, QString& error);
    // fills the module/offset/function/data ids of an annotation from an address, for its TargetKind
    // (TypeName, FieldName, Register, OverlayId and Savestate are set by the caller)
    bool ResolveAnnotationTarget(Annotation& a, int cpu, u32 addr, bool haveAddr, u32 end, int overlay, bool wholeFunction, QString& error);
    bool DeleteAnnotation(qint64 id, QString& error);
    bool AddBookmark(int cpu, u32 addr, const QString& name, QString& error, qint64* idOut = nullptr);
    bool DeleteBookmark(qint64 id, QString& error);
    bool RecordBuildResult(const BuildResult& r, QString& error);
    bool AddXref(int cpu, u32 from, u32 to, const QString& kind, const QString& source, QString& error);
    bool SetDecompiled(qint64 functionId, const QString& tool, const QString& text, QString& error);
    // decode [start, end) as arm, thumb or data; "clear" removes overrides there. Overlapping
    // overrides are trimmed; the whole change undoes as one
    bool SetDecodeOverride(int cpu, u32 start, u32 end, const QString& mode, int overlay, QString& error);

    // session import
    // one asset per archived file (insert-or-ignore, so repeated loads are cheap)
    bool SeedArchiveAssets(u32 archiveFileId, const QString& archivePath, const std::vector<ArchiveAssetRow>& rows, QString& error);
    bool ImportSession(const QString& file, const QString& sha1, u32 frameStart, u32 frameEnd, u32 flags,
                       const std::vector<EvidenceRow>& evidence, const std::vector<AssetLinkRow>& assets, QString& error);

    // Edits are the person's unless made inside an AuthorScope (an agent working through the API).
    // Each author has its own undo history: Ctrl+Z never undoes an agent's change.
    qint64 PersonAuthor() const { return PersonId; }
    qint64 AgentAuthor(const QString& name);     // finds or creates an author of kind 'agent'
    class AuthorScope
    {
    public:
        AuthorScope(KnowledgeStore& kb, qint64 author, const QString& agentSession);
        ~AuthorScope();
    private:
        KnowledgeStore& K;
        qint64 PrevAuthor;
        QString PrevSession;
    };
    bool Undo(QString& error, qint64 author = 0);   // 0 = the person
    bool Redo(QString& error, qint64 author = 0);
    bool CanUndo(qint64 author = 0) const;
    bool CanRedo(qint64 author = 0) const;

    struct ChangeEntry
    {
        qint64 Id, Row;
        QString Table, Action, Author, AuthorKind, AgentSession, CreatedAt;
        bool Undone;
    };
    std::vector<ChangeEntry> RecentChanges(qint64 afterId, int limit);

    bool ModuleOffset(int cpu, u32 addr, int overlay, int& moduleId, u32& offset) const;

signals:
    void changed();

private:
    bool Exec(const char* sql, QString& error);
    bool Migrate(QString& error);
    bool EnsureModules(const melonDS::WB::NitroROM& rom, QString& error);
    qint64 AuthorId(const QString& name, const QString& kind);
    QJsonObject RowJson(const QString& table, qint64 id);
    bool LogChange(const QString& table, qint64 id, const QString& action, const QJsonObject& before, const QJsonObject& after);
    bool ApplyRow(const QString& table, qint64 id, const QJsonObject& row, bool insert, QString& error);
    bool DeleteRow(const QString& table, qint64 id, QString& error);
    int LastRc = 0;     // the SQLite result of ApplyRow or DeleteRow when it fails
    // rows the database deletes along with this one (ON DELETE CASCADE): build results, decompiler text ...
    std::vector<std::pair<QString, QJsonObject>> CascadedRows(const QString& table, qint64 id);
    struct Stmt;
    static QJsonObject ColumnsJson(Stmt& s);    // the current row, every column
    static void BindJson(Stmt& s, int idx, const QJsonValue& v);
    bool InsertRow(const QString& table, const QJsonObject& row, QString& error);
    bool Replay(bool undo, QString& error, qint64 author);
    static const char* KeyColumn(const QString& table);    // "id", or the table's own key
    // changes made while a group is open undo and redo as one (a delete and the rows that go with it)
    void BeginGroup() { if (GroupDepth++ == 0) GroupId = -1; }
    void EndGroup() { if (GroupDepth > 0) GroupDepth--; }
    int GroupDepth = 0;
    qint64 GroupId = 0;
    // logs the rows of `table` whose `column` is `id`, then deletes them (or clears the column)
    bool DetachRows(const QString& table, const QString& column, qint64 id, bool remove, QString& error);
    void LoadCaches();
    qint64 LastInsertId() const;

    sqlite3* Db = nullptr;
    QString DbPath;
    QString Person;
    qint64 PersonId = 0;
    qint64 ActiveAuthor = 0;          // author of the edits being made now
    QString ActiveSession;            // agent session tag for the change log, empty for the person
    int Version = 0;
    long long DataVersion = -1;

    std::vector<Module> ModuleList;
    std::vector<Function> FunctionList;          // sorted by (cpu, addr)
    std::vector<Label> LabelList;
    std::vector<DataObject> DataList;
    std::map<QString, Type> TypeMap;
    std::vector<Annotation> AnnotationList;
    std::vector<Bookmark> BookmarkList;
    std::vector<DecodeOverride> OverrideList;    // sorted by (cpu, start)
    std::map<qint64, size_t> FunctionIndex;

    struct ChangeRec
    {
        qint64 LogId; QString Table; qint64 Row; QString Action; QJsonObject Before, After; qint64 Group = 0;
        std::vector<std::pair<QString, QJsonObject>> Cascaded;     // removed with an undone insert, back on redo
    };
    struct Stacks { std::vector<ChangeRec> Undo, Redo; };
    std::map<qint64, Stacks> History;  // per author
};

}

#endif
