#!/usr/bin/env python3
"""runs the build's tests in a Wine, once as each GPU family, and prints what each made of them.

the tests are the build's own (meson's intro-tests.json: what tests/*/meson.build declares), run in a Wine the
build's libraries are installed into. a family other than the GPU's own lowers what the Metal bridge answers
(MULLION_GPU_FAMILY: src/winemetal/unix), so the paths an older GPU takes run here. a test passes when it says so on
every repeat: one that fails, ends without a result, outlives its time or answers differently on one repeat has failed.
the run fails on a failure or skip that tests/known.txt does not have, by what the test said, and on a test it lists
that passes.
"""
import argparse
import atexit
import concurrent.futures
import contextlib
import fcntl
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

# what a test's last word and exit status make of it (tests/*/..._test.hpp)
RESULTS = {"passed": 0, "skipped": 77}
# a runner that is told to end ends as one that is interrupted: through its finally clauses
signal.signal(signal.SIGTERM, lambda *_: sys.exit("ended"))
# how long a test waits for a machine short of memory before the run gives up
PATIENCE = 1800
# how long the machine's lock is left free for a job that waits for it
TURN = 6
# how long a test may say nothing after a fault of the GPU's before it counts as hung
QUIET = 30
# what says where a hung test's threads are, set by environment(): the Wine's binary and the build's program for it
# (tests/d3d11/where.cpp), the build's modules by name, and the symbolizer of the build's toolchain
WHERE = {}
# the Objective-C runtime keeps freed objects as zombies under the tests and says when one is messaged (Foundation,
# NSDebug.h: NSZombieEnabled): an object released once too often names itself there, whatever its memory holds by then
ZOMBIES = {"NSZombieEnabled": "YES"}
# every failure and skip that is known, by what the test said, with its cause: one that is not there, or that says
# something else, is new, whatever the test is called
KNOWN = Path(__file__).with_name("known.txt")
# under Metal's validation (--validate) a test can also answer otherwise, so those runs have their own
KNOWN_VALIDATED = Path(__file__).with_name("known-validated.txt")


def libraries(builds):
    """the libraries a Wine takes from the builds, the first build that has one giving it: unix ones, then PE ones"""
    unix, pe = {}, {}
    for build in builds:
        for path in sorted(build.glob("src/*/unix/*.so")):
            unix.setdefault(path.name, path)
        for path in sorted(build.glob("src/*/*.dll")):
            pe.setdefault(path.name, path)
    return unix, pe


# Wine's names for a PE file's machine (IMAGE_FILE_HEADER.Machine)
MACHINES = {0x8664: "x86_64", 0xAA64: "aarch64", 0x14C: "i386"}


def machine(pe):
    data = pe.read_bytes()
    header = int.from_bytes(data[0x3C:0x40], "little")
    return MACHINES[int.from_bytes(data[header + 4:header + 6], "little")]


def builtin(pe):
    """whether the build marked the library as one of Wine's own (winebuild --builtin), which Wine loads from beside
    its own; a library without the mark is an application's, loaded from the prefix"""
    with open(pe, "rb") as file:
        file.seek(0x40)
        return file.read(16) == b"Wine builtin DLL"


def install(wine, prefix, unix, pe):
    for path in pe.values():
        shutil.copy2(path, prefix / "drive_c" / "windows" / "system32")
        if builtin(path):
            shutil.copy2(path, wine / "lib" / "wine" / f"{machine(path)}-windows")
        if path.stem + ".so" in unix:
            shutil.copy2(unix[path.stem + ".so"], wine / "lib" / "wine" / f"{machine(path)}-unix")


# what the library and Metal say when the GPU's work went wrong under a test: a command buffer that failed
# (src/d3d12/d3d12_command_queue.cpp, src/mullion/mullion_command_queue.cpp), a rule of Metal's validation broken
# (Metal's debug layer), a shader's access that Metal's shader validation refused, which it reports and fills with zeros
# while the command buffer still completes (VALIDATION), a message to an Objective-C object that was freed (ZOMBIES)
FAULTS = ("Device error", "failed assertion", "Invalid Metal usage", "message sent to deallocated instance")
# Metal's API and shader validation, with the shader's reports on stderr, in the test's log (metalvalidation(1))
VALIDATION = {"MTL_DEBUG_LAYER": "1", "MTL_SHADER_VALIDATION": "1", "MTL_SHADER_VALIDATION_REPORT_TO_STDERR": "1"}


