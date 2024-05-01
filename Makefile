.PHONY: all clean

INCLUDE = include
EXAMPLES = examples
BUILD = build
ALL_TARGET_NAME = cat echo echo_coroutine multi_task_test \
	feature_multishot feature_multishot2 feature_sqpoll   \
	feature_io_drain feature_io_link feature_provided_buffers

all: $(ALL_TARGET_NAME)

clean:
	@rm -r $(BUILD)

%: $(EXAMPLES)/%.cpp
	@mkdir -p $(BUILD)
	-$(CXX) -std=c++20 -Wall -Wextra -g -I$(INCLUDE) $^ -luring -o $(BUILD)/$@
