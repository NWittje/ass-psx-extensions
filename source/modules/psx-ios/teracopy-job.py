"""Read installed TeraCopy history without modifying its application databases."""
import json
import os
from pathlib import Path
import sqlite3
import sys


def normal(path):
    return os.path.normcase(os.path.normpath(path))


def inspect(path, target, expected):
    with sqlite3.connect(path.as_uri() + "?mode=ro", uri=True, timeout=0.1) as db:
        db.row_factory = sqlite3.Row
        run = next((r for r in db.execute("SELECT * FROM Run ORDER BY rowid DESC")
                    if normal(r["target"]) == normal(target) and r["mode"] == 1), None)
        if run is None:
            return None
        sources = [r[0] for r in db.execute("SELECT Source FROM Files WHERE IsFolder=0")]
        if sorted(map(normal, sources)) != sorted(map(normal, expected)):
            return None
        counts = dict(db.execute("SELECT t.result,COUNT(*) FROM Target t JOIN Files f ON f.rowid=t.file WHERE f.IsFolder=0 GROUP BY t.result"))
        completion = [r[0] for r in db.execute("SELECT Message FROM Log ORDER BY Timestamp")
                      if r[0].startswith("Copying:")]
        return dict(database=str(path), target=run["target"], started=run["started"],
                    finished=run["finished"], completion=completion, sources=sources,
                    copied=counts.get(2, 0), skipped=counts.get(5, 0),
                    errors=sum(n for code, n in counts.items() if code not in (2, 5)),
                    file_results={str(k): v for k, v in counts.items()})


if __name__ == "__main__":
    expected = Path(sys.argv[2]).read_text(encoding="utf-8-sig").splitlines()
    history = Path(os.environ["APPDATA"]) / "TeraCopy" / "History"
    recent = sorted(history.glob("*.db"), key=lambda p: p.stat().st_mtime, reverse=True)[:128]
    for path in recent:
        try:
            job = inspect(path, sys.argv[1], expected)
            if job is not None:
                print(json.dumps(job))
                sys.exit(0)
        except (sqlite3.Error, OSError):
            continue
    print(json.dumps({"error": "Matching completed-copy history not available yet"}))
    sys.exit(2)
