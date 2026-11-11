#!/usr/bin/env python3
"""A clean bootstrap world improves itself, on a real Model, and the person checks it from outside.
   scripts/test-bootstrap.py <scratch-dir> [--port 7811] [--record <dir holding one ChatGPT record; the test's own, written back>]
                             [--model gpt-5.6-sol] [--task view-refs-test] [--budget 1800]

What happens, in order:
1. a fresh data dir: a release of this repository's HEAD, a supervisor, a world; first good
2. the person adopts a signed-in ChatGPT record into a new Model `chatgpt`, probes it, and puts root on it
3. root gets rules for reads and tests; every other command root asks for is a card the person decides
   here, by a policy the report lists in full
4. the person asks root for a real change to ORBIS, with its acceptance: fix, test, commit, release.make,
   world.restart; the kernel swap is always the person's own card, and this script approves it
5. the world goes and comes back on root's release; root's wake resumes and reports
6. checks from outside: release.good is root's commit; src HEAD is that commit; the behaviour the task
   asked for holds on the new world and did not before; root ran tests before releasing; its reply
   cites the commit; its memory/notes carry the release

Nothing here touches another world. Standard library only; the JSON protocol through scripts/bench/ws.py."""
import argparse, json, os, re, shutil, signal, stat, subprocess, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "bench"))
from ws import World

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# ---------------------------------------------------------------- the tasks a bootstrap can be asked for

TASKS = {
    # the open punch-list item, 2026-11-05: a person's project.add fails "no Model" even with `model`
    "project-add-model": {
        "text": (
            "A real change to ORBIS, in your working copy `src`.\n\n"
            "Bug: a person calling `project.add` gets `no Model: give \\`model\\` or bind one to yourself first`, "
            "even when they pass `model`. Two causes in `world/src/knowledge/world/act/`: the `project.add` schema "
            "in actions.clj has no `model`, and the Principal a project gets (`adopt-project!` in handlers.clj, "
            "through `principal.create`) takes its Model from the caller, which a person does not have.\n\n"
            "Fix: `project.add` takes an optional `model` (a Model name or id) and passes it through to "
            "`principal.create`; when the caller has no Model and gives none, the new Principal gets root's first "
            "Model. A person can then add a project from a client. Add a focused unit test. Run the tests that "
            "cover it (`make test-unit` or a kaocha --focus on the test namespace) and fix what fails.\n\n"
            "Then: `git add -A && git commit` with a one-line message, `release.make`, and `world.restart` onto that "
            "release with a reason. Your wake resumes after the restart: read `world/release` on the world, confirm "
            "it is your commit, and reply with the commit, the files you changed, the test you ran and its result, "
            "and what you saw after the restart. Claim nothing you did not run. Keep memory/notes current."
        ),
        "expect_files": ["world/src/knowledge/world/act/actions.clj", "world/src/knowledge/world/act/handlers.clj"],
        # fixed on master 2026-11-10 (052c7fb, root's own commit from this test): `before` refuses to run it again
        "before": lambda w, scratch: not project_add_as_person(w, check_dir(scratch, "check-before"), "check-before", "chatgpt").get("ok"),
        "after": lambda w, scratch, run, changed: bool(project_add_as_person(w, check_dir(scratch, "check-after"), "check-after", "chatgpt").get("ok")),
        "after_text": "a person's project.add with model succeeds",
    },
    # punch list: `title-refs` and `data-refs` in views have no test
    "view-refs-test": {
        "text": (
            "A real change to ORBIS, in your working copy `src.fs`.\n\n"
            "`title-refs` and `data-refs` in views (`world/src/knowledge/world/view/`) have no test. Read how they are "
            "computed and where they are used, then add one focused unit test namespace under `world/test/` that "
            "exercises both: a view whose title and data carry references, the refs the world records for each, and "
            "that removing the behaviour would fail it. Run it with `clojure -M:test --focus <your namespace>` in `src/world` "
            "and fix what fails.\n\n"
            "Then: `git add -A && git commit` with a one-line message, `release.make`, and `world.restart` onto that "
            "release with a reason. Your wake resumes after the restart: read `world/release` on the world, confirm "
            "it is your commit, and reply with the commit, the files you changed, the test you ran and its result, "
            "and what you saw after the restart. Claim nothing you did not run. Keep memory/notes current."
        ),
        "expect_files": [],
        "before": lambda w, scratch: not any("title-refs" in open(os.path.join(r, f)).read()
                                             for r, _, fs in os.walk(os.path.join(REPO, "world", "test")) for f in fs if f.endswith(".clj")),
        "after": lambda w, scratch, run, changed: any(f.startswith("world/test/") and f.endswith(".clj") for f in changed),
        "after_text": "the commit adds a test under world/test",
    },
}

