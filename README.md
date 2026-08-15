# PulseView

## Personal fork

This is my personal AI-slop fork of PulseView. It contains experimental,
AI-assisted changes shaped around my own workflows and should not be mistaken
for an official sigrok release.

## What has changed

Compared with [upstream PulseView](https://github.com/sigrokproject/pulseview),
this fork currently adds:

- An optional embedded MCP server and stdio bridge for AI-assisted inspection
  and control of running PulseView sessions. It can list sessions, report view
  context, query paginated protocol-decoder annotations, place cursors, and
  zoom to a sample range.
- Better behavior with large captures, including faster edge lookup for cursor
  interaction and bounded annotation rendering when decoder traces are zoomed
  out.
- Decoder fixes, including a crash fix for stacked protocol decoders and
  decoder rows that remain anchored by default.
- Interactive time and edge-count measurements directly on logic traces.
- Persisted sample-rate and sample-count settings across sessions.
- Several memory-leak fixes in decoder, input-file, widget, and logic-trace
  code.

The [sigrok project](https://sigrok.org/) aims to create a portable,
cross-platform, Free/Libre/Open-Source signal-analysis suite that supports
logic analyzers, oscilloscopes, multimeters, and other devices.

PulseView is a Qt-based logic-analyzer, oscilloscope, and MSO GUI for sigrok.

## Status

PulseView is in a usable state and has had official tarball releases.

## Copyright and license

PulseView is licensed under the terms of the GNU General Public License,
version 3 or later ([GPL-3.0-or-later](https://spdx.org/licenses/GPL-3.0-or-later.html)).

Some individual source files are licensed under GPL-2.0-or-later,
GPL-3.0-or-later, or MIT. This does not change the fact that the program as a
whole is licensed under GPL-3.0-or-later, including because it links against
GPL-3.0-or-later libraries.

See the individual source files for the full list of copyright holders.

## Copyright notices

A copyright notice indicating a range of years must be interpreted as having
had copyrightable material added in each of those years. For example:

```text
Copyright (C) 2010-2013 Contributor Name
```

is interpreted as:

```text
Copyright (C) 2010, 2011, 2012, 2013 Contributor Name
```

## Resource authors and licenses

The following icons come from the
[Tango Icon Library](http://tango.freedesktop.org/Tango_Desktop_Project) and
are in the public domain:

- `icons/application-exit.png`
- `icons/document-new.png`
- `icons/document-open.png`
- `icons/document-save-as.png`
- `icons/edit-paste.svg`
- `icons/help-browser.png`
- `icons/media-playback-pause.png`
- `icons/media-playback-start.png`
- `icons/preferences-system.png`
- `icons/search.svg`
- `icons/window-new.png`
- `icons/zoom-fit-best.png`
- `icons/zoom-in.png`
- `icons/zoom-out.png`

Additional resources:

- `icons/information.svg` by
  [Bobarino](https://en.wikipedia.org/wiki/File:Information.svg), licensed
  under GFDL 1.2 or later / CC BY-SA 3.0. See the
  [file's licensing information](https://en.wikipedia.org/wiki/File:Information.svg#Licensing).
- `icons/add-math-channel.svg` by
  [Inductiveload](https://en.wikipedia.org/wiki/File:Icon_Mathematical_Plot.svg),
  public domain.
- [QDarkStyleSheet](https://github.com/ColinDuquesnoy/QDarkStyleSheet) by
  Colin Duquesnoy, licensed under
  [CC BY 4.0](https://github.com/ColinDuquesnoy/QDarkStyleSheet/blob/master/LICENSE.md).
- [DarkStyle](https://github.com/Jorgen-VikingGod/Qt-Frameless-Window-DarkStyle)
  by Juergen Skrotzky, licensed under the
  [MIT License](https://github.com/Jorgen-VikingGod/Qt-Frameless-Window-DarkStyle#licence).
- [QHexView](https://github.com/virinext/QHexView) by Victor Anjin, licensed
  under the [MIT License](https://github.com/virinext/QHexView/blob/master/LICENSE).
- [ExprTk](https://www.partow.net/programming/exprtk/index.html) by Arash
  Partow, licensed under the MIT License.

## Community

- [Mailing list](https://lists.sourceforge.net/lists/listinfo/sigrok-devel)
- IRC: `#sigrok` on [Libera.Chat](https://libera.chat/)
- [PulseView website](https://sigrok.org/wiki/PulseView)
