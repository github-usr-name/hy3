#pragma once

#include <hyprland/src/debug/Log.hpp>

template <typename... Args>
void hy3_log(LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
	auto msg = std::vformat(fmt.get(), std::make_format_args(args...));
	Debug::log(level, "[hy3] {}", msg);
}

struct Hy3TraceContext {
private:
	std::string context;
	bool enabled;

public:
	Hy3TraceContext(std::string ctx) {
		context = ctx;
		trace("entered context");
	}

	template <typename... Args>
	Hy3TraceContext(std::string ctx, std::format_string<Args...> fmt, Args&&... args) {
		context = ctx;

        if(fmt.get() != "") {
            auto msg = std::vformat(fmt.get(), std::make_format_args(args...));
            trace("entered context, {}", msg);
        } else {
            trace("entered context");
        }
	}

	~Hy3TraceContext() {
		trace("exiting context");
	}

	template <typename... Args>
	void trace(std::format_string<Args...> fmt, Args&&... args) {
        auto msg = std::vformat(fmt.get(), std::make_format_args(args...));
        Debug::log(TRACE, "[hy3] [{}] {}", context, msg);
	}
};
