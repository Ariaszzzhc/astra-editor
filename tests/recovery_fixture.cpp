#include <astra/recovery/store.hpp>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>

int wmain(int count, wchar_t** arguments) {
    if (count != 2) { return 71; }
    (void)_setmode(_fileno(stdout), _O_BINARY);
    try {
        astra::recovery::RecoveryStore store{std::filesystem::path(arguments[1])};
        astra::Document document({42}, "unsaved \xf0\x9f\x98\x80\r\n");
        document.set_selections({{{8}, {12}}});
        store.checkpoint({document.snapshot(), {}, astra::io::TextEncoding::utf8, {}});
        std::cout << store.session_id() << '\n' << std::flush;
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what();
        return 72;
    }
}
#else
#error The recovery crash fixture is Windows-only in the current validation suite.
#endif
