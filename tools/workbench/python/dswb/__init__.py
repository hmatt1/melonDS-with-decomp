"""Python bindings for the melonDS DS Decomp Workbench.

- dswb.Client: typed JSON-RPC client for the control and query API
- dswb.session: session file reader
- dswb._generated: record layouts, enums and API types generated from
  src/workbench/schema/workbench.schema.json (do not edit)
"""
from ._generated import *  # noqa: F401,F403
from .client import ApiError, Client, find_info_file  # noqa: F401
from . import session  # noqa: F401
