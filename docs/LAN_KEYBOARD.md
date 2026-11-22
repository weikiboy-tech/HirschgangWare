# Main-PC keyboard over LAN

The keyboard sender runs in the signed-in Windows session on the main PC.
Raw Input listens for physical keyboard events and forwards the configured
keys. The default allowlist for new pairing bundles contains `E,Y`.
The protocol carries key states. It carries no mouse events or device
identifiers. The sender has no listening socket.

On the second PC, HirschgangWare uses the existing `Main PC` binding source.
If a valid PCIe keyboard snapshot is available it has priority. Otherwise the
paired LAN sender supplies the keyboard state. MAKCU mouse bindings and local
bindings retain their separate sources. The LAN receiver starts with the
launcher. Starting game/DMA capture still requires the launcher Start button.
Clutch Mode reads a live paired LAN sender directly. It falls back to the
PCIe keyboard in DMA mode when the sender disconnects. Its hotkey must also
be in the main-PC sender's allowlist.

## Second PC

1. Build `build.bat x64` and `tools\build_lan_keyboard.bat` once in a Visual Studio C++ environment.
2. Run from the repository, replacing the example with the second PC's LAN IPv4 address.

   ```powershell
   .\tools\setup_lan_keyboard_receiver.ps1 -ListenAddress 192.168.1.42
   ```

3. If Windows Firewall blocks the connection, rerun the setup in an elevated
   shell with `-OpenFirewall`. The rule applies only to `HirschgangWare.exe`,
   the selected interface and IP, TCP `47832` by default, and the local
   subnet. The default profile is Private. Add `-FirewallProfile Public`
   if the intended Ethernet interface is registered as a Public network.
4. Restart the launcher to load the new configuration. Transfer
   `build\HirschgangWare-LAN-MainPC.zip` privately to the main PC. It contains
   the pairing key. Keep the ZIP and both INI files out of GitHub and shared links.
   If the receiver's LAN address changes, rerun setup with the new address
   and install the updated bundle on the main PC.

## Main PC

Extract the package and run `install_lan_keyboard_sender.ps1` once as the user
who will use the keyboard. The installer starts the updater and adds it to
that user's Windows Startup folder. The updater starts the sender at sign-in.
Use `-Uninstall` on the same script to remove the shortcut and stop both processes.

The receiver setup can generate a new bundle with `-Keys 'E,F9'` if other
hotkeys are needed. The list supports up to eight keys. Use capitals for
letters, `F1` through `F24`, `Escape`, or `VK:` followed by a decimal virtual-key
code. Reinstall the updated bundle to apply it.
For an update of an already paired installation, the public package can reuse
the installed pairing file. A fresh installation still needs the private INI.

To bind any supported physical key directly in HirschgangWare, use the updated
sender. Run `.\install_lan_keyboard_sender.ps1 -AllKeys` on the main PC, or
generate the private package with `-Keys All` on the second PC. Both options
set `keys=All` without changing the LAN protocol or pairing. This mode sends
key states to the paired PC even outside the game while Windows is unlocked.
Switch back to a limited list in the sender INI and restart the sender to
limit forwarding again. Clutch continues to reserve Escape, End and F8.

## Sender updates

An existing installation needs the new package once to enable automatic updates.
The updater checks the fixed HirschgangWare GitHub repository over HTTPS when
it starts and hourly thereafter. It validates the published SHA-256 and the
executable's embedded revision. A newer sender starts after one minute of
input inactivity. Startup failure restores the previous sender. The pairing
INI and shortcut stay in place. Keyboard input continues over LAN.

The latest status is in
`%LOCALAPPDATA%\HirschgangWareInput\lan-keyboard-update.log`.
An offline or invalid update leaves the installed sender running.

To publish a new sender, increment `HG_LAN_SENDER_REVISION` in
`tools/lan_keyboard_version.h`, run `tools\build_lan_keyboard.bat` and
`tools\package_lan_keyboard.ps1`, then publish the public ZIP, raw sender EXE,
and `lan-keyboard-version.txt` from `build\lan-keyboard-public` together
under `dist/`. Keep the private pairing bundle out of that directory.

The sender keeps short transitions in a bounded queue and sends each change
in order. The receiver counts presses and releases separately per key and
per authenticated connection, even when a whole tap falls between two UI
frames. The Assist and Trigger panels show these counts for the configured
LAN key and whether it is currently held. Binding can use a completed tap.
Hold actions use the current state and never extend a released key to replay
an old tap. The sender reconnects if its message pump stops or the change
queue gets too old or overflows. The first snapshot is a baseline, so input
from a previous connection is not replayed. Update both programs to get all
of these changes. The message format and pairing file are unchanged.

## Verification

After starting capture, open the Trigger bindings and check `LAN keyboard:
connected`. Start binding, then press and release `E` at the main PC. The
binding should say `Main PC (LAN)` and show `HELD` for the duration of an
ordinary hold. A press on the second PC must leave that remote binding idle.
Under `ESP OVERLAY → CLUTCH MODE`, press and release the configured Clutch
key on the main PC. In Toggle mode, each press changes the active state
once. The LAN line shows the held state and separate press and release counts.
Check an existing MAKCU mouse binding independently. When the sender exits
or stops sending authenticated snapshots, the reader clears held keys within
220 ms of the last snapshot. A held key during reconnection must be released
once before it can activate again.

The peers use TCP with AES-256-GCM, a random challenge per connection, and
ordered snapshots. The receiver reports connected after the first valid
snapshot. The listener binds only to the configured private interface.
