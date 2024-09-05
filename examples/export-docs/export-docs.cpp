#include "common.h"
#include "llama.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

// Export usage message (-h) to markdown format

static void export_md(std::string fname, llama_example ex) {
    std::ofstream file(fname, std::ofstream::out | std::ofstream::trunc);

    gpt_params params;
    auto options = gpt_params_parser_init(params, ex);

    file << "| Argument | Explanation |\n";
    file << "| -------- | ----------- |\n";
    for (auto & opt : options) {
        file << "| `";
        // args
        for (const auto & arg : opt.args) {
        if (arg == opt.args.front()) {
                file << (opt.args.size() == 1 ? arg : (arg + ", "));
            } else {
                file << arg << (arg != opt.args.back() ? ", " : "");
            }
        }
        // value hint
        std::string md_value_hint(opt.value_hint);
        string_replace_all(md_value_hint, "|", "\\|");
        file << " " << md_value_hint;
        // help text
        std::string md_help(opt.help);
        string_replace_all(md_help, "\n", "<br/>");
        string_replace_all(md_help, "|", "\\|");
        file << "` | " << md_help << " |\n";
    }
}

int main(int, char **) {
    export_md("autogen-main.md", LLAMA_EXAMPLE_MAIN);
    export_md("autogen-server.md", LLAMA_EXAMPLE_SERVER);

    return 0;
}