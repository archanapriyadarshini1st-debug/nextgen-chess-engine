
from pathlib import Path
import json

ROOT = Path(__file__).resolve().parents[1]
DATA = ROOT / "datasets"
MODELS = ROOT / "networks"


def main() -> None:
    print(json.dumps({
        "root": str(ROOT),
        "datasets": str(DATA),
        "models": str(MODELS),
        "status": "training scaffold ready",
    }, indent=2))


if __name__ == "__main__":
    main()
