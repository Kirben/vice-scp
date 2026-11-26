"""Check both sides of 1571 SCP fixtures using a built c1541.

Usage: python test-c1541-1571.py C1541 HEAD0_SCP HEAD1_SCP EXPECTED_PRG
Both fixtures must contain SAVECHECK with the supplied expected contents,
allocated on the indicated head. Only temporary copies are attached.
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
    executable, head0, head1, expected = map(lambda value: Path(value).resolve(), sys.argv[1:])
    contents = expected.read_bytes()
    with tempfile.TemporaryDirectory(prefix="vice-scp-1571-", dir=Path.cwd()) as directory:
        for head, source in enumerate((head0, head1)):
            original = digest(source)
            image = Path(directory) / ("head%d.scp" % head)
            output = Path(directory) / ("head%d.prg" % head)
            shutil.copyfile(source, image)
            before = digest(image)
            subprocess.run([str(executable), "-attach", image.as_posix(), "-read", "savecheck", output.as_posix()],
                           check=True)
            assert output.read_bytes() == contents, "incorrect extracted data on head %d" % head
            assert digest(image) == before, "test copy changed"
            assert digest(source) == original, "source changed"
            print("head %d extraction matches expected PRG; SCP unchanged" % head, flush=True)


if __name__ == "__main__":
    main()
