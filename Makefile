# Static library target.
TARGET := libavrsim.a

# Sources, listed rather than wildcarded so that adding a file is a visible change in a diff.
SOURCE_FILES := source/avrsim/mcu.cpp \
                source/avrsim/pin.cpp \
                source/avrsim/stackwatch.cpp \
                source/avrsim/trace.cpp

# Where the objects and their dependency files are written, so that the source tree holds
# sources and nothing else. The layout under it mirrors source/.
BUILD_DIR := build

OBJECT_FILES := $(SOURCE_FILES:source/%.cpp=$(BUILD_DIR)/%.o)

# C++ compiler.
CXX_COMPILER := g++

# C++ compiler flags. -Wall -Werror because this is shipped code the reader never edits: a
# warning here is our bug, not theirs.
# -MMD -MP writes a .d file beside each object listing the headers it included, so that editing
# include/avrsim/mcu.hpp rebuilds mcu.o instead of leaving a stale one in the tree.
CXX_FLAGS := -std=c++17 -Wall -Werror -Iinclude -MMD -MP

# The measuring instrument the cross-check exercises use.
TOOL := avrsim
TOOL_SOURCES := source/main.cpp \
                source/avrsim/utils.cpp

TOOL_OBJECT_FILES := $(TOOL_SOURCES:source/%.cpp=$(BUILD_DIR)/%.o)

# Build the library and the tool as default. The tool links the library, so the ordering is a
# real dependency and not a convenience: it is expressed in $(TOOL)'s prerequisites rather than
# in this list, so that `make -j` cannot start the link before the archive exists.
default: lib tool

# Build the static library.
lib: $(TARGET)

# Build the tool.
tool: $(TOOL)

# Build the library and the tool the way CI does, from a clean tree.
build:
	@bash ci/build.sh

$(TOOL): $(TOOL_OBJECT_FILES) $(TARGET)
	@$(CXX_COMPILER) $(TOOL_OBJECT_FILES) $(TARGET) -o $@ -lsimavr -lelf

$(TARGET): $(OBJECT_FILES)
	@ar rcs $@ $^

$(BUILD_DIR)/%.o: source/%.cpp
	@mkdir -p $(dir $@)
	@$(CXX_COMPILER) $(CXX_FLAGS) -c $< -o $@

# The generated header dependencies, one beside each object under $(BUILD_DIR). Absent on a
# first build, which is why this is -include.
-include $(OBJECT_FILES:.o=.d) $(TOOL_OBJECT_FILES:.o=.d)

# Reformat every source and header in place, and say which ones changed.
format:
	@bash ci/format.sh

# Report what is not formatted and fail, changing nothing. This is what CI runs.
check-format:
	@bash ci/format.sh --check

# Remove the library, the tool, and everything the build wrote.
clean:
	@rm -rf $(BUILD_DIR)
	@rm -f $(TARGET) $(TOOL)

.PHONY: default lib tool build format check-format clean
