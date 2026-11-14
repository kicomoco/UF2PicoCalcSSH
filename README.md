# PicoCalc SSH

SSH client firmware for a **ClockworkPi PicoCalc with Raspberry Pi Pico 2 W**,
using the same **uf2loader** SD-card launch arrangement.

## Install

1. Copy the compiled [`picocalc_ssh.uf2`](build/picocalc_ssh.uf2) into
   `pico2-apps/` on the SD card.
2. Optionally put private keys in an SD directory such as `/keys/`.
3. Boot the PicoCalc's existing uf2loader and select **picocalc_ssh**.

Do not replace your existing bootloader. This is a regular Pico 2 W UF2, not
the offset-linked `.bin` format used by ClockworkPi's older SD launcher.
Settings are stored on SD, never in application flash.

## Connect

The application opens on **F1 WiFi**, focused on saved networks rather than a
connection form. Both WiFi and SSH use ASCII-framed windows and highlighted focus.
The top bar of every screen, including the SSH terminal, shows the battery level
(`[Bat 87%]`, `+` while charging, `--` if unknown; yellow at 30% or less, red at 15% or less).

1. Startup scans nearby WiFi and attempts to join the **strongest compatible
   saved network**. Alternatively, select a saved network and press **Enter**
   or **F5 Join WiFi**. **Delete/F8** forgets it after confirmation. While a
   join is in progress, a smaller **Connecting WiFi** window overlays the page,
   showing the network, the progress text with a `/ - \ |` spinner, and
   **Esc: cancel**. This window is modal: other keys are ignored until the join
   finishes or is cancelled.
2. To add WiFi, select the secondary **[ Add new WiFi connection ]** button.
   Enter the SSID/password manually (including hidden networks), or select
   **[ Scan networks ]**, choose a result, and enter its password.
   Press **F5 Join WiFi** or **[ Join WiFi ]**. **Tab**, **Up/Down**, and
   **Shift+Tab** move between fields/buttons; **Ctrl+U** clears a field.
3. After a successful join and DHCP, a **WiFi connected** window shows a
   **three-second countdown** to **F2 SSH**. **Enter** opens SSH immediately;
   any other key closes the window and stays on WiFi (an F-key also opens its
   page). WiFi loss also cancels the countdown.
4. The SSH page focuses on saved connections. Select a preset and press
   **Enter** to **immediately attempt the SSH connection**, using its saved
   host, port, username and private-key path. The SSH form opens with focus on
   **[ Connect SSH ]**, which also stays focused after a failed attempt for an
   easy retry. A **Connecting SSH** window shows `user@host:port`, the progress
   text with a spinner, and **Esc: cancel**. Press **E** instead to edit a
   preset or enter a password before connecting.
   **[ Add new SSH connection ]** opens a blank form with host, port (default
   `22`), username, optional password and optional private key (**Enter** on the
   Private key field browses the SD card).
   From the form, **[ Connect SSH ]** or **F9** connects; **F6** saves the profile.
   When the form came from a saved preset (E, or a preset attempt), **F6 updates
   that preset in place**, even if you changed its host, port or username. If
   the edited details now match another saved preset, the two are merged. Forms
   opened with Add New save a new preset.
   New/editable SSH details and connections remain locked until WiFi is ready.
5. On the first connection to each host/port, compare the displayed SHA256
   host-key fingerprint with a trusted source, then press Enter to trust it.
   The fingerprint is displayed in hexadecimal, not OpenSSH's default base64.
   For comparison, on the server use, for example:

   ```sh
   ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub -E sha256
   # Convert the SHA256 base64 suffix to hex on a trusted computer:
   python3 -c 'import base64; print(base64.b64decode(input("SHA256 suffix: ") + "==").hex())'
   ```

   Verify the key type actually used by the server (RSA, ECDSA or Ed25519).
   Changed keys are **blocked**, not silently replaced. **F3 Host Keys** manages trusted
   host keys; forget a changed key only after independently verifying why it
   changed. Trust approval expires after 60 seconds. If the SD card cannot save
   a new host key, the connection is rejected before authentication.

