
from dataclasses import dataclass

@dataclass
class SprtConfig:
    elo0: float = 0.0
    elo1: float = 5.0
    alpha: float = 0.05
    beta: float = 0.05


def summary(cfg: SprtConfig) -> str:
    return f"SPRT({cfg.elo0}, {cfg.elo1}, alpha={cfg.alpha}, beta={cfg.beta})"