def check_dir(scratch, name):
    d = os.path.join(scratch, name); os.makedirs(d, exist_ok=True)
    open(os.path.join(d, "README.md"), "w").write("# check\nA directory a person adds as a project.\n")
    return d

# ---------------------------------------------------------------- the person's policy for root's cards

SAFE_PROGRAMS = {"git", "make", "clojure", "clj", "cmake", "ctest", "ls", "cat", "grep", "rg", "sed", "find", "head",
                 "tail", "wc", "diff", "pwd", "echo", "true", "test", "awk", "sort", "uniq", "tr", "cut", "stat", "mkdir", "touch"}
NEVER = re.compile(r"\b(sudo|ssh|scp|curl|wget|pip3?|brew|kill|pkill|chmod|chown|rm\s+-rf?\s+/|launchctl|open\s)\b")

def policy(scratch, f):
    """Approve a command root asks for when it is a build, test or git step inside the test world; deny the
    rest. Returns (ok, why). The report lists every decision."""
    op = f.get("task/op") or {}
    args = (op.get("args") or {}) if isinstance(op, dict) else {}
    inner = args.get("args") or args
    a = str(args.get("action") or op.get("op") or f.get("task/summary") or "")
    summary = str(f.get("task/summary") or "")
    if summary.startswith("replace the running kernel") or a == "world.restart":
        return True, "the kernel swap: the person's own card, approved for this test"
    cmd = str(inner.get("cmd") or "")
    if not cmd:
        return False, "not a command: " + summary[:80]
    if NEVER.search(cmd):
        return False, "refused by policy: " + cmd[:80]
    for p in re.findall(r"(?<![\w/.-])/[\w./-]+", cmd):  # any absolute path must be inside the test world
        if not p.startswith(scratch):
            return False, "a path outside the test world: " + p
    programs = {s.strip().split()[0] for s in re.split(r"&&|\|\||;|\|", cmd) if s.strip()}
    bad = programs - SAFE_PROGRAMS
    if bad:
        return False, "program not in the allow-list: " + " ".join(sorted(bad))
    return True, "a build, test or git step in the working copy"

# ---------------------------------------------------------------- helpers

def say(s): print(s, flush=True)

def check(what, ok, soft=False):
    say(("ok   " if ok else ("soft " if soft else "FAIL ")) + what)
    if not ok and not soft:
        raise SystemExit(1)
    return ok

def wait(what, pred, seconds=300, every=1.0):
    end = time.time() + seconds
    while time.time() < end:
        try:
            v = pred()
            if v:
                return v
        except (OSError, EOFError, ValueError, KeyError, RuntimeError):
            pass
        time.sleep(every)
    raise SystemExit(f"FAILED waiting for {what}")

def log_of(D):
    try: return open(f"{D}/logs/supervise.log").read()
    except OSError: return ""

def connect(D, port):
    return World(token_file=f"{D}/person.token", port=port, kind="bootstrap-test")

def text_of(fields):
    t = fields.get("message/text")
    return t if isinstance(t, str) else json.dumps(t)

def is_reply(e, p):
    b = e.get("event/body") or {}; f = b.get("fields") or {}
    return e.get("event/kind") == "created" and b.get("class") == "message" and f.get("message/from") == p \
        and str(f.get("message/kind") or "").strip(":") in ("", "None", "reply", "text")

def inner(r):
    """An act's answer: the handler's result sits inside the response's result."""
    res = r.get("result") or {}
    return res.get("result") if isinstance(res.get("result"), dict) else res

def sh(*argv, **kw):
    return subprocess.check_output(list(argv), text=True, **kw).strip()

# ---------------------------------------------------------------- the person's side of root's wake

