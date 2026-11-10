-- Migration 0005: how a range is decoded, set by a person or an agent when the recording can't tell
-- (code never executed, data inside a function). Absolute addresses; module_id names the overlay for
-- overlay code, since several overlays share addresses.

CREATE TABLE decode_override (
    id          INTEGER PRIMARY KEY,
    cpu         TEXT NOT NULL DEFAULT 'arm9' CHECK (cpu IN ('arm9', 'arm7')),
    module_id   INTEGER REFERENCES module(id),
    start_addr  INTEGER NOT NULL,
    end_addr    INTEGER NOT NULL,                -- exclusive
    mode        TEXT NOT NULL CHECK (mode IN ('arm', 'thumb', 'data')),
    author_id   INTEGER NOT NULL REFERENCES author(id),
    created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    CHECK (end_addr > start_addr)
);
CREATE INDEX decode_override_addr ON decode_override(cpu, start_addr);
