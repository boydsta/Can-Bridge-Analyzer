DBC files in this folder are baked into the SPIFFS image with:

    pio run -e can-bridge-analyzer-mcp251863 -t uploadfs

and are available at /dbc on first boot. Files can also be uploaded over WiFi
at any time via http://192.168.1.1/dbc

Credits / licensing
-------------------
The Nissan Leaf DBC files

    CAR-can_AZE0.dbc
    EV-can_AZE0.dbc
    EV-can_ZE0.dbc
    EV-can_ZE1.dbc
    QC-CAN_ALL.dbc

are from Dala's (dalathegreat) leaf_can_bus_messages project:
https://github.com/dalathegreat/leaf_can_bus_messages
They are licensed under the GNU GPL v3 -- see LICENSE-GPL-3.0.txt.
They are distributed unmodified as separate data files and are not part of the
MIT-licensed firmware.

SPUD-Dash.dbc is part of this project (MIT).