class Person:
    def __init__(self, D, port, scratch, decided):
        self.D, self.port, self.scratch, self.decided = D, port, scratch, decided
        self.w = connect(D, port)
        self.root = self.w.resolve("root"); self.route = self.w.resolve("root.route")

    def reconnect(self):
        self.w = wait("the world to answer", lambda: connect(self.D, self.port), 300)

    def cards(self, since, seen):
        """Decide every open card root asked for since seq `since`. Returns the seq reached, and whether a
        kernel swap was approved (the world is about to go)."""
        swapped = False
        for e in self.w.events(since):
            since = max(since, e.get("event/seq", 0))
            b = e.get("event/body") or {}
            if e.get("event/kind") == "created" and b.get("class") == "task":
                t = e.get("event/stream")
                if t in seen: continue
                for _ in range(10):
                    f = self.w.fields(t)
                    if f.get("task/requested-by"): break
                    time.sleep(0.3)
                if f.get("task/requested-by") != self.root or str(f.get("task/kind") or "").strip(":") != "approval":
                    continue
                if f.get("task/rule"):
                    continue
                if str(f.get("task/state") or "").strip(":") not in ("pending", "open", ""):
                    seen.add(t); continue
                seen.add(t)
                ok, why = policy(self.scratch, f)
                self.decided.append({"summary": f.get("task/summary"), "approved": ok, "why": why, "seq": e.get("event/seq")})
                say(("     approve " if ok else "     deny    ") + str(f.get("task/summary"))[:110])
                try:
                    self.w.act("task.approve" if ok else "task.deny", {"task": t, **({} if ok else {"reason": why})})
                except (OSError, EOFError):
                    pass  # the world went: a kernel swap
                if ok and str(f.get("task/summary") or "").startswith("replace the running kernel"):
                    swapped = True
        return since, swapped

    def ask(self, text, budget, quiet_s=10, idle_s=240):
        """Send root a Message, decide its cards, follow it through a restart, and return what happened."""
        w = self.w; start = w.head(); since = start; seen = set(); t0 = time.time(); waited = 0.0
        r = w.call("send", {"to": [self.root], "route": self.route, "text": text})
        if not r.get("ok"): raise SystemExit("send failed: " + str(r.get("err")))
        last_busy = time.time(); restarts = []
        while time.time() - t0 - waited < budget:
            tick = time.time(); time.sleep(1.5)
            try:
                since, swapped = self.cards(since, seen)
                if swapped:
                    say("     waiting for the world to go and come back on root's release")
                    before = log_of(self.D).count("healthy")
                    wait("the restart", lambda: "restart to" in log_of(self.D).split("healthy")[-1] or log_of(self.D).count("healthy") > before, 300)
                    wait("the return", lambda: log_of(self.D).count("healthy") > before, 600, 2)
                    self.reconnect(); w = self.w
                    restarts.append({"at": time.time() - t0, "release": open(f"{self.D}/release").read().strip()})
                    waited += time.time() - tick
                    last_busy = time.time()
                    continue
                f = w.fields(self.root)
                busy = bool(f.get("loop/iteration")) or bool(f.get("loop/queue"))
                if self.running_task(start): waited += time.time() - tick
                if busy: last_busy = time.time()
                replied = any(is_reply(e, self.root) for e in w.events(start))
                if not busy and replied and time.time() - last_busy > quiet_s: break
                if not busy and time.time() - last_busy > idle_s:
                    say(f"     root idle for {idle_s} s with no reply: giving up on this wake"); break
            except (OSError, EOFError):
                say("     connection lost; reconnecting")
                self.reconnect(); w = self.w
        wall = time.time() - t0
        ev = w.events(start); after = w.fields(self.root)
        out = {"wall": round(wall, 1), "worked": round(wall - waited, 1), "timed_out": wall - waited >= budget,
               "restarts": restarts, "replies": [], "ops": [], "stalled": after.get("loop/stalled"),
               "evals": after.get("cost/evaluations"), "tokens_in": after.get("cost/tokens-in"), "tokens_out": after.get("cost/tokens-out"),
               "start_seq": start}
        for e in ev:
            b = e.get("event/body") or {}
            if b.get("name") == "evaluation/results":
                for x in b.get("value") or []:
                    out["ops"].append({"seq": e.get("event/seq"), "op": x.get("op"), "ok": x.get("ok"), "args": json.dumps(x.get("args"))[:400],
                                       "err": x.get("err"), "result": json.dumps(x.get("result"))[:1200]})
            if is_reply(e, self.root):
                out["replies"].append({"seq": e.get("event/seq"), "text": text_of(b.get("fields") or {})})
        return out

    def running_task(self, start):
        moved = {}
        for e in self.w.events(start):
            if str(e.get("event/kind")).strip(":") == "task-moved":
                b = e.get("event/body") or {}
                moved[b.get("task")] = str(b.get("to")).strip(":")
        return any(v == "active" for v in moved.values())

