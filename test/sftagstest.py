#!/usr/bin/env python3
"""Check sftags pointer generation after longer tag lines."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def check(executable):
    with tempfile.TemporaryDirectory(prefix="nhsftags-") as work:
        root = Path(work)
        (root / "include").mkdir()
        (root / "util").mkdir()
        tags = root / "input.tags"
        for padding in (86, 96, 240, 600):
            for ending in ("\n", "\r\n", ""):
                rows = []
                for name, datatype in (("data", "permonst"),
                                       ("mextra", "mextra")):
                    # A stale final kind field used to turn the next
                    # member into a non-member, skipping pointer parsing.
                    rows.append('pad\tignored.h\t/^    int pad;$/;"\tignored:'
                                + "x" * padding + "\tv")
                    rows.append('%s\tmonst.h\t/^    struct %s *%s;$/;"'
                                '\tm\tstruct:monst\ttyperef:struct:%s *'
                                % (name, datatype, name, datatype))
                separator = ending or "\n"
                tags.write_bytes((separator.join(rows) + ending).encode())
                result = subprocess.run(
                    [str(executable), str(tags)], cwd=root / "util",
                    capture_output=True, text=True)
                assert result.returncode == 0, result.stdout + result.stderr
                generated = (root / "util" / "sfdata.c").read_text()
                for name in ("data", "mextra"):
                    for direction in ("sfo", "sfi"):
                        expected = ('%s_genericptr(nhfp, (genericptr_t) '
                                    '&d_monst->%s, "%s");'
                                    % (direction, name, name))
                        assert expected in generated, (
                            "pointer misparsed: %s, padding=%d, ending=%r"
                            % (name, padding, ending))
        print("sftags pointer generation PASS (12 fixtures)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sftags", nargs="?", default="util/sftags")
    args = parser.parse_args()
    check(Path(args.sftags).resolve())


if __name__ == "__main__":
    main()
