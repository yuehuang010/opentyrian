#!/usr/bin/env python3
"""
hdkernels.py -- optional C acceleration for the HD asset pipeline's three hot
per-pixel kernels: the separable Lanczos-3 upscaler, the xBRZ 4x pixel-art
upscaler, and the QOI encoder/decoder.

The kernels themselves live in tools/hdkernels.c and are built by CMake into a
shared library (see the `hdkernels` target in CMakeLists.txt). This module finds
that library, loads it with ctypes, and exposes the four entry points. It is
STDLIB-ONLY on purpose: the HD tools are documented as runnable with no compiler
and no third-party Python packages, so `load()` returns None when the library is
absent and every caller falls back to its pure-Python implementation. ctypes is
the only binding mechanism that preserves that property (cffi/pybind11 would add
a hard dependency).

Search order for the library:
  1. $HD_KERNELS_LIB -- an explicit path to the shared library.
  2. $HD_KERNELS_DIR -- a directory to look in.
  3. <repo>/build and its per-config subdirs (Release/Debug/RelWithDebInfo/
     MinSizeRel), then the same for any other <repo>/build* directory. This
     covers both single-config generators (Ninja/Makefiles -> build/) and
     multi-config ones (Visual Studio -> build/Release/).

Set HD_KERNELS=0 in the environment to force the pure-Python fallback (used to
verify that the fallback still produces byte-identical output).

Build it with:
    cmake --build build --target hdkernels
"""

import ctypes
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Every platform's naming convention -- probing all of them is simpler, and just
# as correct, as branching on sys.platform.
_LIB_NAMES = [
    "hdkernels.dll",
    "libhdkernels.so",
    "libhdkernels.dylib",
]

_CONFIG_SUBDIRS = ["", "Release", "RelWithDebInfo", "MinSizeRel", "Debug"]

_MAX_QOI_BYTES_PER_PIXEL = 5  # worst case: an OP_RGBA chunk per pixel


def _build_dirs():
    """Every <repo>/build* directory, `build` first so a conventional layout
    wins over an incidentally-named one."""
    try:
        entries = sorted(os.listdir(REPO_ROOT))
    except OSError:
        return []
    dirs = [e for e in entries
            if e.startswith("build") and os.path.isdir(os.path.join(REPO_ROOT, e))]
    dirs.sort(key=lambda e: (e != "build", e))
    return [os.path.join(REPO_ROOT, e) for e in dirs]


def _candidate_paths():
    explicit = os.environ.get("HD_KERNELS_LIB")
    if explicit:
        yield explicit

    search_dirs = []
    hinted = os.environ.get("HD_KERNELS_DIR")
    if hinted:
        search_dirs.append(hinted)
    for build_dir in _build_dirs():
        for config in _CONFIG_SUBDIRS:
            search_dirs.append(os.path.join(build_dir, config) if config else build_dir)

    for directory in search_dirs:
        for name in _LIB_NAMES:
            yield os.path.join(directory, name)