Successful WiFi connections are remembered automatically, with the most recently
connected network first. Startup scans asynchronously, then compares all results
against saved SSIDs and open/protected authentication settings. Unknown networks,
WEP and incompatible open/protected matches are ignored. The compatible match
with the highest RSSI is tried once, regardless of saved order. If no match exists,
the scan fails, or joining fails, the app stays available for manual selection and
shows the result/error. There is no endless retry or automatic fallback.
Hidden networks require manual selection because they do not appear in scans.
Any keyboard input cancels pending startup auto-selection (the radio scan still
finishes in the background); **Esc** also cancels a join already in progress.

Existing settings from earlier firmware remain readable and participate in
startup matching immediately. SD load errors/recovery warnings disable automatic
joining, while still allowing scanning and manual operation.

SSH profiles are saved explicitly with **[ Save SSH profile ]** or **F6** on
the SSH page, and contain host, port, username and private-key path.
**SSH passwords are not saved**; they are cleared from the form when a connection
starts and from the SSH client after authentication or disconnect. A preset
attempt does not reuse an old form password. For password-only profiles, use
**E** to supply a password, or enter one on the form after an unsuccessful attempt.

### F4 Settings

| Setting | Values |
| --- | --- |
| Screen | LCD backlight 10-100% in 10% steps (never fully dark) |
| Keyboard light | 0-100%, or the keyboard firmware's default; shows n/a on factory keyboard firmware without the backlight register |
| Startup WiFi | Auto-join strongest known network, or Manual (no startup scan) |
| Open SSH page | After the 3-second countdown, or Manual (stay on WiFi; use F2) |
| Software | **[ Check for updates ]** (Enter; needs WiFi) - see [Updates](#updates) |

**Up/Down** selects, **Left/Right** changes, **Enter** steps/toggles (brightness
wraps). Brightness applies immediately. Changes are saved to
`/sshclient/prefs.bin` shortly after the last keypress (CRC-protected, separate
from WiFi/SSH profiles). A damaged preferences file falls back to defaults with
a warning. The page also shows battery level/charging, the WiFi network, IP
address and the installed firmware version.

### Updates

**F4 Settings → [ Check for updates ]** checks the latest
[GitHub release](https://github.com/kicomoco/UF2PicoCalcSSH/releases) over HTTPS
and, if it is newer than the installed version, asks before installing it:

1. **Checking** - downloads the release's `SHA256SUMS` and compares versions
   (development builds always count as older). It then finds the SD file the
   app was launched from by comparing each `.uf2` in `/pico2-apps` (and its
   subfolders) with the running firmware. If none matches, the update is saved as
   `/pico2-apps/picocalc_ssh.uf2`. A popup shows the versions and the file that
   will be replaced; **Enter** updates, **Esc** postpones.
2. **Downloading** - a progress bar shows KB and percent; **Esc** cancels and
   removes the partial file. The UF2 is written to `<file>.new`, checked block
   by block (RP2350 family, addresses, numbering), and its SHA-256 must match `SHA256SUMS`.
3. **Verifying/installing** - the saved file is re-read and re-hashed, the
   original is renamed to `<file>.bak`, the new file takes its place and is
   verified again, then the backup is deleted. Any failure restores the
   original file. Do not power off during this step.
4. **Reboot** - a five-second countdown reboots into the uf2loader menu
   (**Enter** now, **Esc** aborts). Choose **PicoCalc SSH** in the menu to
   flash the new version. If you abort, the update stays on the SD card; hold
   **Up** (or F1) while powering on to open the menu later.

HTTPS uses wolfSSL TLS 1.2/1.3 with the CA roots embedded in
[ca_roots.c](src/ca_roots.c), generated by
[gen_ca_roots.py](tools/gen_ca_roots.py) from the Mozilla bundle. These are
ISRG, USERTrust/Sectigo and DigiCert, as used by github.com and its
release-asset hosts. Hostnames are checked. The PicoCalc has no real-time
clock, so the clock used for certificates starts at the firmware's build
time: certificates issued after the build are accepted, but certificates
that expired before that time are rejected. Releases come from the
repository given by `-DUPDATE_REPO=owner/repo` (CI uses the repository it
runs in).

### Keys outside the remote terminal

| Key | Action |
| --- | --- |
| F1 | Saved WiFi networks, with secondary Add New and Disconnect buttons |
| F2 | Saved SSH connections, with a secondary Add New button |
| F3 | Host Keys: F8/Delete forgets a trusted key |
| F4 | Settings: brightness, keyboard light, startup behaviour, battery/WiFi status |
| F5 | **Join WiFi**: selected saved network or the WiFi form's details |
| F6 | In the SSH form, save/update the current SSH profile |
| F8 / Delete | In a saved list, forget/delete the selected entry |
| F9 | In the SSH form, connect SSH |
| F10 | On-device help |
| E | Edit a selected WiFi/SSH preset; does not connect |
| Esc | Cancel a pending connection, close help, or return from a list/browser |

Deleting saved entries requires confirmation. Saved SSH connections can be
viewed/deleted offline, but loading/editing them requires established WiFi.
Up to **16 WiFi networks**, **24 SSH profiles**, and **32 trusted host keys**
can be stored. Existing SSIDs/profiles are updated rather than duplicated.

In both saved lists, Enter connects to the selected preset. PgUp/PgDn page through lists.
To disconnect WiFi, select **[ Disconnect WiFi ]** on the WiFi page.

In the key browser: Enter opens a directory or chooses a file; Backspace goes
to the parent directory; PgDn/PgUp page through all entries; Esc returns to SSH.
Paths up to 255 bytes and long filenames are supported.

### WiFi scanning

The scan runs without blocking the keyboard or disconnecting established WiFi.
Wait for it to finish before selecting a result. The list shows SSID, security
and signal strength (dBm), with stronger signals first. Repeated access points
with the same SSID/security are combined using their strongest signal.
Up to 32 entries are shown; a notice indicates when the list limit is reached.
Hidden/empty SSIDs are omitted; enter these manually using **Add new WiFi connection**.

Enter loads the selected SSID into the new/edit WiFi form, reusing a saved password
for protected networks when available. Open networks clear the password.
Review the password and press **F5 Join WiFi**; selecting alone does not connect or
save anything. Successful joins are remembered. WEP-only networks are
marked unsupported; the radio's scan information cannot distinguish all
WPA2 enterprise/WPA3-only networks, which remain unsupported.

On the scan screen, **Tab** switches between results and **[ Scan again ]**;
**Enter** activates the focused item, **PgUp/PgDn** changes page, and **Esc**
returns to WiFi. Leaving the screen lets
the current scan finish in the background; wait for completion before joining.
Failed or timed-out scans show an error and can be retried with **[ Scan again ]**.

### Remote terminal

A 53-column by 24-row color terminal supports common ANSI/VT100 cursor controls,
scroll regions, alternate screens, UTF-8 decoding, 16/256/RGB colors and terminal
size reporting. Rendering supports ASCII and Latin-1, with ASCII approximations
for box-drawing characters; it is not a full Unicode/xterm implementation and
has no scrollback.

**Ctrl+] disconnects**. All other supported keys, including F1-F10, arrows,
Ctrl combinations, Home/End and Page Up/Down, are sent to the remote host.
When a session ends (Ctrl+], `exit` on the server, a server disconnect or WiFi
loss), a session started from a **saved connection** returns to the **saved SSH
connection list** with that connection still selected, so Enter reconnects. A
session for an unsaved, typed-in connection returns to the SSH form. Connection
attempts that fail before the session opens (for example authentication
failures) stay on the SSH form with **[ Connect SSH ]** focused, so the
details/password can be corrected.

