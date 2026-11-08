# Path to the Rack SDK (override on the command line: make RACK_DIR=/path/to/Rack-SDK)
RACK_DIR ?= ../..

SOURCES += $(wildcard src/*.cpp)

DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)

include $(RACK_DIR)/plugin.mk
