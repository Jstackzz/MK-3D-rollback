"""Run aqtinstall with support for the Qt 6.11+ split repository layout on Windows.

aqtinstall 3.3.0 looks for online/qtsdkrepository/windows_x86/desktop/qt6_6120/qt6_6120/Updates.xml,
but from Qt 6.11 the desktop packages live in per-toolchain folders such as qt6_6120/qt6_6120_msvc2022_64/.
This wrapper maps the desktop Windows architectures to that folder suffix, then runs aqt unchanged.
Use it only for Qt 6.11 and newer (older versions keep the old layout; run `python -m aqt` for them):

    python launcher/tools/aqt_split_layout.py install-qt windows desktop 6.12.0 win64_msvc2022_64 -O external/qt
"""
import sys

from aqt import metadata

_orig = metadata.QtRepoProperty.extension_for_arch
_MAP = {"win64_msvc2022_64": "msvc2022_64", "win64_mingw": "mingw", "win64_llvm_mingw": "llvm_mingw"}


def _patched(architecture, is_version_ge_6):
    if is_version_ge_6 and architecture in _MAP:
        return _MAP[architecture]
    return _orig(architecture, is_version_ge_6)


metadata.QtRepoProperty.extension_for_arch = staticmethod(_patched)

import aqt  # noqa: E402

if __name__ == "__main__":
    sys.exit(aqt.main())
