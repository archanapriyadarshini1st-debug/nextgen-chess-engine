
from pathlib import Path
import json

OUT = Path(__file__).resolve().parents[1] / 'datasets' / 'games' / 'selfplay.jsonl'


def main() -> None:
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open('a', encoding='utf-8') as f:
        f.write(json.dumps({'status': 'selfplay scaffold', 'note': 'wire engine binary here'}) + '
')
    print(OUT)


if __name__ == '__main__':
    main()
