
from pathlib import Path
import json


def load_report(path: Path):
    return json.loads(path.read_text(encoding='utf-8'))