def waiting(markers):
    """whether a live process has a marker in the directory; a dead one's is removed"""
    for marker in Path(markers).glob("*"):
        try:
            os.kill(int(marker.name), 0)
            return True
        except (ProcessLookupError, ValueError):
            marker.unlink(missing_ok=True)
    return False


# the tests of this run that hold the machine's lock, which the first takes and the last leaves (--jobs)
HOLDERS = threading.Condition()
HOLDERS.count = 0


@contextlib.contextmanager
def alone():
    """one GPU job at a time on a machine that has others: MULLION_TEST_LOCK names their lock, a directory that holds
    its owner's process id and is taken over when the owner is gone. held for this run's tests, and let go between
    two of them while another job waits for it"""
    lock = os.environ.get("MULLION_TEST_LOCK")
    # MULLION_TEST_URGENT marks a run that others let go first, for as long as it lives: a check of one change
    # waits for one test of a long run, not for its turns
    urgent = lock and os.environ.get("MULLION_TEST_URGENT")
    with HOLDERS:
        # a waiter, which says so in the lock's .waiting directory (crossover/tools/lock.sh), gets the lock once the
        # tests that hold it end
        while lock and HOLDERS.count and not urgent and waiting(lock + ".waiting"):
            HOLDERS.wait(1)
        if urgent and not hasattr(alone, "marker"):
            alone.marker = Path(lock + ".urgent", str(os.getpid()))
            alone.marker.parent.mkdir(exist_ok=True)
            alone.marker.touch()
            atexit.register(alone.marker.unlink, missing_ok=True)
        if lock and not HOLDERS.count:
            asking = Path(lock + ".waiting", str(os.getpid()))
            asking.parent.mkdir(exist_ok=True)
            asking.touch()
        while lock and not HOLDERS.count:
            # the guard is never removed: publication and takeover must lock the same inode, including shell users
            with open(lock + ".guard", "a") as guard:
                fcntl.flock(guard, fcntl.LOCK_EX)
                if urgent or not waiting(lock + ".urgent"):
                    try:
                        os.mkdir(lock)
                        Path(lock, "pid").write_text(str(os.getpid()))
                        asking.unlink(missing_ok=True)
                        break
                    except FileExistsError:
                        try:
                            os.kill(int(Path(lock, "pid").read_text()), 0)
                        except (ProcessLookupError, FileNotFoundError, ValueError):
                            shutil.rmtree(lock, ignore_errors=True)
            time.sleep(5)
        HOLDERS.count += 1
    try:
        yield
    finally:
        with HOLDERS:
            HOLDERS.count -= 1
            if lock and not HOLDERS.count:
                with open(lock + ".guard", "a") as guard:
                    fcntl.flock(guard, fcntl.LOCK_EX)
                    shutil.rmtree(lock, ignore_errors=True)
                # the others look for the lock every few seconds: a waiter gets that long to take it, and takes its
                # mark away once it has
                for _ in range(TURN * 10):
                    if not waiting(lock + ".waiting"):
                        break
                    time.sleep(0.1)
            HOLDERS.notify_all()


def finish(command, env, seconds, log):
    """runs the command with its output in the log: its exit status, or None when it outlived its time. nothing
    starts on a machine short of memory (the kernel's pressure level, 1 when normal): a GPU test there can take the
    machine down with it. the test waits for the memory, up to PATIENCE seconds, and not while it has the machine's
    lock"""
    for waited in range(0, PATIENCE + 1, 10):
        with alone():
            level = subprocess.run(["sysctl", "-n", "kern.memorystatus_vm_pressure_level"], capture_output=True, text=True).stdout.strip()
            if level == "1":
                with open(log, "wb") as out:
                    end = time.monotonic() + seconds
                    process = subprocess.Popen(
                        command, env=env, stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT, start_new_session=True
                    )
                    # the test and what it started, which nothing else is in the session of, do not outlive their time
                    # or this runner: a test left behind would use the GPU without the lock
                    try:
                        # a test whose GPU work failed, or that called a result wrong, has failed: once it is also
                        # quiet, nothing waits for it
                        while time.monotonic() < end:
                            try:
                                return process.wait(QUIET)
                            except subprocess.TimeoutExpired:
                                lines = log.read_text(errors="replace").splitlines()
                                if time.time() - log.stat().st_mtime > QUIET and (
                                        faults(lines) or any(line.startswith("wrong: ") for line in lines)):
                                    break
                        where(command, env, process, log)
                        return None
                    finally:
                        if process.poll() is None:
                            os.killpg(process.pid, signal.SIGKILL)
                            process.wait()
        time.sleep(10)
    sys.exit(f"memory pressure level {level} for {PATIENCE} s: no test started")


