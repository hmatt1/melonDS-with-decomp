-- Migration 0007: status comes from build results alone, and library code has one subsystem.
-- Stores made before 0001 took this form get the same status view and vocabulary: a function
-- whose subsystem left the vocabulary becomes 'library', in the change log too, so the log
-- only names tags the vocabulary has.

DROP VIEW IF EXISTS function_status;

INSERT OR IGNORE INTO vocab_subsystem (name) VALUES ('library');
UPDATE function SET subsystem = 'library'
WHERE subsystem NOT IN ('unknown', 'game', 'library', 'msl', 'runtime');
UPDATE change_log SET before = json_set(before, '$.subsystem', 'library')
WHERE table_name = 'function' AND json_extract(before, '$.subsystem') NOT IN ('unknown', 'game', 'library', 'msl', 'runtime');
UPDATE change_log SET after = json_set(after, '$.subsystem', 'library')
WHERE table_name = 'function' AND json_extract(after, '$.subsystem') NOT IN ('unknown', 'game', 'library', 'msl', 'runtime');
DELETE FROM vocab_subsystem WHERE name NOT IN ('unknown', 'game', 'library', 'msl', 'runtime');

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
