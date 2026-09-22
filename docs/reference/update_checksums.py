"""Regenerate SHA256SUMS for the EEPROM archive, including provenance files."""
from hashlib import sha256
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def main():
    paths = sorted(path for path in ROOT.rglob('*') if path.is_file()
                   and path.name != 'SHA256SUMS'
                   and '__pycache__' not in path.parts)
    records = [f'{sha256(path.read_bytes()).hexdigest()}  {path.relative_to(ROOT).as_posix()}' for path in paths]
    (ROOT / 'SHA256SUMS').write_text('\n'.join(records) + '\n', encoding='utf-8')
    print(f'SHA256SUMS covers {len(records)} files')


if __name__ == '__main__':
    main()
