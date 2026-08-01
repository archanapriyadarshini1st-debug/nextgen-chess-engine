#!/usr/bin/env python3
# SPRT + a real cutechess-cli driver.
#
# The old tuning/spsa.py never played a game: evaluate_params() returned a win
# rate derived from the euclidean distance to a hardcoded optimal dict, so SPSA
# was descending a synthetic bowl. Everything here shells out to cutechess-cli.
import math, os, re, shutil, subprocess
from dataclasses import dataclass, field


def elo_to_score(elo):
    return 1.0 / (1.0 + 10 ** (-elo / 400.0))


def score_to_elo(score):
    score = min(max(score, 1e-6), 1 - 1e-6)
    return -400.0 * math.log10(1.0 / score - 1.0)


def llr(w, l, d, elo0=0.0, elo1=5.0):
    """Log likelihood ratio of H1(elo1) vs H0(elo0), trinomial model."""
    n = w + l + d
    if n == 0 or w + l == 0:
        return 0.0
    pw, pl, pd = w / n, l / n, d / n
    score = pw + 0.5 * pd
    var = pw * (1 - score) ** 2 + pd * (0.5 - score) ** 2 + pl * score ** 2
    if var <= 0:
        return 0.0
    s0, s1 = elo_to_score(elo0), elo_to_score(elo1)
    return (s1 - s0) * (2 * score - s0 - s1) / (2 * var / n)


def sprt_bounds(alpha=0.05, beta=0.05):
    return math.log(beta / (1 - alpha)), math.log((1 - beta) / alpha)


def sprt_verdict(w, l, d, elo0=0.0, elo1=5.0, alpha=0.05, beta=0.05):
    lo, hi = sprt_bounds(alpha, beta)
    v = llr(w, l, d, elo0, elo1)
    if v >= hi:
        return "accept", v
    if v <= lo:
        return "reject", v
    return "continue", v


def elo_with_error(w, l, d):
    n = w + l + d
    if n == 0:
        return 0.0, 0.0
    score = (w + 0.5 * d) / n
    pw, pl, pd = w / n, l / n, d / n
    var = pw * (1 - score) ** 2 + pd * (0.5 - score) ** 2 + pl * score ** 2
    sd = math.sqrt(var / n) if var > 0 else 0.0
    lo, hi = score_to_elo(score - 1.96 * sd), score_to_elo(score + 1.96 * sd)
    return score_to_elo(score), (hi - lo) / 2.0


@dataclass
class MatchResult:
    wins: int = 0
    losses: int = 0
    draws: int = 0
    crashes: int = 0
    timeouts: int = 0
    raw: str = ""

    @property
    def games(self):
        return self.wins + self.losses + self.draws

    @property
    def score(self):
        return (self.wins + 0.5 * self.draws) / self.games if self.games else 0.5

    def __str__(self):
        elo, err = elo_with_error(self.wins, self.losses, self.draws)
        extra = ", %d crashes" % self.crashes if self.crashes else ""
        return "%dW-%dL-%dD (%d games, %+.1f +/- %.1f Elo%s)" % (
            self.wins, self.losses, self.draws, self.games, elo, err, extra)


@dataclass
class EngineSpec:
    cmd: str
    name: str
    options: dict = field(default_factory=dict)

    def args(self):
        a = ["-engine", "cmd=" + self.cmd, "name=" + self.name, "proto=uci"]
        a += ["option.%s=%s" % (k, v) for k, v in self.options.items()]
        return a


SCORE_RE = re.compile(r"Score of .*?:\s*(\d+)\s*-\s*(\d+)\s*-\s*(\d+)")
CRASH_RE = re.compile(r"(terminated|stalled|does not respond|illegal move|crash)", re.I)


def find_cutechess():
    for name in ("cutechess-cli", "cutechess"):
        p = shutil.which(name)
        if p:
            return p
    env = os.environ.get("CUTECHESS_CLI")
    if env and os.path.exists(env):
        return env
    raise RuntimeError("cutechess-cli not found; set CUTECHESS_CLI=/path/to/cutechess-cli")


def play_match(a, b, games=200, tc="10+0.1", concurrency=1, openings=None,
               hash_mb=64, threads=1, timeout=None, pgnout=None):
    """Run a real cutechess-cli match. Result is from A point of view."""
    cc = find_cutechess()
    rounds = max(1, games // 2)
    for e in (a, b):
        e.options.setdefault("Hash", hash_mb)
        e.options.setdefault("Threads", threads)

    cmd = [cc] + a.args() + b.args() + [
        "-each", "tc=" + tc, "proto=uci",
        "-games", "2", "-rounds", str(rounds), "-repeat",
        "-concurrency", str(concurrency),
        "-resign", "movecount=4", "score=700",
        "-draw", "movenumber=40", "movecount=8", "score=10",
        "-recover", "-ratinginterval", "0",
    ]
    if openings:
        fmt = "epd" if str(openings).endswith(".epd") else "pgn"
        cmd += ["-openings", "file=" + str(openings), "format=" + fmt, "order=random"]
    if pgnout:
        cmd += ["-pgnout", pgnout]

    res = MatchResult()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True,
                              timeout=timeout or (games * 60))
        out = proc.stdout + proc.stderr
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or "") + (e.stderr or "")
        res.timeouts += 1
    except Exception as e:
        return MatchResult(raw="cutechess failed: %s" % e)

    res.raw = out[-4000:]
    res.crashes = len(CRASH_RE.findall(out))
    found = SCORE_RE.findall(out)
    if found:
        w, l, d = found[-1]
        res.wins, res.losses, res.draws = int(w), int(l), int(d)
    return res


def run_sprt(a, b, elo0=0.0, elo1=5.0, alpha=0.05, beta=0.05,
             batch=200, max_games=40000, **kw):
    """Play in batches until SPRT accepts, rejects, or we run out of games."""
    total = MatchResult()
    while total.games < max_games:
        r = play_match(a, b, games=batch, **kw)
        if r.games == 0:
            print("  no games completed - aborting")
            print(r.raw[-800:])
            break
        total.wins += r.wins
        total.losses += r.losses
        total.draws += r.draws
        total.crashes += r.crashes
        verdict, v = sprt_verdict(total.wins, total.losses, total.draws, elo0, elo1, alpha, beta)
        lo, hi = sprt_bounds(alpha, beta)
        print("  %s  LLR %+.2f [%.2f, %.2f] -> %s" % (total, v, lo, hi, verdict))
        if verdict != "continue":
            return verdict, total
    return "inconclusive", total


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description="SPRT two engines against each other")
    ap.add_argument("--engine", required=True)
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--tc", default="10+0.1")
    ap.add_argument("--elo0", type=float, default=0.0)
    ap.add_argument("--elo1", type=float, default=5.0)
    ap.add_argument("--batch", type=int, default=200)
    ap.add_argument("--max-games", type=int, default=40000)
    ap.add_argument("--concurrency", type=int, default=1)
    ap.add_argument("--openings", default=None)
    args = ap.parse_args()

    verdict, total = run_sprt(
        EngineSpec(args.engine, "new"),
        EngineSpec(args.baseline, "base"),
        elo0=args.elo0, elo1=args.elo1, batch=args.batch, max_games=args.max_games,
        tc=args.tc, concurrency=args.concurrency, openings=args.openings)
    print("\nSPRT %s: %s" % (verdict, total))
    raise SystemExit(0 if verdict == "accept" else 1)
