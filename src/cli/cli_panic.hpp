// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 miosal@cadml.org

#pragma once

// Installs a std::terminate handler that prints any in-flight
// exception's message to stderr as `error: <what>` before letting
// the runtime exit. Without this, an uncaught std::runtime_error
// (from a Lua script crash, a Manifold init failure, or any
// std::bad_alloc from a hostile input) surfaces as a libc++abi
// terminate trace — useless to the user.
//
// Tools opt in with a single line in main():
//   cadml::cli::install_panic_handler();
// Place it as the first statement of main(). The handler is global
// and idempotent; calling install_panic_handler() more than once
// is harmless.

#include <cstdio>
#include <exception>
#include <mutex>

namespace cadml::cli {

inline void install_panic_handler() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        std::set_terminate([]() {
            const auto ep = std::current_exception();
            if (ep) {
                try {
                    std::rethrow_exception(ep);
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "error: %s\n", e.what());
                } catch (...) {
                    std::fprintf(stderr, "error: unrecognised exception\n");
                }
            } else {
                std::fprintf(stderr, "error: terminated without a "
                                       "live exception\n");
            }
            std::_Exit(2);
        });
    });
}

}  // namespace cadml::cli
