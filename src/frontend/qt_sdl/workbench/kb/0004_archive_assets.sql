-- Migration 0004: files inside game archives are assets of their own.
-- TrackMania DS keeps nearly every asset in vfs.zip. Each archived file gets an asset row
-- (member = its path inside the archive; '' for a plain NitroFS file), and two link kinds
-- record what the game did with it: 'archive_read' (card reads of its bytes in the archive)
-- and 'content' (where its unpacked bytes were found in RAM, and the code that wrote them).

CREATE TABLE asset_new (
    id            INTEGER PRIMARY KEY,
    file_id       INTEGER NOT NULL,              -- NitroFS FAT id or special id; the archive's id for members
    member        TEXT NOT NULL DEFAULT '',      -- path inside the archive, '' for the ROM file itself
    path          TEXT NOT NULL,
    format        TEXT NOT NULL DEFAULT '',
    rom_offset    INTEGER NOT NULL,              -- members: ROM offset of the compressed data
    size          INTEGER NOT NULL,              -- members: unpacked size
    packed_size   INTEGER,
    UNIQUE (file_id, member)
);
INSERT INTO asset_new(id, file_id, member, path, format, rom_offset, size)
    SELECT id, file_id, '', path, format, rom_offset, size FROM asset;

CREATE TABLE asset_link_new (
    id            INTEGER PRIMARY KEY,
    asset_id      INTEGER NOT NULL REFERENCES asset(id) ON DELETE CASCADE,
    kind          TEXT NOT NULL CHECK (kind IN ('card_read', 'decompress', 'dma', 'archive_read', 'content')),
    ram_start     INTEGER NOT NULL,
    ram_end       INTEGER NOT NULL,
    loader_function_id INTEGER REFERENCES function(id),
    loader_pc     INTEGER,
    session_id    INTEGER REFERENCES session(id) ON DELETE CASCADE,
    frame         INTEGER
);
INSERT INTO asset_link_new SELECT * FROM asset_link;

DROP TABLE asset_link;
DROP TABLE asset;
ALTER TABLE asset_new RENAME TO asset;
ALTER TABLE asset_link_new RENAME TO asset_link;
CREATE INDEX asset_link_asset ON asset_link(asset_id);