def compiler(builds):
    """the C compiler the build's own programs for Windows are made with"""
    return next(json.loads(path.read_text()) for b in builds for path in [b / "meson-info" / "intro-compilers.json"]
                if path.exists())["host"]["c"]["exelist"]


def named(file, offset):
    """the function and line the build's symbols give an offset in a module: the function the source has there, not
    what the compiler put into it"""
    said = subprocess.run([WHERE["symbolizer"], f"--obj={file}", "--relative-address", "-f", "-C", "-p", "-s", hex(offset)],
                          capture_output=True, text=True).stdout.strip()
    return said.splitlines()[-1].replace(" (inlined by) ", "") if said else hex(offset)


def where(command, env, process, log):
    """what a test in the Wine that outlived its time leaves behind. in its log: where every thread of its program is
    in Windows, a line a thread (tests/d3d11/where.cpp, which has until it has said nothing for as long as a hung test
    may), and under `stuck:` the innermost frame each thread has in the build's own modules with the number of
    threads there, and the threads that never gave their stack. beside its log: where the threads of its processes
    are in macOS, in the libraries' unix halves and the system's (sample, for as long as it samples when it is not
    told)"""
    if command[:1] != WHERE.get("tool", [])[:1] or len(WHERE["tool"]) < 2:
        return
    pids = subprocess.run(["pgrep", "-g", str(process.pid)], capture_output=True, text=True).stdout.split()
    samples = [subprocess.Popen(["sample", pid, "-file", f"{log}.{pid}.sample"], stdin=subprocess.DEVNULL,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) for pid in pids]
    with open(log, "ab") as out:
        asked = time.time()
        reader = subprocess.Popen(WHERE["tool"] + [Path(command[1]).name], env=env, stdin=subprocess.DEVNULL, stdout=out,
                                  stderr=subprocess.STDOUT, start_new_session=True)
        while reader.poll() is None and time.time() - max(asked, log.stat().st_mtime) < QUIET:
            time.sleep(1)
        if reader.poll() is None:
            os.killpg(reader.pid, signal.SIGKILL)
        reader.wait()
    for sample in samples:
        sample.wait()
    files = dict(WHERE["files"], **{Path(command[1]).name.lower(): command[1]})
    places, text = {}, log.read_text(errors="replace")
    silent = set(" ".join(re.findall(r"^threads: \w+:(.*)$", text, re.M)).split()) - set(re.findall(r"^(?:where|refused): \w+ (\w+):", text, re.M))
    if silent:
        places["gave no stack (" + " ".join(sorted(silent)) + ")"] = len(silent)
    for refusal in re.findall(r"^refused: \w+ \w+: (.*)$", text, re.M):
        places[f"refused by {refusal}"] = places.get(f"refused by {refusal}", 0) + 1
    for frames in re.findall(r"^where: \w+ \w+:(.*)$", text, re.M):
        own = [(module, offset) for module, offset in (frame.rsplit("+", 1) for frame in frames.split()) if module.lower() in files]
        # a thread with no frame in what was built is where its innermost frame is
        place = f"in {own[0][0]}: {named(files[own[0][0].lower()], int(own[0][1], 16))}" if own else "in " + (frames.split() or ["no frame"])[0]
        places[place] = places.get(place, 0) + 1
    with open(log, "a") as out:
        out.write("stuck: " + "; ".join(f"{count} {place}" for place, count in places.items()) + "\n")


def faults(lines):
    return [line for line in lines if any(fault in line for fault in FAULTS)]


def run(command, env, seconds, log):
    """the command's verdict, and what it said last"""
    started = time.monotonic()
    status = finish(command, env, seconds, log)
    lines = [line for line in log.read_text(errors="replace").splitlines() if line.strip()]
    if status is None:
        return "failed", f"still running after {time.monotonic() - started:.0f} s: {lines[-1] if lines else 'nothing'}", time.monotonic() - started
    # a test's result is the last line that starts with one
    said = next((line for line in reversed(lines) if line.split(":")[0] in (*RESULTS, "failed")), "")
    word = said.split(":")[0]
    # a result read while the GPU's work had failed is no result: the readback may hold what an earlier pass left
    if word == "passed" and faults(lines):
        return "failed", f"said '{said}' after: {faults(lines)[0]}", time.monotonic() - started
    if RESULTS.get(word) == status:
        return word, said, time.monotonic() - started
    last = lines[-1] if lines else "nothing"
    return "failed", said if word == "failed" else f"exit status {status} after: {last}", time.monotonic() - started


