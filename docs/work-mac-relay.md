# Work-Mac relay setup

Current status (2026-09-08): deployed and verified. The work Mac owns Bluetooth
and Apple Music; the Mac mini serves Claude, Codex, and Pluribus over Tailscale.
Keep the Mac-mini Bluetooth writer unloaded while this arrangement is active.

This is the Phase 2 setup for using the physical Clawdmeter at the office while
keeping its data sources split across two Macs:

```text
Mac mini                      Work Mac
Claude usage  ─┐
Codex usage   ─┼─ Tailscale ─> receiver ── Bluetooth ─> Clawdmeter
Pluribus      ─┘                  +
                              Apple Music
```

The ESP32 firmware and its compact BLE payloads do not change. The work Mac is
the only Bluetooth owner. It downloads display-safe Claude, Codex, and Pluribus
state from the Mac mini, adds Apple Music from its own Music.app, and sends each
screen through the existing daemon. Raw transcripts, Claude credentials, Codex
sessions, and Pluribus credentials never leave the Mac mini.

The relay listens only on the Mac mini's Tailscale address. Tailscale encrypts
the connection, and the endpoint also requires a randomly generated bearer
token. Treat that token like a password.

## 1. Start the collector on the Mac mini

Both Macs must be signed into the same tailnet. On the Mac mini, update the
personal checkout and run:

```bash
cd /Users/macmini/Clawdmeter-main
git pull --ff-only origin main
./install-relay-macmini.sh
```

The installer detects the Mac mini's active Tailscale IPv4 address, creates or
preserves a private relay token, installs a LaunchAgent, and prints two values:

```text
Work-Mac relay URL:   http://100.x.y.z:8765/v1/state
Work-Mac relay token: <private token>
```

Transfer both values to the work Mac through a private channel. Do not paste the
token into Git, a Claude prompt, a screenshot, or a shell command that will be
saved in history.

Check the collector at any time:

```bash
launchctl list | grep clawdmeter-relay
tail -F ~/Library/Logs/clawdmeter-relay.out.log
tail -F ~/Library/Logs/clawdmeter-relay.err.log
```

The collector can run alongside the current Mac-mini Bluetooth daemon while the
device is still at home. Starting it does not alter the ESP32 or its bond.

## 2. Install the receiver on the work Mac

Install and sign into Tailscale, then clone the personal repository. Do not use
an Impulse Engineering checkout or remote.

```bash
cd ~
git clone https://github.com/chrislrosesb/clawdmeter-codex.git Clawdmeter
cd ~/Clawdmeter
./install-work-mac.sh http://100.x.y.z:8765/v1/state
```

Use the exact URL printed by the Mac-mini installer. The work-Mac installer asks
for the token without echoing it, confirms the authenticated feed is reachable,
then installs the normal macOS BLE daemon in relay mode. It may ask for:

- Bluetooth permission for Python and `blueutil`.
- Automation permission for Python to read Music.app.
- A one-time foreground scan to establish those permissions.

The token is stored only in
`~/.config/claude-usage-monitor/config`, with file mode `600`. The receiver does
not need Claude Code credentials or a local Pluribus server. Claude Code may
remain installed for unrelated use.

Before moving the hardware, verify the receiver log says:

```text
Phase 2 relay mode: Claude, Codex, and Pluribus come from Mac mini
```

It will also say the device is not found until the Bluetooth transfer is done;
that is expected.

## 3. Transfer the Bluetooth owner at the office

Do this once, in this order. Do not erase or reflash the ESP32.

1. On the Mac mini, stop only its Bluetooth writer. Leave the new relay running:

   ```bash
   launchctl unload ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist
   ```

2. With the Clawdmeter powered on, hold the middle PWR button for at least three
   seconds, then release it while the device is still on. The release clears its
   saved bond and returns it to pairing mode.
3. Forget Clawdmeter on the Mac mini if it remains in Bluetooth settings.
4. On the work Mac, open System Settings → Bluetooth and connect to Clawdmeter.
5. Complete or dismiss macOS Keyboard Setup Assistant after the Bluetooth entry
   is connected. The keyboard flow is normal: the device exposes HID for its
   physical shortcut buttons as well as GATT for the display.
6. The work-Mac daemon should connect on its next retry and send the remote usage
   and Pluribus screens. Play a track in Music.app to verify local metadata and
   artwork.

Only one Mac may own the device. Never restart the Mac-mini Bluetooth writer
while the Clawdmeter is paired to the work Mac.

## 4. Verify and operate

On the work Mac:

```bash
launchctl list | grep claude-usage
tail -F ~/Library/Logs/claude-usage-daemon.out.log
tail -F ~/Library/Logs/claude-usage-daemon.err.log
```

Verify the screens independently before waiting for a full 30-second rotation:

1. Claude and Codex values match the Mac mini.
2. A recent Pluribus activity appears and ages normally.
3. Music.app playback adds Now Playing with artwork from the work Mac.
4. Pausing/stopping Music removes that screen while the other screens remain.

If Tailscale or the relay is temporarily unavailable, the receiver leaves the
last valid usage on the display instead of replacing it with false zeroes. It
rejects a relay envelope older than three minutes and independently rejects old
source slots. Pluribus activity still expires under its existing 24-hour rule.

## Updating either Mac

Pull from the personal repository and rerun the appropriate installer:

```bash
git pull --ff-only origin main
```

- Mac mini: `./install-relay-macmini.sh`
- Work Mac: `./install-work-mac.sh http://100.x.y.z:8765/v1/state`

The Mac-mini installer preserves its existing token. The work-Mac installer asks
for it again and rewrites the private receiver config. No firmware flash is
needed for host-only updates.

For an actual firmware update, use a normal upload. This preserves the Bluetooth
bond and settings; never erase flash/NVS as part of the update.

The preferred build host for Chris's deployment is the Mac mini: bring only the
Clawdmeter home, leave its Bluetooth bond untouched, and flash it from the
known-good checkout/toolchain. The managed office network intercepts HTTPS and
has prevented PlatformIO from downloading pioarduino and ESP32 packages through
both `uv` and Python `requests`. Do not disable TLS verification to work around
that. A normal Mac-mini upload preserves the work-Mac owner stored in NVS.

If the work Mac already has every build dependency cached, it may still flash
normally with:

```bash
launchctl unload ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist
./flash-mac.sh waveshare_amoled_216
launchctl load -w ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist
```

## Rollback to the Mac mini

1. Stop the work-Mac daemon.
2. Clear the device bond with the same three-second hold-and-release gesture.
3. Forget the device on the work Mac and pair it to the Mac mini, completing the
   keyboard flow once.
4. Restart the Mac-mini Bluetooth writer:

   ```bash
   launchctl load -w ~/Library/LaunchAgents/com.user.claude-usage-daemon.plist
   ```

The relay may remain running or be stopped separately with:

```bash
launchctl unload ~/Library/LaunchAgents/com.user.clawdmeter-relay.plist
```
