#pragma once

// The three trees built FROM a meta descriptor — the binding palettes the studio authors panels against:
//
//   buildDictionary  — telemetry channels + config scalars/tables, grouped by module: the draggable palette
//   buildSigilTree   — the sigils a widget's text fields can interpolate, for the {} picker
//   buildSourceTree  — an arbitrary path list arranged as a tree, for the source picker
//
// Pure functions of the meta (plus, for the sigil tree, the widget's own field list): no window, no app state.
// They were 200 lines in main.cpp purely because that is where the tree views are constructed.

#include "../model/MetaModel.h"

#include <j/core/JTreeView.h>   // JTreeViewNode

#include <string>
#include <vector>

// Flat list of every bindable path (telemetry channels + config scalars + tables) — the source picker's input.
std::vector<std::string> channelPaths(const MetaModel& meta);

// The CONFIGURATION half of the dictionary as a tree of its own — module ▸ Values / Tables / arrays ▸
// element ▸ field, every leaf carrying its binding path. What a field picker should show: a flat list
// cannot say which sensor or which cylinder a row belongs to. Config only, no telemetry — a preset
// WRITES fields, and a telemetry channel cannot be written.

// The dictionary's PcVariables category ends with this ghost row; activating it adds a variable. Same
// affordance the navigation tree uses for adding nodes.
const char* pcPlaceholderCaption();

JTreeViewNode buildConfigTree(const MetaModel& meta);
JTreeViewNode buildDictionary(const MetaModel& meta);
// `firmware` = the expression will run on the ECU. It changes what the tree OFFERS, not the grammar:
// the language's functions are listed with the ones that back end can actually emit, and the tables an
// expression can name get a group of their own. A host-evaluated expression (a visibility test, a data
// source) gets the host's function list and no tables — the studio has no VM to read one with.
JTreeViewNode buildSigilTree(const MetaModel& meta, const std::vector<std::string>& widgetFields,
                             bool firmware = false);
JTreeViewNode buildSourceTree(const std::vector<std::string>& paths, const MetaModel& meta);
