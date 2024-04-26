.PHONY: all clean

INCLUDE = include
EXAMPLES = examples
BUILD = build
ALL_TARGET_NAME = cat echo echo_coroutine multi_task_test

all: $(ALL_TARGET_NAME)

clean:
	@rm -r $(BUILD)

%: $(EXAMPLES)/%.cpp
	@mkdir -p $(BUILD)
	$(CXX) -std=c++20 -Wall -Wextra -g -I$(INCLUDE) $^ -luring -o $(BUILD)/$@
