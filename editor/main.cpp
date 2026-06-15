// liminal-editor entry point.
//   liminal-editor                      -> landing screen (project chooser)
//   liminal-editor --project <path>     -> opens project.ljson (or its dir)
//   liminal-editor --empty              -> no project, blank editor scene
//   liminal-editor --sample             -> opens the bundled sample project
#include "editor_app.hpp"

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    std::string project; // empty -> landing screen
    bool empty = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--project") == 0 && i + 1 < argc) {
            project = argv[++i];
        } else if (std::strcmp(argv[i], "--sample") == 0) {
            project = LIMINAL_EDITOR_SAMPLE_PROJECT;
        } else if (std::strcmp(argv[i], "--empty") == 0) {
            empty = true;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            std::printf("usage: liminal-editor [--project <project.ljson|dir>] "
                        "[--sample] [--empty]\n");
            return 0;
        }
    }
    liminal::editor::EditorApp app(project, empty);
    app.run();
    return 0;
}
