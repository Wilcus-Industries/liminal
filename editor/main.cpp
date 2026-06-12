// liminal-editor entry point.
//   liminal-editor                      -> opens the bundled sample project
//   liminal-editor --project <path>     -> opens project.ljson (or its dir)
//   liminal-editor --empty              -> no project, empty scene
#include "editor_app.hpp"

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    std::string project = LIMINAL_EDITOR_SAMPLE_PROJECT;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--project") == 0 && i + 1 < argc) {
            project = argv[++i];
        } else if (std::strcmp(argv[i], "--empty") == 0) {
            project.clear();
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("usage: liminal-editor [--project <project.ljson|dir>] "
                        "[--empty]\n");
            return 0;
        }
    }
    liminal::editor::EditorApp app(project);
    app.run();
    return 0;
}
