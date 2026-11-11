"""Check complete c1541 extraction from a disposable image copy.

Usage: python test-c1541-extract.py C1541 IMAGE EXPECTED_FILE EXTRACTED_NAME
The fixture must contain the named file with EXPECTED_FILE's contents.
"""
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).digest()


def main():
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    executable, source, expected = (Path(value).resolve() for value in sys.argv[1:4])
    name = sys.argv[4]
    assert Path(name).name == name, "extracted name must be a basename"
    contents = expected.read_bytes()
    original = digest(source)
    with tempfile.TemporaryDirectory(prefix="vice-extract-", dir=Path.cwd()) as directory:
        image = Path(directory) / ("input" + source.suffix)
        shutil.copyfile(source, image)
        result = subprocess.run([str(executable), "-attach", image.as_posix(), "-extract"],
                                cwd=directory, capture_output=True, check=True)
        assert b"trying to extract more data" not in result.stderr, result.stderr
        extracted = Path(directory) / name
        assert extracted.read_bytes() == contents, "extracted file differs from expected data"
        assert digest(image) == original, "test copy changed"
        assert digest(source) == original, "source changed"
        print("extracted %d bytes, matching expected data; image unchanged" % len(contents))


if __name__ == "__main__":
    main()
