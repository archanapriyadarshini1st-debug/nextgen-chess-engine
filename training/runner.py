from pathlib import Path


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    print(f"training runner placeholder at {root}")


if __name__ == "__main__":
    main()