## Authentication and network support

- 2.4 GHz open or WPA/WPA2-Personal WiFi with DHCP. No captive-portal,
  enterprise/EAP, static-IP or WPA3-only setup.
- IPv4 addresses and DNS hostnames; custom SSH ports.
- Password authentication, or **unencrypted RSA (up to 4096-bit) / NIST P-256
  private keys in PEM or DER**, including PKCS#8. A selected key is tried first;
  a supplied SSH password can be used if the server rejects it.
- Encrypted private keys, OpenSSH-format private-key files, Ed25519 *user*
  keys, keyboard-interactive/OTP and SSH agents are not supported. Unsupported
  files produce an explicit error. The SSH password field is a server login
  password, not a private-key passphrase.
- Ed25519, RSA-SHA2 and ECDSA-P256 **server host keys** are supported.
- SSH uses Curve25519/ECDH-P256, AES-CTR and HMAC-SHA256. No legacy SHA1
  host signatures/key exchange or CBC ciphers are offered.

For an existing RSA OpenSSH private key, convert a **copy** to PEM on a trusted
computer (`ssh-keygen -p -m PEM -f copied_key`). Use a dedicated, minimally
privileged SSH account/key for the handheld.

### SD-card privacy and recovery

`/sshclient/settings.bin` contains WiFi passwords **in plaintext**, SSH profile
metadata and trusted host fingerprints. Private-key files are also unencrypted.
Keep the SD card physically secure. Forget/Delete removes the logical saved
entry and old backup, but **does not securely erase flash-card sectors**, delete
private-key files, or revoke access on the SSH server.

