# iOS 15.1.1 device lookup validation using Frida

## Result

The original image-lookup defect was reproduced on the connected device with the 21 engine, and the same scoped test passed with the 22 engine. This validates the targeted fix; it does not validate replacing the system engine or completing a userspace reboot.

Device environment: iPhone OS 15.1.1, full-access USB Frida 16.1.4. The diagnostic executable and both test engines loaded their `arm64e` slice (Mach-O CPU subtype `0x80000002`). Installed packages before and after the test were ElleKit `1.2-3` and RootHide `0.1.0`.

| Query | Baseline 21 | Candidate 22 |
| --- | --- | --- |
| Explicit IOSurface `dlopen` | Non-null handle | Non-null handle |
| iOS 16 header SPI | Not exported | Not exported |
| IOSurface `realpath` | Unavailable, errno 2 / ENOENT | Unavailable, errno 2 / ENOENT |
| dyld loaded image header | `0x1a9869000` | `0x1a9869000` |
| `MSGetImageByName` | NULL | `0x1a9869000` |
| `MSFindSymbol("__iosService")` | Not reached | `0x1da87a8f8` |
| `MSFindSymbol("__iosConnect")` | Not reached | `0x1da87a8e0` |
| Native diagnostic output | `FAIL: image lookup rejected or mismatched an explicitly loaded IOSurface` | `PASS: image and private symbols resolved` |

Addresses above are observations from this device boot, not constants to embed in code.

## Isolation and attribution

Both libraries were taken from the already verified build artifacts and uploaded to a unique temporary test directory. Byte-for-byte SHA-256 verification succeeded before execution:

- Baseline 21: `4f6376e8aa89725148b634a60abaaa694b8019b9e6a27ccb2dcdf39fa039a700`
- Candidate 22: `2e5c32d14a98d424a0e18b1e3182bd865bfa513e41a2d4b30a28f1c31aefd6df`
- Native diagnostic executable: `cf9fa78bca293bc11f914d91ba7717c9ef0bb34f4608fde35bb3b195631cd5ec`

Each version ran in a different newly spawned diagnostic process. Frida observed `dlopen`, the missing header SPI, and engine-specific `MSGetImageByName` / `MSFindSymbol` entry points. Export addresses belonged to each uploaded engine, rather than to the installed ElleKit library. All hooks were confined to those diagnostic processes; no hooks were installed into launchd.

The comparison was then repeated without attaching any agent script to the diagnostic executable. It produced the same baseline FAIL and candidate PASS. Both native test processes exited, and no test process remained afterward.

The process still inherited normal RootHide runtime libraries, including the installed ElleKit library. Direct engine-specific lookups verify this query path in that environment; they are not equivalent to starting launchd with the replacement as its sole engine.

## Test setup issue

The first diagnostic launch attempts from a test directory under the jailbreak root's `var/tmp` were rejected with `unsupported file format`. That location resolved into an AppGroup data directory. Moving the same verified signed universal executable into a dedicated directory under the jailbreak root's `usr/lib` allowed it to start. Thinning the binary was not necessary. This was a diagnostic setup failure, not a failed engine query.

When running a standalone tool manually in RootHide, account for its code/data path handling instead of assuming an arbitrary media/tmp directory is executable. This test used a unique `usr/lib/lh-frida-*` directory and removed it afterward.

## Cleanup and remaining boundary

The test did not install either deb, replace ElleKit, modify the tweak-loader or `basebin/present` links, invoke IOKit connection operations, or request a userspace/device reboot. All four exact directories created during the session were removed or confirmed absent, including only their own verified `.jbroot` links. Package records and relevant system links matched the pre-test snapshot. All Frida sessions were detached.

The next questions are separate device validations: actual launchd replacement startup, mandatory XPC hooks and their original trampolines, code-page/PAC behavior, tweak loading and safe-mode behavior. A successful lookup test does not settle those questions. No full engine replacement or reboot was attempted in this session.
