"""Compile the production out-of-submission fence block against CPU-only mocks.

Run with a C++ compiler on PATH (on Windows, use a developer command prompt):
    python scripts/tests/test_d3d12_submission_fence.py --cxx clang-cl

--source accepts an unmodified command_processor.cpp from an earlier revision
as a negative control. No D3D12 device, queue, OS event, or real wait is created.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

REPO_ROOT = Path(__file__).resolve().parents[2]


def extract_block(source: str) -> str:
    function = source.index("void D3D12CommandProcessor::CheckSubmissionFence(")
    start = source.index("if (queue_operations_done_since_submission_signal_) {", function)
    end = source.index("// A submission won't be ended", start)
    return source[start:end].rstrip()


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source", type=Path,
        default=REPO_ROOT / "src/graphics/d3d12/command_processor.cpp",
    )
    parser.add_argument(
        "--cxx", default=os.environ.get("CXX", "clang-cl" if os.name == "nt" else "c++"),
    )
    args = parser.parse_args(argv)
    compiler = shutil.which(args.cxx)
    if compiler is None:
        parser.error(f"C++ compiler not found: {args.cxx}")
    harness = Path(__file__).with_suffix(".cpp.in").read_text(encoding="utf-8")
    assert harness.count("// PRODUCTION_FENCE_BLOCK") == 1
    block = extract_block(args.source.read_text(encoding="utf-8"))
    harness = harness.replace("// PRODUCTION_FENCE_BLOCK", block)
    with tempfile.TemporaryDirectory(prefix="d3d12-fence-test-") as directory:
        build = Path(directory)
        source = build / "fence_test.cpp"
        binary = build / ("fence_test.exe" if os.name == "nt" else "fence_test")
        source.write_text(harness, encoding="utf-8")
        if Path(compiler).stem.lower() in ("clang-cl", "cl"):
            command = [
                compiler, "/nologo", "/std:c++20", "/EHsc", "/W4", str(source), f"/Fe:{binary}",
            ]
        else:
            command = [compiler, "-std=c++20", "-Wall", "-Wextra", str(source), "-o", str(binary)]
        subprocess.run(command, cwd=build, check=True, timeout=60)
        return subprocess.run([str(binary)], cwd=build, timeout=10).returncode


def test_production_fence_block():
    # Collected by the existing scripts/tests pytest CI job; requires a C++ compiler.
    assert main([]) == 0


if __name__ == "__main__":
    raise SystemExit(main())
