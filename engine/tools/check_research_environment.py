#!/usr/bin/env python3
"""Run the actual ordinary-Play research filter without Android dependencies."""
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    source = ROOT / 'ports/shared-android/apk/app/src/main/java/com/lasrecomp/android/SharedFiles.java'
    test = ROOT / 'tests/ResearchEnvironmentTest.java'
    out = ROOT / 'build/research-environment'
    out.mkdir(parents=True, exist_ok=True)
    text = source.read_text()
    start = text.index('    static String withoutResearchEnvironment(')
    end = text.index('    private static String contents(', start)
    method = text[start:end]
    extracted = out / 'SharedFilesFilter.java'
    extracted.write_text('import java.util.*;\nfinal class SharedFilesFilter {\n' + method + '}\n')
    classes = out / 'classes'
    classes.mkdir(exist_ok=True)
    subprocess.run(['javac', '--release', '17', '-d', str(classes), str(extracted), str(test)], check=True)
    result = subprocess.run(['java', '-cp', str(classes), 'ResearchEnvironmentTest'], check=True,
                            capture_output=True, text=True, timeout=30)
    report = {'state': 'passed', 'host': json.loads(result.stdout),
              'scope': 'Actual extracted SharedFiles newline/key filter, LF/CRLF and presence-only flags; no device claim.',
              'source_sha256': {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest()
                                for p in (source, test, Path(__file__).resolve())}}
    (out / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
