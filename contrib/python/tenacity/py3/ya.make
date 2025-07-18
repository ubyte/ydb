# Generated by devtools/yamaker (pypi).

PY3_LIBRARY()

VERSION(9.1.2)

LICENSE(Apache-2.0)

NO_LINT()

NO_CHECK_IMPORTS(
    tenacity.tornadoweb
)

PY_SRCS(
    TOP_LEVEL
    tenacity/__init__.py
    tenacity/_utils.py
    tenacity/after.py
    tenacity/asyncio/__init__.py
    tenacity/asyncio/retry.py
    tenacity/before.py
    tenacity/before_sleep.py
    tenacity/nap.py
    tenacity/retry.py
    tenacity/stop.py
    tenacity/tornadoweb.py
    tenacity/wait.py
)

RESOURCE_FILES(
    PREFIX contrib/python/tenacity/py3/
    .dist-info/METADATA
    .dist-info/top_level.txt
    tenacity/py.typed
)

END()
