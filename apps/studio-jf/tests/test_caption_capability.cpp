// A LABEL IS A CAPTION, AND A CAPTION WITH A LINK IS A HYPERLINK.
//
// There was a separate Hyperlink widget subclassing Label to add a Link. The split cost more than it
// carried: every caption behaviour had to be listed by TYPE NAME in the surface, and the double-click that
// opens a caption for typing was one of those lists — so the Hyperlink was the one caption on a page you
// could not edit. One widget now, and caption behaviour is a capability the widget declares.
#include "surface/WidgetRegistry.h"
#include "surface/CanvasWidget.h"
#include <j/core/SceneGraph.h>
#include <cstdio>
int main() {
    jf::JSceneGraph g;
    int fails = 0;
    auto ck = [&](bool ok, const char* what) {
        std::printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL"); if (!ok) ++fails; };
    ck(widgetEditsCaption("label"),     "a Label edits its caption in place");
    ck(widgetTitle("hyperlink").empty(), "the Hyperlink type is gone — a Link is a Label's property");
    ck(!widgetEditsCaption("configedit"), "a value field does not");
    ck(!widgetEditsCaption("panel"),      "nor a container");
    std::printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "one caption widget, with or without a link",
                fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
