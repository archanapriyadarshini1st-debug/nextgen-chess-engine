
from pathlib import Path


def build_command(engine_a: Path, engine_b: Path, rounds: int = 2) -> str:
    return f"cutechess-cli -engine cmd={engine_a} -engine cmd={engine_b} -rounds {rounds}"
