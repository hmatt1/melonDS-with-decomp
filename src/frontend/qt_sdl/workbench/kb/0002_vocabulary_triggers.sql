-- Migration 0002: enforce vocabularies for every writer.
-- Foreign keys are only checked on connections that enable them; these triggers
-- hold regardless of how a tool connects.

CREATE TRIGGER function_vocab_insert BEFORE INSERT ON function
WHEN NEW.mode NOT IN (SELECT mode FROM vocab_mode) OR NEW.subsystem NOT IN (SELECT name FROM vocab_subsystem)
BEGIN
    SELECT RAISE(ABORT, 'function.mode or function.subsystem is not in its vocabulary');
END;

CREATE TRIGGER function_vocab_update BEFORE UPDATE OF mode, subsystem ON function
WHEN NEW.mode NOT IN (SELECT mode FROM vocab_mode) OR NEW.subsystem NOT IN (SELECT name FROM vocab_subsystem)
BEGIN
    SELECT RAISE(ABORT, 'function.mode or function.subsystem is not in its vocabulary');
END;

-- authors must exist (every write is attributed)
CREATE TRIGGER annotation_author_insert BEFORE INSERT ON annotation
WHEN NOT EXISTS (SELECT 1 FROM author WHERE id = NEW.author_id)
BEGIN
    SELECT RAISE(ABORT, 'annotation.author_id must name an author');
END;
