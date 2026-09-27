# Panel/Instance Model — single definition, multiple views

Status: **BUILT** (the full §1 inversion shipped 2026-06-20). Supersedes the earlier node→dashboard
page model for the right-hand content area. The `Dashboard` widget is deleted; the
tree is a pure palette/drag-source; all content is surfaces. Node content lives in the panel library
(a node = a definition). Authoring decision: selecting a node does NOT auto-open a view ("pure
palette") — author by dragging onto a surface; legacy per-node `pages` JSON is dropped on load.

## 1. The inversion

Today: each tree node lazily owns a `Dashboard` page; selecting a node swaps the right-hand
`QStackedWidget` to that page. The tree *navigates* content and tabs would mirror it.

New model:

- A **tree node = a panel definition** — a reusable composition of controls (with layout +
  bindings). Authored once, referenced many times.
- A **surface (per tab) = a composition of placements**, each a reference `{ definitionId, rect }`.
- The **tree is a palette / drag-source** (like the dictionary dock is for channels), not a navigator.
- **Tabs are user-composed, treeview-free**, and can be **loaded/unloaded at runtime**.

Drag node `test` onto tab 1 and tab 3 at the same position, omit it from tab 2 → flipping
tabs makes that panel appear anchored while the rest of each surface differs. The old "floating
panel above the tabs" effect falls out of this for free, with no floating layer.

## 2. Document–View (MVC), strategy (a)

A QWidget can exist in only one place, so the definition is **not** any instance widget — it is a
shared **model** that views render. Editing through *any* view in edit mode mutates the model;
every view re-renders. "Single definition, multiple panels view it."

```
            PanelDefinition  (model: elements + layout + bindings)
             ▲   │   ▲
       edit  │   │   │  edit          edits route through MODEL MUTATIONS, not widget pokes
             │   ▼   │
     ┌───────┴──┐ ┌──┴───────┐ ┌──────────┐
     │ view A   │ │ view X   │ │ view …   │   all re-render on model change
     │ (tab 1)  │ │ (tab 3)  │ │ (nested) │
     └──────────┘ └──────────┘ └──────────┘
```

Two layers, synced differently:

| Layer | Source of truth | Sync |
|---|---|---|
| **Definition** — layout, elements, bindings | the `PanelDefinition` model | views subscribe to model change signals |
| **Runtime** — live values, config being tuned | `Cache` / config singletons | inherently shared (controls bind by signal name) |

So only the *definition* is the new shared model; runtime data sharing already exists.

We chose strategy (a) — the true model — over the lighter "authoritative-editing-instance +
re-serialize" because it is the correct foundation for nesting, any number of simultaneously-live
views, and future per-instance overrides. The cost is that editing moves from "manipulate live
widgets, serialize later" to "mutate model, views react" (§7).

## 3. One model type for everything: `PanelDefinition`

A tab surface, a node panel, and a nested panel are structurally identical — all are compositions
of elements. So they are all `PanelDefinition`s, each with a stable `definitionId`.

```
PanelDefinition {
  id            : stable definition id (node path, or generated id for a tab root)
  layout        : { mode: free|flowed|grid, canvasSize, static, ... }
  elements      : [ Element ]
}

Element = Control   { type, bindings, props, rect|slot }      // a leaf widget
        | Placement { definitionId, rect|slot }               // a reference to another definition
```

- **Placement** is what enables both reuse-across-tabs and **nesting** (a definition referencing
  other definitions).
- `definitionId` for a node panel is the node path; a tab's root surface owns a generated-id
  definition.

## 4. The view: `PanelView`

`PanelView` is a widget that renders one `PanelDefinition`:

- builds child widgets — `CanvasElement`s for controls, **nested `PanelView`s** for placements
- subscribes to the definition's change signals; updates on mutation
- in edit mode, user gestures (drop / drag / resize / select / bind) translate to **model
  commands**, never direct widget edits
- lazily instantiated when its tab/placement first becomes visible

A **tab surface** is a `PanelView` over a root definition. A **placement** renders as a nested
`PanelView` over the referenced definition. Editing through a nested view mutates *that*
definition, so the change propagates to every view of it (inside this tab and elsewhere).

## 5. Mutations as undoable commands

Edits are commands on a `PanelDefinition` (`addElement`, `moveElement`, `setProp`, `addPlacement`,
`removeElement`, `reorder`, …). These slot into the existing `QUndoStack` / `QUndoGroup`
architecture — model mutations become undoable, and the existing tree-vs-tune undo split extends
naturally to definition edits. On mutation the definition emits granular change signals; all its
`PanelView`s react.

## 6. Nesting + cycle detection

A placement references a `definitionId`; the placement graph **must be acyclic** (A→B→A forbidden).

- Detect on `addPlacement`: a would-be cycle is rejected (with a user-visible reason).
- Optional max nesting depth as a second guard.
- Rendering recursion is bounded by acyclicity; lazy instantiation keeps deep trees cheap.

## 7. Runtime tabs (load / unload)

Tabs are a **dynamic, user-managed set at runtime**, not just in edit mode.

- A **tab = { name, rootDefinitionId }** — a *view* onto a root `PanelDefinition`.
- **Load a tab** = open a view onto a (new or existing library) root definition.
- **Unload a tab** = close the view. The definition **persists in the library**; only the tab view
  is removed. Document–view makes this clean: opening/closing tabs never destroys definitions.
- Workspace state stores the open-tab list (order + which definition); the definition library is
  stored separately.

## 8. Persistence shape

```
{
  "definitions": { "<defId>": PanelDefinition, ... },   // the library (nodes + tab roots + nested)
  "tabs":        [ { "name": "...", "defId": "..." } ], // open-tab views, ordered
}
```

A root surface definition embeds `Placement` elements referencing other `defId`s plus loose
`Control` elements. Placements store only `{ definitionId, rect }` — tiny; the widgets are rebuilt
from the referenced definition.

## 9. Existing mechanisms — extend, don't reinvent

Per the research-before-new-mechanisms discipline:

- **`CanvasElement`** (controls: paint / live data / units / conditions) — **kept as-is**; it is
  the leaf control rendered by a `PanelView`. `sizeHint()`/`minimumSizeHint()` (already added) let
  it sit in flowed layouts.
- **`Dashboard`** (free-placement canvas) — **becomes a `PanelView`** over a free-mode root
  definition; its scale/letterbox stays as the free-layout renderer. Its `pages_` node→widget map
  is replaced by the definition **library** + tab views.
- **`PanelWidget`** (managed-layout container of child elements) — its layout engine
  (`relayoutChildren`/`naturalOf`/`fit`) **feeds the flowed `PanelView` renderer**; `PanelWidget`
  and `PanelView` converge (both are "container of elements"). Reuse, don't duplicate.
- **`CanvasElement::save()/load()` JSON** — becomes the **definition serialization** shape.
- **`QUndoStack`/`QUndoGroup`** — hosts the model-mutation commands (§5).
- **Properties dock** — today takes `CanvasElement*` and pokes widget props. Under strategy (a)
  it edits the **selected element's model** in its `PanelDefinition`; the view reflects the model
  change. This is the component that changes most.
- **Dictionary-dock drag** (`kChannelMime`) — mirrored by a **node mime** so the tree is a
  drag-source for placements.

## 10. Honest costs

1. **Edit path rework.** Moving from direct-widget editing to model-mutation editing touches the
   Dashboard edit loop and (most of all) the Properties dock. This is the core of the work.
2. **Re-render churn.** A definition edit updates every view of it. Cheap given save/load is solid,
   but a view loses transient UI state on update (selection is irrelevant; an in-progress inline
   text edit on the same definition in another visible view is the rare wrinkle).
3. **`onNodeSelected` structural change.** The tree stops driving a stack page; it becomes a
   palette/drag-source. The right-hand area becomes user-composed runtime tabs.
4. **Cycle/depth guards** are mandatory once nesting exists (§6).

## 11. Staged roadmap

1. **Model + library.** Define `PanelDefinition` (data + change signals) and the definition
   library; route save/load through them. One view per definition (no behaviour change yet).
2. **`PanelView` renderer.** Render a definition into widgets (controls first, no placements).
3. **Mutation/command layer.** Route edits through undoable model commands; prove the loop on a
   single view.
4. **Multi-view + propagation.** Multiple `PanelView`s of one definition; edit one, others react.
5. **Placements + nesting + cycle detection.** A definition can reference others; drag a node →
   placement; reject cycles.
6. **Runtime tabs.** User add/remove tab views onto root definitions; persist the open-tab set.
7. **Properties dock → model.** Final rewiring so property edits mutate the model.

Each stage is independently buildable and reviewable; later stages don't block earlier value.
