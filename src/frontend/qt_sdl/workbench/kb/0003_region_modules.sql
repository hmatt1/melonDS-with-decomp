-- Migration 0003: 'region' modules.
-- Code and data can live outside the modules listed in the ROM (homebrew that relocates
-- itself, code generated at run time). Region modules (main RAM, ITCM, WRAM per CPU) give
-- such addresses a home; ROM modules take precedence wherever they apply.

CREATE TABLE module_new (
    id            INTEGER PRIMARY KEY,
    name          TEXT NOT NULL UNIQUE,
    kind          TEXT NOT NULL CHECK (kind IN ('static', 'overlay', 'autoload', 'region')),
    cpu           TEXT NOT NULL CHECK (cpu IN ('arm9', 'arm7')),
    overlay_id    INTEGER,
    load_address  INTEGER NOT NULL,
    size          INTEGER NOT NULL,
    bss_size      INTEGER NOT NULL DEFAULT 0,
    rom_file      TEXT,
    compressed    INTEGER NOT NULL DEFAULT 0 CHECK (compressed IN (0, 1))
);
INSERT INTO module_new SELECT id, name, kind, cpu, overlay_id, load_address, size, bss_size, rom_file, compressed FROM module;
DROP TABLE module;
ALTER TABLE module_new RENAME TO module;
