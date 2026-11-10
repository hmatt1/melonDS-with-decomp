"""The fingerprint the workbench keeps for an approved compare command (Workbench.CompareApproved).

Compare asks before running a command it hasn't seen, or after the command or the project's
compare script changed. Tests that drive Compare set this in melonDS.toml up front: the sha1 of the
command template, a NUL byte and the script, which a fresh project copies from the template
(tools/workbench/compare/compare.py, embedded in the build).
"""
import hashlib
import os

HERE = os.path.dirname(os.path.abspath(__file__))
TEMPLATE = os.path.join(HERE, "..", "compare", "compare.py")

# WBHost::DefaultCompareCommand(), the command an empty Workbench.CompareCommand runs (keep in step)
DEFAULT_COMMAND = ('"{python}" "{script}" --file "{file}" --symbol {name} --target "{target}" --context "{context}" '
                   '--compiler "{compiler}" --compiler-label "{compiler_label}" --flags "{flags}" --objdiff "{objdiff}" --timeout {timeout}')


def fingerprint(command, script_path=TEMPLATE):
    with open(script_path, "rb") as f:
        script = f.read()
    return hashlib.sha1(command.encode("utf-8") + b"\0" + script).hexdigest()
