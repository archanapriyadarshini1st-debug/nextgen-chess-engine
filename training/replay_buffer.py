
from dataclasses import dataclass
from pathlib import Path
from typing import List
import json

@dataclass
class Sample:
    fen: str
    move: str
    eval_cp: int
    depth: int
    pv: List[str]
    result: str


def append_sample(path: Path, sample: Sample) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('a', encoding='utf-8') as f:
        f.write(json.dumps(sample.__dict__) + "
")
