# Frappy configuration for one mm4 pressure controller.
#
# Set the board's address in MM4_URI (or edit the default below): the
# address shown on the board's screen, port 5000. The board takes one
# connection at a time, so close any terminal or logger on port 5000 first.
#
# MM4_COMMANDS: '0.12' for the firmware as built (0.12.19 to 0.12.21),
# 'plan' once the board has the remote command set.

import os

Node('mm4.cryostat',
     'mm4 pot pressure controller (needle valve)',
     'tcp://10767',
)

Mod('mm4_io',
    'frappy_mm4.mm4.Mm4IO',
    'text link to the mm4',
    uri=os.environ.get('MM4_URI', 'tcp://192.168.1.50:5000'),
    command_set=os.environ.get('MM4_COMMANDS', '0.12'),
)

Mod('pressure',
    'frappy_mm4.mm4.Pressure',
    'pot pressure, setpoint and MANUAL / AUTO',
    io='mm4_io',
    pollinterval=1,
    tolerance=0.5,
)

Mod('valve',
    'frappy_mm4.mm4.Valve',
    'needle valve position, set in MANUAL',
    io='mm4_io',
    pollinterval=2,
)