# ---------------------------------------------------------------- the ChatGPT record

STORE = {}  # the record store the test owns: the refreshed record goes back there, since the refresh token rotates

def give_back_record():
    """The world's copy refreshed the token; the store must hold that copy or the next run is signed out."""
    if STORE.get("copy") and os.path.isfile(STORE["copy"]):
        shutil.copyfile(STORE["copy"], STORE["src"]); os.chmod(STORE["src"], stat.S_IRUSR | stat.S_IWUSR)
        say("     the refreshed ChatGPT record went back to " + STORE["src"])

def adopt_chatgpt(w, D, record_dir, model_id):
    """A Model `chatgpt` on the plan, from a record a stopped world signed in: the Model Object, the record
    under its id, model/api on the plan route, a probe. The person owns both the world and the record."""
    r = w.act("model.create", {"name": "chatgpt", "url": "https://api.openai.com/v1", "model-id": model_id,
                               "label": "ChatGPT plan", "reasoning": "off"})
    if not r.get("ok"): raise SystemExit("model.create: " + str(r.get("err")))
    m = inner(r)["model"]
    src = [f for f in os.listdir(record_dir) if f.endswith(".json")]
    if len(src) != 1: raise SystemExit(f"{record_dir} holds {len(src)} records; give the one")
    dest = os.path.join(D, "credentials", "chatgpt"); os.makedirs(dest, exist_ok=True)
    for a, b in ((src[0], m + ".json"), ("host-id", "host-id")):
        shutil.copyfile(os.path.join(record_dir, a), os.path.join(dest, b))
        os.chmod(os.path.join(dest, b), stat.S_IRUSR | stat.S_IWUSR)
    STORE["src"] = os.path.join(record_dir, src[0]); STORE["copy"] = os.path.join(dest, m + ".json")
    r = w.call("set-field", {"object": m, "field": "model/api", "type": "keyword", "value": "chatgpt-plan"})
    if not r.get("ok"): raise SystemExit("model/api: " + str(r.get("err")))
    p = w.act("model.probe", {"model": m})
    return m, inner(p)

# ---------------------------------------------------------------- the task's own checks

def project_add_as_person(w, path, name, model):
    return w.act("project.add", {"name": name, "path": path, "charter": "A bootstrap check: a person adds a project. Do nothing.", "model": model})

# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scratch"); ap.add_argument("--port", type=int, default=7811)
    ap.add_argument("--record", default=os.path.expanduser("~/.orbis/bootstrap-record"),
                    help="a dir with one ChatGPT sign-in record (<id>.json and host-id), copied from a stopped world once; the test owns it and writes the refreshed record back")
    ap.add_argument("--model", default="gpt-5.6-sol"); ap.add_argument("--task", default="view-refs-test")
    ap.add_argument("--budget", type=int, default=1800); ap.add_argument("--keep", action="store_true", help="leave the world running")
    a = ap.parse_args()
    scratch = os.path.abspath(a.scratch); os.makedirs(scratch, exist_ok=True)
    D = os.path.join(scratch, "world"); port = a.port
    if os.path.exists(D):
        subprocess.call(["chmod", "-R", "u+w", D]); shutil.rmtree(D)
    os.makedirs(D)
    results = {"task": a.task, "model": a.model, "decided": [], "checks": []}
    decided = results["decided"]
    task = TASKS[a.task]

    # 1. a fresh supervised world from this repository's HEAD
    first = sh(f"{HERE}/release.sh", "--data", D, "--repo", REPO, "--rev", "HEAD")
    with open(os.path.join(D, "world.env"), "w") as f:  # the boot Model's key, as world-sky.sh hands it over
        os.chmod(f.name, 0o600)
        for line in open(os.path.join(REPO, ".envrc")):
            if line.startswith("export KE_MODEL_KEY="): f.write(line)
    env = dict(os.environ, KE_PORT=str(port), KE_MODEL_URL="http://localhost:8000/v1", KE_MODEL_ID="your-model",
               KE_SETTLE_S="10", KE_HEALTH_S="120", KE_WORLD_NAME="bootstrap-test", KE_PUBLIC_URL=f"ws://192.0.2.10:{port}/ws")
    env.pop("KE_DATA", None)
    sup = subprocess.Popen([f"{HERE}/supervise.sh", D], env=env, stdout=subprocess.DEVNULL)
    try:
        say(f"world {D} on {port}, release {first[:12]}")
        wait("first start", lambda: connect(D, port), 300)
        wait("first good", lambda: os.path.exists(f"{D}/release.good"), 120)
        check("a fresh supervised world runs from a release of HEAD", open(f"{D}/release.good").read().strip() == first)

        # 2. root on the real Model
        w = connect(D, port)
        m, probe = adopt_chatgpt(w, D, a.record, a.model)
        check(f"the ChatGPT plan Model probes ok: {probe.get('status')} ({len(probe.get('models') or [])} models)", probe.get("status") == "ok")
        check(f"{a.model} is among them", a.model in (probe.get("models") or []))
        root = w.resolve("root")
        r = w.act("model.choose", {"principal": root, "model": m, "id": a.model})
        check("root is put on it", r.get("ok"))
        c = inner(w.act("model.chosen", {"principal": root}))
        check("model.chosen agrees: " + json.dumps(c)[:120], a.model in json.dumps(c))

        # 3. the person's rules: reads and tests run; the rest is a card decided by `policy`
        for action in ("task.run", "shell"):
            for preset in ("read-only", "test"):
                r = w.act("approval.rule.add", {"principal": "root", "action": action, "match": {"preset": preset}})
                if not r.get("ok"): say("     rule " + action + " " + preset + ": " + str(r.get("err")))
        say("     rules: read-only and test commands run without a card")

        # the behaviour before: the task's defect is there on this checkout, or there is nothing to improve
        if not task["before"](w, scratch):
            say(f"STOP task {a.task} is already done on this checkout: give --task one of " + ", ".join(TASKS))
            raise SystemExit(2)
        check(f"before: the task's defect is there ({a.task})", True)

        # 4. root does the work; the person decides cards and the kernel swap
        say("asking root; budget " + str(a.budget) + " s of the model's time")
        person = Person(D, port, scratch, decided)
        run = person.ask(task["text"], a.budget)
        results["run"] = run
        say(f"     wall {run['wall']} s, worked {run['worked']} s, restarts {len(run['restarts'])}, ops {len(run['ops'])}, replies {len(run['replies'])}")
        check("root replied", bool(run["replies"]))
        check("the world restarted during the wake", bool(run["restarts"]))

        # 5. from outside
        w = connect(D, port)
        release = open(f"{D}/release").read().strip(); good = open(f"{D}/release.good").read().strip()
        src_head = sh("git", "-C", f"{D}/src", "rev-parse", "HEAD")
        check(f"the world runs root's release {release[:12]} and it is good", release != first and release == good)
        check("src HEAD is that commit", src_head == release)
        changed = sh("git", "-C", f"{D}/src", "diff", "--name-only", f"{first}..{release}").split()
        results["changed"] = changed
        check("the commit changes the files the task names: " + " ".join(changed), all(f in changed for f in task["expect_files"]))
        tests = [o for o in run["ops"] if o["op"] in ("act",) and "task.run" in o["args"] and re.search(r"make test|-M:test|kaocha|ctest", o["args"])]
        check(f"root ran tests before releasing ({len(tests)} test runs)", bool(tests))
        rel = [o for o in run["ops"] if "release.make" in o["args"]]
        check("root made the release itself", any(o["ok"] for o in rel))
        reply = run["replies"][-1]["text"]
        check("the reply names the commit", release[:7] in reply)
        check("the reply came after the restart (the wake resumed)", run["replies"][-1]["seq"] > run["start_seq"] and bool(run["restarts"]))
        notes = str(w.fields(root).get("memory/notes") or "")
        results["notes"] = notes
        check("memory/notes carry the release (the lesson stays)", release[:7] in notes or "release" in notes.lower(), soft=True)
        # the behaviour after: the improvement holds on the new world
        check("after: " + task["after_text"] + " (the improvement holds)", bool(task["after"](w, scratch, run, changed)))
        denied = [d for d in decided if not d["approved"]]
        say(f"     cards: {len(decided)} decided, {len(denied)} denied")
        say("all good")
    finally:
        give_back_record()
        with open(os.path.join(scratch, "results.json"), "w") as f:
            json.dump(results, f, indent=1, default=str)
        say("results " + os.path.join(scratch, "results.json"))
        if not a.keep:
            sup.send_signal(signal.SIGTERM)
            try: sup.wait(60)
            except subprocess.TimeoutExpired: sup.kill()
        else:
            say(f"world left running: {D} on {port}; stop with scripts/supervise.sh --stop {D}")

if __name__ == "__main__":
    main()
