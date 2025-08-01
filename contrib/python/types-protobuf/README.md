## Typing stubs for protobuf

This is a [PEP 561](https://peps.python.org/pep-0561/)
type stub package for the [`protobuf`](https://github.com/protocolbuffers/protobuf) package.
It can be used by type-checking tools like
[mypy](https://github.com/python/mypy/),
[pyright](https://github.com/microsoft/pyright),
[pytype](https://github.com/google/pytype/),
[Pyre](https://pyre-check.org/),
PyCharm, etc. to check code that uses `protobuf`. This version of
`types-protobuf` aims to provide accurate annotations for
`protobuf~=6.30.2`.

Partially generated using [mypy-protobuf==3.6.0](https://github.com/nipunn1313/mypy-protobuf/tree/v3.6.0) and libprotoc 29.0 on [protobuf v30.2](https://github.com/protocolbuffers/protobuf/releases/tag/v30.2) (python `protobuf==6.30.2`).

This stub package is marked as [partial](https://peps.python.org/pep-0561/#partial-stub-packages).
If you find that annotations are missing, feel free to contribute and help complete them.


This package is part of the [typeshed project](https://github.com/python/typeshed).
All fixes for types and metadata should be contributed there.
See [the README](https://github.com/python/typeshed/blob/main/README.md)
for more details. The source for this package can be found in the
[`stubs/protobuf`](https://github.com/python/typeshed/tree/main/stubs/protobuf)
directory.

This package was tested with
mypy 1.16.1,
pyright 1.1.400,
and pytype 2024.10.11.
It was generated from typeshed commit
[`dbd3ad356ef3bceae506fb71a29a48b6192213ba`](https://github.com/python/typeshed/commit/dbd3ad356ef3bceae506fb71a29a48b6192213ba).