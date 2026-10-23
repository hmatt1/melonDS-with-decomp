-- Migration 0006: a system author for workbench-created rows (automatic functions).
-- The author table's kind check is rebuilt to allow 'system'. Existing authors
-- are copied unchanged. The annotation trigger names the author table, so it is
-- dropped for the rebuild and put back afterwards.

DROP TRIGGER IF EXISTS annotation_author_insert;

CREATE TABLE author_new (
    id INTEGER PRIMARY KEY,
    name TEXT NOT NULL UNIQUE,
    kind TEXT NOT NULL CHECK (kind IN ('person', 'agent', 'system')),
    created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);
INSERT INTO author_new (id, name, kind, created_at) SELECT id, name, kind, created_at FROM author;
DROP TABLE author;
ALTER TABLE author_new RENAME TO author;

CREATE TRIGGER annotation_author_insert BEFORE INSERT ON annotation
WHEN NOT EXISTS (SELECT 1 FROM author WHERE id = NEW.author_id)
BEGIN
    SELECT RAISE(ABORT, 'annotation.author_id must name an author');
END;
