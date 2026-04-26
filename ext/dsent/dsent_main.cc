#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include "DSENT.h"
#include "libutil/String.h"

using namespace DSENT;

int main(int argc, char **argv)
{
    if (argc < 3 || std::string(argv[1]) != "-cfg") {
        std::cerr << "Usage: dsent -cfg <config_file> "
                  << "[-overwrite \"Key1=Val1; Key2=Val2;\"]" << std::endl;
        return 1;
    }

    const char *cfg_file = argv[2];

    std::map<String, String> overwrite;
    for (int i = 3; i < argc - 1; i++) {
        if (std::string(argv[i]) == "-overwrite") {
            String ow_str = argv[i + 1];
            std::vector<String> pairs = ow_str.split(";");
            for (auto &p : pairs) {
                p.trim();
                if (p.size() == 0) continue;
                std::vector<String> kv = p.split("=");
                if (kv.size() == 2) {
                    kv[0].trim();
                    kv[1].trim();
                    overwrite[kv[0]] = kv[1];
                }
            }
        }
    }

    Model *model = DSENT::initialize(cfg_file, overwrite);
    std::map<std::string, double> outputs;
    DSENT::run(overwrite, model, outputs);

    for (auto &kv : outputs) {
        std::cout << kv.first << " = " << kv.second << std::endl;
    }

    DSENT::finalize(overwrite, model);
    return 0;
}
