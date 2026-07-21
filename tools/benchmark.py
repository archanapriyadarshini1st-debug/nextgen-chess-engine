
from pathlib import Path
import json

REPORT = Path(__file__).resolve().parents[1] / 'benchmarks' / 'report.json'


def main() -> None:
    REPORT.parent.mkdir(parents=True, exist_ok=True)
    data = {
        'status': 'benchmark scaffold',
        'targets': ['Stockfish', 'Lc0', 'previous versions'],
        'metrics': ['elo', 'nodes_per_second', 'win_rate', 'blunder_rate'],
    }
    REPORT.write_text(json.dumps(data, indent=2), encoding='utf-8')
    print(REPORT)


if __name__ == '__main__':
    main()