Settings have a versioned header, CRC32 corruption check, synced temporary file
and backup rotation. Recoverable backups are loaded with an on-screen warning.
Unreadable/damaged settings are not silently overwritten: restore them or remove
the settings files on a computer to reset. SD errors are displayed; the app
does not claim an entry was saved/forgotten when its write fails.

## Build

Requires CMake 3.20+, Git, Python 3, a host C/C++ build tool, and Arm GNU Toolchain
(`arm-none-eabi-gcc`; the verified build used 14.2.rel1).

```sh
# Point to your Arm GNU Toolchain installation:
export PICO_TOOLCHAIN_PATH=/path/to/arm-gnu-toolchain
export PATH="$PICO_TOOLCHAIN_PATH/bin:$PATH"
./setup.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
python3 tools/verify_uf2.py build/picocalc_ssh.uf2
```

An existing Pico SDK 2.2.0 checkout can be reused instead of the pinned copy:

```sh
export PICO_SDK_PATH=/path/to/pico-sdk
export PICO_TOOLCHAIN_PATH=/path/to/arm-gnu-toolchain
./setup.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

Dependencies are pinned by commit in [setup.sh](setup.sh):
Pico SDK 2.2.0, wolfSSH 1.6.0 and wolfSSL/wolfCrypt 5.9.4.
The [crypto build](tools/crypto.cmake) uses stock upstream wolfSSH/wolfCrypt
sources with embedded settings; the client sends the terminal dimensions as
soon as its PTY session opens. Upstream dependency checkouts are unchanged.
The linker reserves a 16 KiB main stack in SRAM, instead of overflowing the SDK's
4 KiB scratch bank; flash addresses remain compatible with uf2loader.

## Versions and releases

The firmware version is shown on the F4 Settings page. [VERSION](VERSION) holds
`MAJOR.MINOR`; local builds report `MAJOR.MINOR.0-dev` unless
`-DAPP_VERSION=X.Y.Z` is passed to CMake.

Every push to `main` runs [the release workflow](.github/workflows/release.yml):
it runs the host and SSH integration tests plus a live HTTPS check against
GitHub (the device's update path), picks the next patch number from
the existing `vMAJOR.MINOR.*` tags (the first release is `vMAJOR.MINOR.0`),
builds and verifies the UF2 with Arm GNU Toolchain 14.2.rel1, then publishes a
GitHub release `vX.Y.Z` with `picocalc_ssh-vX.Y.Z.uf2` and `SHA256SUMS`.
No commit is pushed back, so releases never retrigger the workflow. To start a
new minor/major series, edit `VERSION` (e.g. `1.1`) and the next release is `v1.1.0`.

## Tests and verification

```sh
cmake -S tests -B build-host -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host -j8
ctest --test-dir build-host --output-on-failure

