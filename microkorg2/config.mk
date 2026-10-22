##############################################################################
# Vektor project configuration (microKORG 2 oscillator)
#

PROJECT := Vektor
PROJECT_TYPE := osc

# C sources (the microKORG 2 runtime doesn't provide the pitch table, so it's compiled in)
CSRC = header.c \
       $(realpath $(PLATFORM_COMMON_PATH)/dsp/midi_to_hz_lut.c)

# C++ sources
CXXSRC = unit.cc

ACSRC =
ACXXSRC =
ASMSRC =
ASMXSRC =

# Shared Vektor DSP core (core/vektor.h)
UINCDIR = $(PROJECT_ROOT)/../core

ULIBDIR =

ULIBS  = -lm
ULIBS += -lc

# -O3 rather than the default -Os: the oscillator runs once per voice
OPTIM = -O3

UDEFS =
