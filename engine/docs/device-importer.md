# LAS import directly on Thor

`tools/import_on_thor.py` packages the existing game-free host importer as Android DEX. The optional device run decrypts/extracts the already present LAS base NSP locally on Thor, avoiding a multi-gigabyte ROMFS transfer over Wi-Fi. LAS is the only accepted title. The tool never copies keys or game packages between host and device.

Build only, with no ADB calls:

```bash
python3 tools/import_on_thor.py --title las
```

Prerequisites: JDK17, SDK36 platform and build-tools36.0.0, pinned source checkouts, and generated Java classes from the host LAS importer. It prefers `build/host-importer/las`; the older `build/host-importer` layout is accepted after checking the compiled LAS title selector. `--classes` and `--sdk` override those locations. Python's `zipfile` creates the input JAR, so the missing host `zip` program is unnecessary. Output is ignored `build/device-importer/las/classes.dex`; generated helper/JAR/classes contain importer code only.

Parent-run device action, after installing/preparing the isolated baseline app:

```bash
python3 tools/import_on_thor.py --title las --serial "$THOR_SERIAL"
```

`--serial` explicitly enables mutation. The tool reads the single LAS base NSP and the first device key path from ignored `evidence/device-inventory.json`; it does not infer permission or correctness from inventory alone. It verifies shell readability, package installation and an existing writable isolated `files` directory before pushing only the DEX to its content-addressed `/data/local/tmp` directory. It runs Java through `CLASSPATH=… app_process /system/bin …` under the **ADB shell UID**. Remote operands use `shlex.join`, including paths containing spaces and brackets.

Output is exclusively `/sdcard/Android/data/com.lasrecomp.android.test/files/extracted`. The destination must not exist: both remote preflight and an exclusive Java `mkdir()` prevent rewriting a previous extraction. Importer failures leave partial output for inspection; there is no automatic deletion, retry/resume, or overwrite flag. The utility does not install, start, stop or configure apps, invoke root, change permissions/performance settings, or touch emulator saves/assets. Its generated temporary DEX stays on-device for reproducibility. Parent should separately manage any later cleanup.

The Java launcher uses the same `NxPackage` and build-ID helper classes, requires LAS base build prefix `ae16f71e002af8cb`, and converts importer error results into command failure. Completion requires importer success plus main/sdk and ROMFS presence. Device logs and a path-only manifest remain in ignored `evidence/las-device-import.*`; key contents are never printed or recorded.

Shell-context readability/writability does **not** establish that the app UID can read the resulting tree. Android13 external-storage FUSE/scoped-storage behavior, SELinux and existing package-directory state still require an app-context check by parent. Permission failures are reported without using root or chmod. The actual app must use its own extracted files; it does not gain access to the emulator's key directory through this helper. Game library deployment/internal-storage loading remains a separate step.

Host validation on 2026-10-24: the default build completed with local JDK17 and SDK36 D8; the output passed its DEX magic check, input JAR contained only class files, and inventory selection resolved the LAS base entry and a device key path without reading keys. DEX compilation does not verify ART execution, cryptography, extraction speed, successful app loading or gameplay. No device calls or import were executed during implementation validation.
