
from pathlib import Path
import json


def clean_dataset(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    seen = set()
    with src.open('r', encoding='utf-8') as fin, dst.open('w', encoding='utf-8') as fout:
        for line in fin:
            line = line.strip()
            if not line:
                continue
            obj = json.loads(line)
            key = (obj.get('fen'), obj.get('move'))
            if key in seen:
                continue
            seen.add(key)
            fout.write(json.dumps(obj) + '
')
