.PHONY: all clean

INCLUDE = include
EXAMPLES = examples
BUILD = build
ALL_TARGET_NAME = cat echo echo_coroutine \
	feature_multishot feature_multishot2 feature_sqpoll \
	feature_io_drain feature_io_link feature_provided_buffers \
	test_multi_task test_context_switch

CXX_FLAGS = -std=c++20 -Wall -Wextra -g -I$(INCLUDE) $^ -luring -pthread
CXX_FLAGS_DEBUG =

all: $(ALL_TARGET_NAME)

clean:
	@rm -r $(BUILD)

%: $(EXAMPLES)/%.cpp
	@mkdir -p $(BUILD)
	-$(CXX) $(CXX_FLAGS) $(CXX_FLAGS_DEBUG) -o $(BUILD)/$@