class Kernels(object):
    """Thin ctypes wrapper. One instance per process; see load()."""

    def __init__(self, lib, path):
        self.path = path
        self._lib = lib

        u8p = ctypes.POINTER(ctypes.c_ubyte)

        lib.hdk_lanczos_upscale.restype = ctypes.c_int32
        lib.hdk_lanczos_upscale.argtypes = [
            ctypes.c_char_p, ctypes.c_int32, ctypes.c_int32,
            ctypes.c_int32, ctypes.c_int32, ctypes.c_int32, u8p,
        ]

        lib.hdk_xbrz_scale_4x.restype = ctypes.c_int32
        lib.hdk_xbrz_scale_4x.argtypes = [
            ctypes.c_char_p, ctypes.c_int32, ctypes.c_int32, u8p,
        ]

        lib.hdk_qoi_encode.restype = ctypes.c_int64
        lib.hdk_qoi_encode.argtypes = [
            ctypes.c_char_p, ctypes.c_int64, u8p, ctypes.c_int64,
        ]

        lib.hdk_qoi_decode.restype = ctypes.c_int32
        lib.hdk_qoi_decode.argtypes = [
            ctypes.c_char_p, ctypes.c_int64, ctypes.c_int64, u8p,
        ]

    @staticmethod
    def _out_buffer(size):
        """A writable ctypes view onto a fresh bytearray -- (buffer, view), so
        the C side writes straight into the bytearray with no extra copy."""
        buf = bytearray(size)
        return buf, (ctypes.c_ubyte * size).from_buffer(buf)

    def lanczos_upscale(self, src, src_w, src_h, dst_w, dst_h, channels=3):
        """Separable Lanczos-3 resample of an interleaved buffer. Returns a
        bytearray of dst_w * dst_h * channels bytes."""
        dst, view = self._out_buffer(dst_w * dst_h * channels)
        rc = self._lib.hdk_lanczos_upscale(
            bytes(src), src_w, src_h, dst_w, dst_h, channels, view)
        if rc != 0:
            raise RuntimeError("hdk_lanczos_upscale failed (rc=%d)" % rc)
        return dst

    def xbrz_scale_4x(self, rgba, src_w, src_h):
        """xBRZ 4x upscale of an RGBA buffer. Returns a bytearray of
        (src_w*4) * (src_h*4) * 4 bytes."""
        dst, view = self._out_buffer(src_w * 4 * src_h * 4 * 4)
        rc = self._lib.hdk_xbrz_scale_4x(bytes(rgba), src_w, src_h, view)
        if rc != 0:
            raise RuntimeError("hdk_xbrz_scale_4x failed (rc=%d)" % rc)
        return dst

    def qoi_encode(self, rgba_bytes, width, height):
        """Encode raw RGBA8888 into a bare QOI chunk stream. Returns bytes."""
        px_count = width * height
        cap = px_count * _MAX_QOI_BYTES_PER_PIXEL
        out, view = self._out_buffer(cap)
        written = self._lib.hdk_qoi_encode(bytes(rgba_bytes), px_count, view, cap)
        if written < 0:
            raise RuntimeError("hdk_qoi_encode failed (rc=%d)" % written)
        return bytes(out[:written])

    def qoi_decode(self, stream, width, height):
        """Decode a bare QOI chunk stream back to RGBA8888. Returns bytes."""
        px_count = width * height
        out, view = self._out_buffer(px_count * 4)
        rc = self._lib.hdk_qoi_decode(bytes(stream), len(stream), px_count, view)
        if rc != 0:
            raise RuntimeError("hdk_qoi_decode failed (rc=%d)" % rc)
        return bytes(out)


_cached = None
_loaded = False


def load():
    """Return a Kernels instance, or None if the C library isn't available (in
    which case callers must use their pure-Python kernels). Cached: the search
    and the dlopen happen once per process."""
    global _cached, _loaded

    if _loaded:
        return _cached
    _loaded = True

    if os.environ.get("HD_KERNELS") == "0":
        return None

    for path in _candidate_paths():
        if not os.path.isfile(path):
            continue
        try:
            lib = ctypes.CDLL(path)
            _cached = Kernels(lib, path)
        except (OSError, AttributeError):
            # Wrong architecture, or a stale library missing an entry point --
            # keep looking, and fall back to pure Python if nothing works.
            continue
        return _cached

    return None


def describe():
    """One-line status string for a tool to log, so it's always obvious from a
    build log which path actually ran."""
    kernels = load()
    if kernels is None:
        return ("kernels: pure Python (C library not found; build it with "
                "`cmake --build build --target hdkernels` for a much faster run)")
    return "kernels: C (%s)" % kernels.path


if __name__ == "__main__":
    print(describe())
    sys.exit(0 if load() is not None else 1)
