#!/usr/bin/env python3
"""Check the installed Groundhog identity contract without a graphical session."""

import configparser
from pathlib import Path
import sys
import xml.etree.ElementTree as ET

build = Path(sys.argv[1])
data = Path(sys.argv[2])
app_id = "org.nostr.Groundhog"

desktop = configparser.ConfigParser(interpolation=None)
desktop.optionxform = str
desktop.read(build / f"{app_id}.desktop", encoding="utf-8")
entry = desktop["Desktop Entry"]
assert entry["Type"] == "Application"
assert entry["Exec"] == "groundhog"
assert entry["Icon"] == app_id
assert entry["DBusActivatable"] == "true"
assert "MimeType" not in entry  # nostr: belongs to the shared dispatcher

service = configparser.ConfigParser(interpolation=None)
service.optionxform = str
service.read(build / f"{app_id}.service", encoding="utf-8")
assert service["D-BUS Service"]["Name"] == app_id
assert service["D-BUS Service"]["Exec"].endswith("/groundhog")

meta = ET.parse(data / f"{app_id}.metainfo.xml").getroot()
assert meta.findtext("id") == app_id
assert meta.find("launchable").text == f"{app_id}.desktop"
assert meta.find("launchable").attrib["type"] == "desktop-id"

schema = ET.parse(data / f"{app_id}.gschema.xml").getroot().find("schema")
assert schema.attrib == {"id": app_id, "path": "/org/nostr/Groundhog/"}
keys = {key.attrib["name"]: key for key in schema.findall("key")}
assert set(keys) == {
    "current-npub", "signer-method", "notifications-enabled",
    "notification-privacy", "sound-enabled", "window-width",
    "window-height", "window-maximized", "discovery-relays",
}
assert keys["current-npub"].findtext("default") == "''"
assert keys["notification-privacy"].findtext("default") == "'hidden'"
assert keys["notifications-enabled"].findtext("default") == "false"
# No relay is contacted until the user configures one.
assert keys["discovery-relays"].findtext("default") == "[]"

resources = ET.parse(data / "groundhog.gresource.xml").getroot()
files = {node.text for node in resources.iter("file")}
assert "style.css" in files
assert f"icons/{app_id}.svg" in files
assert ET.parse(data / "icons" / f"{app_id}.svg").getroot().tag.endswith("svg")
# All UI is Blueprint: every .blp has its committed compiled .ui fallback, no
# .ui lacks a .blp source, and each is bundled.
blueprints = {path.stem for path in (data / "ui").glob("*.blp")}
compiled = {path.stem for path in (data / "ui").glob("*.ui")}
assert blueprints, "no Blueprint sources in data/ui"
assert blueprints == compiled, f"data/ui .blp/.ui mismatch: {sorted(blueprints ^ compiled)}"
assert {f"ui/{name}.ui" for name in blueprints} <= files
assert {name for name in files if name.startswith("ui/")} == {f"ui/{n}.ui" for n in blueprints}
print("Groundhog app ID, desktop, service, metadata, schema, resources and UI agree")
