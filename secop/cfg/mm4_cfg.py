# Frappy configuration for one mm4 pressure controller.
#
# Set the board's address in MM4_URI (or edit the default below): the
# address shown on the board's screen, port 5000. The board takes one
# connection at a time, so close any terminal or logger on port 5000 first.
#
# Needs the firmware with the remote command set (SP, MODE AUTO, STOP, ...).

import os

Node('mm4.cryostat',
     'mm4 pot pressure controller (needle valve)',
     'tcp://10767',
)

Mod('mm4_io',
    'frappy_mm4.mm4.Mm4IO',
    'text link to the mm4',
    uri=os.environ.get('MM4_URI', 'tcp://192.168.1.50:5000'),
    export=False,   # no raw text commands over SECoP
)

Mod('pressure',
    'frappy_mm4.mm4.Pressure',
    'pot pressure; writing the setpoint starts AUTO',
    io='mm4_io',
    output_module='valve',
    pollinterval=1,
    tolerance=0.5,
)

Mod('valve',
    'frappy_mm4.mm4.Valve',
    'needle valve position; writing it ends AUTO',
    io='mm4_io',
    pollinterval=2,
)

Mod('room_pressure',
    'frappy_mm4.mm4.RoomPressure',
    'room pressure (Pressure 21 Click); leave out if not fitted',
    io='mm4_io',
    pollinterval=10,
)