python3 -m venv .deps/test-venv
.deps/test-venv/bin/python -m pip install -r tests/requirements.txt
# Use the absolute interpreter path (also avoids a relative-venv issue on macOS/Python 3.15):
"$PWD/.deps/test-venv/bin/python" tests/integration.py build-host/ssh_integration
```

The native suite runs production model, UI-controller, network, terminal and SD-storage
code under AddressSanitizer/UBSan. SD tests use real FatFs on a RAM-backed FAT
volume, including old/new settings compatibility, last-network persistence,
device preferences (defaults, interrupted swaps, corruption, invalid values),
reboot/reload, deletion, backup recovery, corruption and failed writes.
Terminal tests include 200,000 deterministic malformed/split input bytes.
Loopback SSH tests use the production SSH client with a real Paramiko server,
fragmented transport, a PTY and 64 KiB of checked output. They cover password,
PEM/DER/PKCS#8 keys, password fallback, bad passwords, host-key rejection before
credentials, and RSA/ECDSA/Ed25519 server keys.

WiFi scan tests use a simulated CYW43 driver to cover asynchronous completion,
duplicates, security variants, signal sorting, capacity, timeout/error recovery
and preservation of the existing connection. UI tests cover separate ASCII
screens, text bounds, function-key navigation, offline SSH locking and saved-list
management, the scan/rescan buttons, paging, protected/open network selection,
saved-password reuse, unsupported WEP and retry/empty states. Startup tests cover
strongest compatible known-network selection with unsorted/duplicate results,
legacy settings, successful/failed/cancelled joins, scan failures/timeouts,
forgetting a pending join, and SD write/load failures. UI tests also verify the
three-second countdown (including cancellation, WiFi loss and clock wrap),
saved-first paging, immediate SSH preset attempts focusing Connect, optional
preset editing and password clearing, in-place preset updates/merges, where
ended sessions return (saved list vs form),
modal WiFi/SSH progress windows with spinners, countdown-window keys, battery
indicators in every top bar, and
F4 settings (immediate backlight changes, debounced saving, clamping, reboot
persistence, disabled auto-join/countdown, unsupported keyboard light, save
failures, and F4 passthrough in the terminal). The software update flow is
covered from the Settings row through modal check/prompt/progress windows,
non-cancellable installation and the abortable reboot countdown.

Update tests (`update_tests`) run the real updater against FatFs on a RAM disk
with a simulated HTTPS server. They cover version ordering, `SHA256SUMS`
parsing, streaming UF2 validation, and finding the launched file among
look-alike and truncated UF2s. The loader-patched proginfo bytes are ignored.
They also cover install-in-place, the default-path fallback, checksum
mismatches, corrupt and truncated downloads, cancellation, stale backups,
HTTP errors and SD write failures; the original file always survives.
`build-host/https_live` (internet required) fetches the real release over
TLS. It checks certificate chains, redirects, a stale device clock, expired
certificates (with a 2050 clock), an untrusted root and HTTP errors.

The UF2 is cross-compiled for Pico 2 W and its family IDs, block numbering and
flash addresses are validated. **Physical PicoCalc/WiFi/SD-loader operation
still requires an on-device test**; native tests cannot verify the actual radio,
display or keyboard hardware.

## License and credits

New application code is GPL-3.0-or-later; see [COPYING](COPYING).
wolfSSH and wolfSSL/wolfCrypt are used under their GPLv3 licensing terms.
Keep source/build instructions and license notices when distributing firmware.

The PicoCalc display, keyboard, SD and graphics drivers follow the PicoCalc
hardware and keyboard timing. FatFs retains
ChaN's embedded license notices. The Pico SDK and its submodules retain their
respective upstream licenses in the pinned dependency checkouts.
