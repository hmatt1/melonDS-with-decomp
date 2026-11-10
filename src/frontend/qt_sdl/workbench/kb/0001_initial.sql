-- DS Decomp Workbench knowledge store, migration 0001.
-- Addresses are integers: an offset within a module. Hex appears only in tools and UIs.
-- Every write carries an author; status is derived from evidence (build results).

CREATE TABLE IF NOT EXISTS schema_migrations (
    version     INTEGER PRIMARY KEY,
    name        TEXT NOT NULL,
    applied_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE TABLE meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE author (
    id          INTEGER PRIMARY KEY,
    name        TEXT NOT NULL UNIQUE,
    kind        TEXT NOT NULL CHECK (kind IN ('person', 'agent')),
    created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

-- vocabularies
CREATE TABLE vocab_mode (mode TEXT PRIMARY KEY);
INSERT INTO vocab_mode VALUES ('arm'), ('thumb');

CREATE TABLE vocab_subsystem (name TEXT PRIMARY KEY, description TEXT NOT NULL DEFAULT '');
INSERT INTO vocab_subsystem (name) VALUES ('unknown'), ('game'), ('library'), ('msl'), ('runtime');

CREATE TABLE module (
    id            INTEGER PRIMARY KEY,
    name          TEXT NOT NULL UNIQUE,          -- arm9, arm7, ov9_0003, itcm, dtcm
    kind          TEXT NOT NULL CHECK (kind IN ('static', 'overlay', 'autoload')),
    cpu           TEXT NOT NULL CHECK (cpu IN ('arm9', 'arm7')),
    overlay_id    INTEGER,
    load_address  INTEGER NOT NULL,
    size          INTEGER NOT NULL,
    bss_size      INTEGER NOT NULL DEFAULT 0,
    rom_file      TEXT,
    compressed    INTEGER NOT NULL DEFAULT 0 CHECK (compressed IN (0, 1))
);

CREATE TABLE function (
    id          INTEGER PRIMARY KEY,
    module_id   INTEGER NOT NULL REFERENCES module(id),
    offset      INTEGER NOT NULL,
    size        INTEGER NOT NULL DEFAULT 0 CHECK (size >= 0),
    mode        TEXT NOT NULL REFERENCES vocab_mode(mode),
    name        TEXT,
    signature   TEXT,
    subsystem   TEXT NOT NULL DEFAULT 'unknown' REFERENCES vocab_subsystem(name),
    source_file TEXT,                            -- path in the decomp git tree
    author_id   INTEGER NOT NULL REFERENCES author(id),
    created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    UNIQUE (module_id, offset)
);
CREATE INDEX function_name ON function(name);

CREATE TABLE data_object (
    id          INTEGER PRIMARY KEY,
    module_id   INTEGER REFERENCES module(id),   -- NULL for objects in a region outside modules (heap, VRAM ...)
    region      TEXT,                            -- set when module_id is NULL: main_ram, dtcm, vram ...
    offset      INTEGER NOT NULL,                -- module offset, or absolute address when module_id is NULL
    name        TEXT,
    type_name   TEXT,
    size        INTEGER NOT NULL DEFAULT 0,
    owner_function_id INTEGER REFERENCES function(id),
    author_id   INTEGER NOT NULL REFERENCES author(id),
    created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    CHECK ((module_id IS NULL) <> (region IS NULL))
);
CREATE UNIQUE INDEX data_object_key ON data_object(ifnull(module_id, -1), ifnull(region, ''), offset);

CREATE TABLE type (
    id          INTEGER PRIMARY KEY,
    name        TEXT NOT NULL UNIQUE,
    kind        TEXT NOT NULL CHECK (kind IN ('struct', 'union', 'enum', 'typedef')),
    definition  TEXT NOT NULL,                   -- C source form
    size        INTEGER,
    author_id   INTEGER NOT NULL REFERENCES author(id),
    created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE TABLE label (
    id          INTEGER PRIMARY KEY,
    module_id   INTEGER NOT NULL REFERENCES module(id),
    offset      INTEGER NOT NULL,
    name        TEXT NOT NULL,
    author_id   INTEGER NOT NULL REFERENCES author(id),
    created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    UNIQUE (module_id, offset)
);

CREATE TABLE annotation (
    id            INTEGER PRIMARY KEY,
    target_kind   TEXT NOT NULL CHECK (target_kind IN ('address', 'range', 'function', 'data_object', 'struct_field', 'type', 'register', 'overlay', 'savestate')),
    module_id     INTEGER REFERENCES module(id),
    offset        INTEGER,
    end_offset    INTEGER,                       -- range: exclusive end; register: end of the PC range
    function_id   INTEGER REFERENCES function(id),
    data_object_id INTEGER REFERENCES data_object(id),
    type_name     TEXT,
    field_name    TEXT,
    register      TEXT CHECK (register IS NULL OR register IN ('r0','r1','r2','r3','r4','r5','r6','r7','r8','r9','r10','r11','r12','sp','lr','pc')),
    overlay_id    INTEGER,
    savestate     TEXT,
    comment       TEXT NOT NULL,
    tags          TEXT NOT NULL DEFAULT '',      -- comma separated
    author_id     INTEGER NOT NULL REFERENCES author(id),
    created_at    TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at    TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);
CREATE INDEX annotation_addr ON annotation(module_id, offset);
CREATE INDEX annotation_func ON annotation(function_id);

CREATE TABLE bookmark (
    id          INTEGER PRIMARY KEY,
    module_id   INTEGER REFERENCES module(id),
    address     INTEGER NOT NULL,                -- absolute address (bookmarks may point outside modules)
    cpu         TEXT NOT NULL DEFAULT 'arm9' CHECK (cpu IN ('arm9', 'arm7')),
    name        TEXT NOT NULL,
    author_id   INTEGER NOT NULL REFERENCES author(id),
    created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE TABLE task (
    id          INTEGER PRIMARY KEY,
    title       TEXT NOT NULL,
    description TEXT NOT NULL DEFAULT '',
    priority    INTEGER NOT NULL DEFAULT 2 CHECK (priority BETWEEN 0 AND 4),
    state       TEXT NOT NULL DEFAULT 'open' CHECK (state IN ('open', 'in_progress', 'blocked', 'done', 'wontfix')),
    entity_kind TEXT CHECK (entity_kind IS NULL OR entity_kind IN ('function', 'data_object', 'type', 'module', 'address', 'asset')),
    entity_id   INTEGER,
    author_id   INTEGER NOT NULL REFERENCES author(id),
    created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE TABLE session (
    id              INTEGER PRIMARY KEY,
    file            TEXT NOT NULL UNIQUE,
    rom_sha1        TEXT NOT NULL,
    frame_start     INTEGER NOT NULL,
    frame_end       INTEGER NOT NULL,
    flags           INTEGER NOT NULL DEFAULT 0,
    tool_version    TEXT NOT NULL,
    author_id       INTEGER NOT NULL REFERENCES author(id),
    imported_at     TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE TABLE runtime_evidence (
    function_id   INTEGER NOT NULL REFERENCES function(id) ON DELETE CASCADE,
    session_id    INTEGER NOT NULL REFERENCES session(id) ON DELETE CASCADE,
    exec_count    INTEGER NOT NULL,              -- executions of the entry instruction
    instr_count   INTEGER NOT NULL,              -- total instruction executions inside the function
    bytes_executed INTEGER NOT NULL,
    callers       TEXT NOT NULL DEFAULT '[]',    -- JSON [{"addr":..., "call_site":..., "count":...}]
    callees       TEXT NOT NULL DEFAULT '[]',
    data_touched  TEXT NOT NULL DEFAULT '[]',    -- JSON [{"start":..., "end":..., "access":"r|w|rw"}]
    PRIMARY KEY (function_id, session_id)
);

CREATE TABLE build_result (
    id            INTEGER PRIMARY KEY,
    function_id   INTEGER NOT NULL REFERENCES function(id) ON DELETE CASCADE,
    compiler      TEXT NOT NULL,
    flags         TEXT NOT NULL DEFAULT '',
    score         REAL NOT NULL CHECK (score >= 0 AND score <= 100),
    matched       INTEGER NOT NULL CHECK (matched IN (0, 1)),
    diff          TEXT NOT NULL DEFAULT '',      -- JSON instruction diff
    source_hash   TEXT NOT NULL DEFAULT '',
    commit_hash   TEXT NOT NULL DEFAULT '',
    source_file   TEXT,
    author_id     INTEGER NOT NULL REFERENCES author(id),
    created_at    TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);
CREATE INDEX build_result_func ON build_result(function_id, id);

CREATE TABLE asset (
    id            INTEGER PRIMARY KEY,
    file_id       INTEGER NOT NULL UNIQUE,       -- NitroFS FAT id or special id
    path          TEXT NOT NULL,
    format        TEXT NOT NULL DEFAULT '',
    rom_offset    INTEGER NOT NULL,
    size          INTEGER NOT NULL
);

CREATE TABLE asset_link (
    id            INTEGER PRIMARY KEY,
    asset_id      INTEGER NOT NULL REFERENCES asset(id) ON DELETE CASCADE,
    kind          TEXT NOT NULL CHECK (kind IN ('card_read', 'decompress', 'dma')),
    ram_start     INTEGER NOT NULL,
    ram_end       INTEGER NOT NULL,
    loader_function_id INTEGER REFERENCES function(id),
    loader_pc     INTEGER,
    session_id    INTEGER REFERENCES session(id) ON DELETE CASCADE,
    frame         INTEGER
);

CREATE TABLE xref (
    id            INTEGER PRIMARY KEY,
    from_module   INTEGER REFERENCES module(id),
    from_offset   INTEGER NOT NULL,
    to_module     INTEGER REFERENCES module(id),
    to_offset     INTEGER NOT NULL,              -- absolute address when to_module is NULL
    kind          TEXT NOT NULL CHECK (kind IN ('call', 'jump', 'read', 'write', 'data_ref')),
    source        TEXT NOT NULL CHECK (source IN ('static', 'runtime')),
    UNIQUE (from_module, from_offset, to_module, to_offset, kind, source)
);
CREATE INDEX xref_to ON xref(to_module, to_offset);

CREATE TABLE decompiler_output (
    function_id   INTEGER PRIMARY KEY REFERENCES function(id) ON DELETE CASCADE,
    tool          TEXT NOT NULL,                 -- ghidra, binaryninja
    text          TEXT NOT NULL,
    imported_at   TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE TABLE change_log (
    id            INTEGER PRIMARY KEY,
    author_id     INTEGER NOT NULL REFERENCES author(id),
    agent_session TEXT,
    table_name    TEXT NOT NULL,
    row_id        INTEGER NOT NULL,
    action        TEXT NOT NULL CHECK (action IN ('insert', 'update', 'delete')),
    before        TEXT,                          -- JSON row before the change
    after         TEXT,                          -- JSON row after the change
    undone        INTEGER NOT NULL DEFAULT 0,
    created_at    TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);
CREATE INDEX change_log_row ON change_log(table_name, row_id);

-- Status is derived, never stored: the latest build result.
CREATE VIEW function_status AS
SELECT f.id AS function_id,
       CASE
         WHEN (SELECT b.matched FROM build_result b WHERE b.function_id = f.id ORDER BY b.id DESC LIMIT 1) = 1 THEN 'matched'
         WHEN EXISTS (SELECT 1 FROM build_result b WHERE b.function_id = f.id) THEN 'partially_matched'
         ELSE 'not_started'
       END AS status,
       (SELECT b.score FROM build_result b WHERE b.function_id = f.id ORDER BY b.id DESC LIMIT 1) AS last_score
FROM function f;
