"""Fault injection for the shell runner, including Bash's conditional-errexit trap."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "crates/scbridge-ffi/ctest/run.sh"

class ShellRunnerTests(unittest.TestCase):
    def test_failed_steps_fail_even_when_sourced_in_a_conditional(self):
        for step in ("cc", "cancel", "smoke", "cmp", "tar", "python3", "cli", "cache_cli", "cache_miss"):
            for keep in ("0", "1"):
                with self.subTest(step=step, keep=keep), tempfile.TemporaryDirectory() as temporary:
                    temp = Path(temporary)
                    def executable(name, body):
                        path = temp / name
                        path.write_text("#!/bin/bash\n" + body)
                        path.chmod(0o755)
                        return str(path)
                    probe = executable("discover", "echo /synthetic/Data.p4k\n")
                    library = temp / "libscbridge.so"
                    library.touch()
                    executable("cc", '''[[ $FAIL_STEP == cc ]] && exit 9
while [[ $# -gt 0 ]]; do
  if [[ $1 == -o ]]; then shift; output=$1; fi
  shift
done
cat > "$output" <<'STUB'
#!/bin/bash
[[ $FAIL_STEP == ${0##*/} ]] && exit 9
if [[ $3 == smoke || $3 == bench ]]; then
  mkdir -p "$1"
  printf fixture > "$1/ffi.tar"
  printf fixture > "$1/no-animations.tar"
fi
exit 0
STUB
chmod +x "$output"
''')
                    cli = executable("cli", '''[[ $FAIL_STEP == cli ]] && exit 9
if [[ " $* " == *" --cache "* ]]; then
  [[ $FAIL_STEP == cache_cli ]] && exit 9
  if [[ $FAIL_STEP == cache_miss ]]; then echo 'cache: miss' >&2; exit 0; fi
fi
echo 'cache: hit' >&2
exit 0
''')
                    for name in ("cmp", "tar", "python3"):
                        executable(name, f'[[ $FAIL_STEP == {name} ]] && exit 9\nexit 0\n')
                    env = dict(os.environ, PATH=f"{temp}:{os.environ['PATH']}",
                               FAIL_STEP=step, SCBRIDGE_CTEST_KEEP=keep,
                               SCBRIDGE_CTEST_LIB=str(library), SCBRIDGE_CTEST_CLI=cli,
                               SCBRIDGE_CTEST_DISCOVER=probe)
                    before = set((ROOT / "out").glob("wp-r4-ctest.*"))
                    result = subprocess.run(["bash", "-c", 'source "$0" || exit "$?"', str(SCRIPT)],
                                            cwd=ROOT, env=env, capture_output=True, text=True)
                    after = set((ROOT / "out").glob("wp-r4-ctest.*")) - before
                    try:
                        self.assertEqual(result.returncode, 1 if step == "cache_miss" else 9, result.stdout + result.stderr)
                        self.assertEqual(len(after), 1 if keep == "1" else 0)
                        self.assertNotIn("C test: PASS", result.stdout)
                    finally:
                        import shutil
                        for path in after:
                            shutil.rmtree(path)

if __name__ == "__main__":
    unittest.main()
