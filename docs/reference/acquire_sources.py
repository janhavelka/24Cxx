"""Inventory selected manufacturer repositories and archive pinned references.

Requires requests. Only EEPROM-specific examples/drivers and their licenses are
downloaded. It does not clone entire vendor organizations or claim exhaustive
code-search coverage. GitHub API unauthenticated rate limits may apply.
"""
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from hashlib import sha256
from pathlib import Path
from urllib.parse import quote
import json
import requests

ROOT = Path(__file__).resolve().parent
REPOSITORIES = [
    dict(repo="STMicroelectronics/stm32-m24256", ref="main", name="st-m24256", paths=[
        "README.md", "LICENSE.md", "m24256.c", "m24256.h"]),
    dict(repo="STMicroelectronics/X-CUBE-EEPRMA1", ref="main", name="st-eeprma1", paths=[
        "README.md", "LICENSE.md", "Drivers/BSP/Components/M24xx/LICENSE.md",
        "Drivers/BSP/Components/M24xx/m24xx.c", "Drivers/BSP/Components/M24xx/m24xx.h",
        "Utilities/PC_Software/AT_Commands/README.md", "Utilities/PC_Software/AT_Commands/LICENSE.md",
        "Utilities/PC_Software/AT_Commands/readme.txt"]),
    dict(repo="Microchip-MPLAB-Harmony/core", ref="master", name="microchip-harmony", paths=[
        "License.md", "driver/i2c_eeprom/at24/drv_at24.h", "driver/i2c_eeprom/at24/drv_at24_definitions.h",
        "driver/i2c_eeprom/at24/src/drv_at24.c.ftl", "driver/i2c_eeprom/at24/src/drv_at24_local.h",
        "driver/i2c_eeprom/at24/config/drv_at24.py"]),
    dict(repo="microchip-pic-avr-examples/dspic33ck-curiosity-i2c-eeprom-demo", ref="main", name="microchip-24c08", paths=[
        "README.md", "LICENSE.txt", "dspic33ck-curiosity-i2c-eeprom-demo.X/eeprom/eeprom_24c08.c",
        "dspic33ck-curiosity-i2c-eeprom-demo.X/eeprom/eeprom_24c08.h",
        "dspic33ck-curiosity-i2c-eeprom-demo.X/eeprom/eeprom_24c08_config.h",
        "dspic33ck-curiosity-i2c-eeprom-demo.X/eeprom/eeprom_24c08_types.h"]),
    dict(repo="renesas-rcar/linux-bsp", ref="v6.12.80/rcar-6.1.0", name="renesas-linux", paths=[
        "COPYING", "LICENSES/preferred/GPL-2.0", "drivers/misc/eeprom/at24.c",
        "Documentation/devicetree/bindings/eeprom/at24.yaml"]),
]
SEARCH_QUERIES = [
    'org:STMicroelectronics eeprom',
    'org:Microchip-MPLAB-Harmony eeprom',
    'org:microchip-pic-avr-examples eeprom i2c',
    'org:renesas eeprom',
    'org:RohmSemiconductor eeprom',
    'Zetta ZD24',
    'Giantec EEPROM',
    'onsemi CAT24',
]


def get_json(url, **kwargs):
    response = requests.get(url, timeout=40, **kwargs)
    response.raise_for_status()
    return response.json()


def archive(repo):
    record = dict(repo=repo['repo'], requested_ref=repo['ref'], artifacts=[],
                  checked_utc=datetime.now(timezone.utc).isoformat())
    try:
        commit = get_json(f'https://api.github.com/repos/{repo["repo"]}/commits/{quote(repo["ref"], safe="")}')
        sha = commit['sha']
        record.update(commit=sha, commit_date=commit['commit']['committer']['date'])
        # Large Linux tree can be truncated. Check the EEPROM subtree instead.
        if repo['name'] == 'renesas-linux':
            tree_sha = get_json(f'https://api.github.com/repos/{repo["repo"]}/contents/drivers/misc/eeprom?ref={sha}')
            record['matched_tree_paths'] = [t['path'] for t in tree_sha]
            record['tree_scope'] = 'drivers/misc/eeprom (contents API)'
        else:
            tree = get_json(f'https://api.github.com/repos/{repo["repo"]}/git/trees/{sha}?recursive=1')
            record['tree_truncated'] = tree.get('truncated')
            record['tree_scope'] = 'recursive repository tree; filename search only'
            record['matched_tree_paths'] = [t['path'] for t in tree.get('tree', []) if t['type'] == 'blob' and
                any(key in t['path'].lower() for key in ['at24', 'm24xx', '/eeprom/', 'at_commands'])]
        for upstream in repo['paths']:
            url = f'https://raw.githubusercontent.com/{repo["repo"]}/{sha}/{upstream}'
            response = requests.get(url, timeout=25)
            response.raise_for_status()
            path = ROOT / 'source' / repo['name'] / upstream
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(response.content)
            record['artifacts'].append(dict(path=path.relative_to(ROOT).as_posix(), upstream_path=upstream,
                url=url, bytes=len(response.content), sha256=sha256(response.content).hexdigest(),
                license='Upstream license and source notices apply; reference-only, excluded from library builds.'))
        record['status'] = 'archived'
    except Exception as error:
        record.update(status='incomplete', error=str(error))
    print(repo['repo'], record['status'], len(record['artifacts']), flush=True)
    return record


def search(query):
    record = dict(query=query, api='https://api.github.com/search/repositories',
                  checked_utc=datetime.now(timezone.utc).isoformat())
    try:
        response = get_json(record['api'], params=dict(q=query, per_page=100))
        record.update(total_count=response['total_count'], incomplete_results=response.get('incomplete_results'),
                      repositories=[dict(name=item['full_name'], url=item['html_url'], description=item.get('description')) for item in response.get('items', [])])
    except Exception as error:
        record['error'] = str(error)
    print('search', query, record.get('total_count', record.get('error')), flush=True)
    return record


def main():
    with ThreadPoolExecutor(max_workers=4) as pool:
        records = list(pool.map(archive, REPOSITORIES))
    # Sequential queries respect the search endpoint's separate rate limit.
    searches = [search(query) for query in SEARCH_QUERIES]
    document = dict(scope='Selected official manufacturer repositories plus bounded repository-metadata searches; not authenticated global code search or every manufacturer repository.',
                    repositories=records, repository_searches=searches)
    (ROOT / 'repository-inventory.json').write_text(json.dumps(document, indent=2) + '\n', encoding='utf-8')
    hashes = [f'{item["sha256"]}  {item["path"]}' for record in records for item in record['artifacts']]
    (ROOT / 'SOURCE-SHA256SUMS').write_text('\n'.join(sorted(hashes)) + '\n', encoding='utf-8')
    import update_checksums
    update_checksums.main()


if __name__ == '__main__':
    main()
