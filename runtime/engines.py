"""Stable engine IDs used in preferences and launch adapters."""
# Appended only: preferences and game.ini store the index. RG DS Play (last)
# plays every system but the DS and is never offered as a DS engine.
# PortMaster (appended after it) runs ports, the scripts of the Ports system.
CORES = ("drastic", "dsperate", "nnddss", "melonds", "rgds-experimental", "rgds-play", "portmaster")

# DSCore (rgds-experimental, the DSperate fork) is the only DS engine. The others
# keep their slots so stored indexes keep their meaning; a launch refuses them.
RETIRED = ("drastic", "dsperate", "nnddss", "melonds")
DSPERATE_CORES = ("rgds-experimental",)
