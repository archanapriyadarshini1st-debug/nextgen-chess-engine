
from dataclasses import dataclass
from pathlib import Path
import json
from collections import Counter

@dataclass
class MistakeSummary:
    opening: int = 0
    tactical: int = 0
    positional: int = 0
    endgame: int = 0
    time_management: int = 0
    search_failure: int = 0
    evaluation_failure: int = 0


def classify(sample: dict) -> str:
    phase = (sample.get('phase') or '').lower()
    if 'opening' in phase:
        return 'opening'
    if 'endgame' in phase:
        return 'endgame'
    if sample.get('blunder', False):
        return 'tactical'
    return 'positional'


def analyze(path: Path) -> Counter:
    counts = Counter()
    with path.open('r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            sample = json.loads(line)
            counts[classify(sample)] += 1
    return counts
