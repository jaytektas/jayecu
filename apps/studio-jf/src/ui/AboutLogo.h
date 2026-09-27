#pragma once

// AboutLogo — the Jaytek mark as raw RGBA, for JDialogRequest::imageRgba.
//
// THE FRAMEWORK'S MESSAGE DIALOG ALREADY SHOWS A PICTURE (jscope's About box is the case it was built
// for), and it draws it at its NATIVE size — one texel per pixel, no minification. That is what makes a
// logo look like the artwork rather than a photograph of it, and it is why this hands over pixels at the
// size they will be drawn instead of a big image and a hope.
//
// A dialog is its own window with its own GPU surface, so the request wants BYTES, not a texture handle
// from the main window's HAL. Decoded here, once, when the box is opened.

#include <j/core/Dialog.h>

#include <cstdint>
#include <string>
#include <vector>

namespace aboutlogo {

// Where the artwork is — resources::path, which looks beside the executable before the working
// directory. An About box that is blank depending on where you launched from is not worth having.
std::string path(const char* file = "jaytek-logo.png");

// The splash artwork as raw RGBA, for JSplashWindow. Empty on any failure, which the splash treats as
// "no splash" rather than a blank rectangle in the middle of the screen.
struct Pixels { std::vector<uint8_t> rgba; uint32_t w = 0, h = 0; };
Pixels load(const std::string& file);

// Fill a request's picture from the shipped PNG. A missing or unreadable file leaves the request without
// one and the dialog simply shows its text — the right trade for decoration.
void attach(jf::JDialogRequest& req);

}  // namespace aboutlogo