def arguments(parser):
    """what says where the tests run: the builds, the Wine and its prefix"""
    parser.add_argument("--build", action="append", required=True, type=Path,
                        help="a meson build directory; with several, the first that has a library or the tests gives them")
    parser.add_argument("--wine", required=True, type=Path, help="a Wine's installation: the directory with bin/wine")
    parser.add_argument("--prefix", type=Path, help="the Wine prefix of the tests (made if missing); default: tests-prefix in the first build")
    parser.add_argument("--validate", action="store_true", help="run under Metal's API and shader validation")
    parser.add_argument("--no-install", action="store_true", help="leave the Wine's libraries as they are")


@contextlib.contextmanager
def session(args):
    """keep a Wine's installed libraries unchanged until its tests finish, even between GPU turns"""
    wine = args.wine.resolve().stat()
    directory = Path("/tmp") / f"mullion-tests-{os.getuid()}"
    directory.mkdir(mode=0o700, exist_ok=True)
    if directory.stat().st_uid != os.getuid():
        raise PermissionError(f"test lock directory is not owned by this user: {directory}")
    path = directory / f"wine-{wine.st_dev}-{wine.st_ino}.lock"
    with open(path, "a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        yield environment(args)


def environment(args):
    """the builds, the Wine's binary and the environment of a test in it, with the builds' libraries installed"""
    builds = [build.resolve() for build in args.build]
    wine = args.wine.resolve()
    prefix = (args.prefix or builds[0] / "tests-prefix").resolve()
    unix, pe = libraries(builds)
    base = dict(os.environ, WINEPREFIX=str(prefix), WINEDEBUG=os.environ.get("WINEDEBUG", "-all"), MULLION_SHADER_CACHE="0")
    if getattr(args, "benchmark", False):
        base.pop("NSZombieEnabled", None)
    else:
        base.update(ZOMBIES)
    base.pop("MULLION_GPU_FAMILY", None)
    # the build's libraries in place of the Wine's own, the shader compiler the prefix has (Wine's own when it has
    # none: winetricks' d3dcompiler_47 is x86 only, so an arm64 Wine's ARM64 tests have no other), and no installer or
    # debugger that asks in a dialog
    own = sorted(path.stem for path in pe.values() if builtin(path))
    native = sorted(path.stem for path in pe.values() if not builtin(path))
    overrides = [f"{','.join(native)}=n"] if native else []
    overrides += ["d3dcompiler_47=n,b"] + ([f"{','.join(own)}=b"] if own else []) + ["mscoree,mshtml,winedbg.exe=d"]
    base["WINEDLLOVERRIDES"] = ";".join(overrides)
    if args.validate:
        base.update(VALIDATION)
    binary = wine / "bin" / "wine"
    if not (prefix / "drive_c").exists():
        subprocess.run([binary, "wineboot", "-u"], env=base, stdin=subprocess.DEVNULL, check=True)
    # a display mode a test asks for is Wine's to give (win32u, sysparams.c), never the machine's display's: in every
    # prefix, as one made before the runner set this changes the real display
    subprocess.run([binary, "reg", "add", r"HKCU\Software\Wine\X11 Driver", "/v", "EmulateModeset", "/d", "Y", "/f"],
                   env=base, stdin=subprocess.DEVNULL, check=True)
    if not args.no_install:
        install(wine, prefix, unix, pe)
    WHERE.update(tool=[binary, *sorted(path for build in builds for path in build.glob("tests/*/where.exe"))][:2],
                 files={path.name.lower(): path for path in pe.values()},
                 symbolizer=Path(shutil.which(compiler(builds)[0])).resolve().parent / "llvm-symbolizer")
    return builds, binary, base


def explain(lines, most=6):
    """what a failed test's log says went wrong: the step it was in (tests/trace.hpp), its wrong results, what the
    library said, and Metal's own errors and validation text. at most `most` lines of a kind, each kind once"""
    kinds = {
        "in step": [line[6:] for line in lines if line.startswith("step: ")][-1:],
        "wrong": [line[7:] for line in lines if line.startswith("wrong: ")],
        "library": [line for line in lines if line.startswith(("err:", "warn:")) and not faults([line])],
        "Metal": [line for line in lines if faults([line]) or "-[MTL" in line or "AGXMetal" in line],
    }
    said = []
    for kind, found in kinds.items():
        found = list(dict.fromkeys(found))
        said += [f"{kind}: {line}" for line in found[:most]]
        if len(found) > most:
            said.append(f"{kind}: {len(found) - most} more")
    return said


def signature(said, log):
    """what a result is known by: what the test said, but for the time a hung one ran, and which results were wrong,
    so two failures of one count are two failures"""
    said = re.sub(r"^still running after \d+ s:.*", "still running", said)
    wrong = sorted({line for line in log.read_text(errors="replace").splitlines() if line.startswith("wrong: ")})
    return said + (f" [{hashlib.sha256(chr(10).join(wrong).encode()).hexdigest()[:8]}]" if wrong else "")


def known(path=None):
    """tests/known.txt: a line '<family> <test> <what it says> -- <cause>' for each failure or skip that is known. the
    GPU's own family is 'native(<its chip>)', as another chip is other evidence. only a test that ran to its verdict,
    or was skipped, can be known: one that stopped early (a check, a fault, a hang) hides every case after it"""
    path = path or KNOWN
    entries = {}
    for number, line in enumerate(path.read_text().splitlines() if path.exists() else [], 1):
        if line.strip() and not line.startswith("#"):
            fields = re.fullmatch(r"(native\([^)]*\)|\S+) (\S+) (failed: \d+ wrong results \[[0-9a-f]{8}\]|skipped: .+) -- (.*\S.*)", line)
            if not fields:
                sys.exit(f"{path}:{number}: not '<family> <test> <verdict or skip> -- <cause>'")
            entries[fields[1], fields[2]] = fields[3]
    return entries


def options(parser):
    """what says which of the build's tests run, and how"""
    parser.add_argument("--family", default="native,9,8,7",
                        help="the Apple GPU families to run as, 'native' for the GPU's own (default: %(default)s)")
    parser.add_argument("--repeat", type=int, default=1, help="how many times each test runs, giving the same answer each time")
    parser.add_argument("--jobs", type=int, default=1,
                        help="how many tests run at once; one that fails beside others runs again alone")
    parser.add_argument("--late", type=int, help="delay each Metal command buffer's commit by this many milliseconds")
    parser.add_argument("--suite", action="append", default=[], help="only tests of this suite (may repeat)")
    parser.add_argument("--exact", action="store_true", help="match whole test names instead of prefixes")
    parser.add_argument("--known", type=Path, help="the failures and skips to compare with (default: %s, %s with --validate)" % (KNOWN.name, KNOWN_VALIDATED.name))
    parser.add_argument("--record", type=Path, help="write this run's failures and skips there, in the form --known reads")
    parser.add_argument("--benchmark", action="store_true", help="run declared benchmarks instead of correctness tests")
    parser.add_argument("tests", nargs="*", help="test name prefixes, or whole names with --exact")


def chosen(args):
    """the build's tests the arguments name"""
    declared = [json.loads(path.read_text()) for build in args.build
                for path in [build.resolve() / "meson-info" / ("intro-benchmarks.json" if getattr(args, "benchmark", False) else "intro-tests.json")] if path.exists()]
    declared = next((tests for tests in declared if tests), None)
    if not declared:
        sys.exit("no build was set up with tests (-Denable_tests=true)")
    tests = [t for t in declared
             if (not args.tests or any(t["name"] == name if args.exact else t["name"].startswith(name)
                                       for name in args.tests))
             and (not args.suite or any(suite.split(":")[-1] in args.suite for suite in t["suite"]))]
    unknown = [name for name in args.tests if args.exact and name not in {t["name"] for t in declared}]
    if unknown:
        sys.exit("no such test: " + ", ".join(unknown))
    missing = [t["cmd"][0] for t in tests if not Path(t["cmd"][0]).exists()]
    if missing or not tests:
        sys.exit("not built: " + ", ".join(missing) if missing else "no such test")
    return tests


def own(args, tests, builds, binary, base):
    """runs the build's tests as each family; how many results tests/known.txt does not hold"""
    if args.late is not None and args.late < 0:
        sys.exit("--late must be nonnegative milliseconds")
    logs = builds[0] / "meson-logs" / ("benchmarks" if getattr(args, "benchmark", False) else "tests")
    logs.mkdir(parents=True, exist_ok=True)
    families = args.family.split(",")
    verdicts, said_by = {}, {}
    printing = threading.Lock()

    def one(family, test, again=False):
        env = dict(base) if family == "native" else dict(base, MULLION_GPU_FAMILY=family)
        native = not test["cmd"][0].endswith(".exe")
        command = test["cmd"] if native else [binary] + test["cmd"]
        test_env = dict(env, **test["env"])
        if args.late is not None:
            test_env["MULLION_TEST_LATE"] = str(args.late)
        late = test_env.get("MULLION_TEST_LATE")
        mode = f".late{late}" if late is not None else ""
        seconds, answers = 0.0, {}
        for attempt in range(args.repeat):
            log = logs / f"{test['name']}.{family}{mode}{'.alone' if again else ''}.{attempt}.txt"
            verdict, said, taken = run(command, test_env, test["timeout"], log)
            seconds += taken
            answers.setdefault((verdict, signature(said, log)), said)
        # a test gives one answer on every run: one that gives several has none
        if len(answers) > 1:
            verdict, said = "failed", "unstable: " + " / ".join(known for _, known in answers)
            answers = {(verdict, said): said}
        verdicts[test["name"], family] = verdict
        said_by[test["name"], family] = next(iter(answers))[1]
        lines =[f"{family:>6}  {verdict:7}  {test['name']:32} {seconds:6.1f} s  "
                 f"{'alone ' if again else ''}{f'late={late}ms ' if late is not None else ''}{said}"]
        if verdict == "failed":
            lines += [f"{'':17}{line}" for line in explain(log.read_text(errors="replace").splitlines()) + [f"log: {log}"]]
        with printing:
            print("\n".join(lines), flush=True)

    # a test of the host's own does not meet the GPU. one meson declares not parallel, as one that compares the times
    # its work takes, runs after the others, by itself
    jobs = [(family, test) for family in families for test in tests
            if test["cmd"][0].endswith(".exe") or family == families[0]]
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for done in concurrent.futures.as_completed([pool.submit(one, *job) for job in jobs if job[1]["is_parallel"]]):
            done.result()
    for job in jobs:
        if not job[1]["is_parallel"]:
            one(*job)
    # beside others a test shares the GPU, whose fault in one fails the work of all: each that failed beside others
    # runs again alone, and its verdict is that run's
    if args.jobs > 1:
        again = [(family, test) for family, test in jobs
                 if test["is_parallel"] and verdicts[test["name"], family] == "failed"]
        for family, test in again:
            one(family, test, again=True)
        alone_only = [f"{family} {test['name']}" for family, test in again if verdicts[test["name"], family] != "failed"]
        if alone_only:
            print("\npassed alone after failing beside others: " + ", ".join(alone_only), flush=True)

    width = max(len(test["name"]) for test in tests)
    print("\n" + " " * width + "".join(f"  {family:>7}" for family in families))
    for test in tests:
        print(f"{test['name']:{width}}" + "".join(f"  {verdicts.get((test['name'], family), '-'):>7}" for family in families))
    counts = {word: sum(verdict == word for verdict in verdicts.values()) for word in ("passed", "failed", "skipped")}
    print(f"\n{counts['passed']} passed, {counts['failed']} failed, {counts['skipped']} skipped; logs in {logs}")

    chip = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True).stdout.strip()
    default = KNOWN_VALIDATED if getattr(args, "validate", False) else KNOWN
    path = getattr(args, "known", None) or default
    if path != default and not path.exists():
        sys.exit(f"no such file: {path}")
    listed = known(path)
    ran = {(f"native({chip})" if family == "native" else family, name): (verdict, said_by[name, family])
           for (name, family), verdict in verdicts.items()}
    new = [f"{family} {name} {said}" + (f" (known: {listed[family, name]})" if (family, name) in listed else "")
           for (family, name), (verdict, said) in ran.items() if verdict != "passed" and listed.get((family, name)) != said]
    fixed = [f"{family} {name}" for (family, name) in listed if ran.get((family, name), ("",))[0] == "passed"]
    print(f"{sum(verdict != 'passed' for verdict, _ in ran.values()) - len(new)} known, each with its cause in {path}")
    if getattr(args, "record", None):
        args.record.write_text("".join(f"{family} {name} {said} -- what {builds[0].name} said\n"
                                       for (family, name), (verdict, said) in ran.items() if verdict != "passed"))
    for line in new:
        print(f"new: {line}")
    for line in fixed:
        print(f"passes, still listed as known: {line}")
    return len(new) + len(fixed)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    arguments(parser)
    options(parser)
    args = parser.parse_args()
    tests = chosen(args)
    with session(args) as runtime:
        sys.exit(own(args, tests, *runtime) != 0)


if __name__ == "__main__":
    main()
