## New Features

- Remap modifiers for modulation sources
    - **In one line:** Random and MIDI (a controller or aftertouch, with Slew) sources came as card types, and each
      modulation now has a curve of its own (an envelope segment's bend, in the MODS view); what is left is slewing
      an LFO's or envelope's output, and a freely drawn curve shared between modulations.
    - **Feasible, medium.** A source type whose input is another source (picked like an aux) and whose output is that
      source's slewed or through a drawn curve; it runs after its input in the voice's source loop
      (`Synth::renderBlock()`), so an input in a later place is a block late. 7 spare ids a place are left.


## Issues 

## Improvements 

## Architecture

- Linux support and macOS support (maybe later)

## Organziational

- Decide on names for the major UI elements to put into documentation
    - **In one line:** add a glossary at the top of `docs/features.md`; most names are already used there.
    - **Feasible, small** (a decision; a draft is quick). features.md already names most of them; a glossary at
      its top would fix them: header, preset bar, view tabs, routing panel (graph, matrix), inspector, FX chain
      and cards, modulation panel and source cards, source handle, depth handle, modulation ring, keyboard strip
      (note source, macros, wheels, keys), the shape, partial and curve editors, and the settings dialog.
    - Decided: the docs say "modulation" (not "automation", which in a DAW means host automation) and "depth
      handle" (not "blip" or "dot"). The modulation list's tab is **MODS** for now, apart from MATRIX (the routing
      grid). features.md says "depth handle" in places now, but its Modulation section still introduces them as
      small dots.

