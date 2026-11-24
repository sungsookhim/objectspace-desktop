# Writing a WPS plug-in

A plug-in adds classes to WPS: new kinds of objects, or new behaviour for
existing ones. It runs in its own sandboxed process (`wps-plughost`: no
network, files limited to the system's libraries, the plug-in and its data
folder), and a call that takes longer than 2 seconds falls back to the
parent class's behaviour. See `docs/object-properties-design.md`, 2.7.

## Start

```
wpsctl plugin new MyNotes OsdDataFile     # a folder MyNotes/ with a skeleton
cd MyNotes && make && make install       # into ~/.config/wps/plugins
```

Log out and in again (or restart `wpsd`): `wpsctl stats` lists it under
`plugins`.

## What a class declares (its `.osdi`)

```
class OsdImageFile : OsdDataFile
version 1.0
types image/png, image/jpeg, image/*      # data files of these types are its objects
property rating : choice "none,1,2,3,4,5" = none page Image label Rating
event image.rated                         # an event of its own (on:image.rated actions)
override osdModifyPopupMenu                # its code, in its host
override osdMenuItemSelected
classoverride osdclsQueryTitle
```

- **Properties** are kept by WPS: they appear on the object's notebook,
  can be set with setup strings (`wpsctl set FILE 'rating=4'`), and for
  all objects of the class or a type through Class... (`class:OsdImageFile`),
  with no code. Its own code reads them from the object's state
  (`osd_state_get_str(self, "OsdImageFile", "rating", &v)`) and changes
  them with `osd_state_put_str`: the changes go back to WPS.
- **Methods** it overrides run in its host: menus, menu choices, settings
  pages of its own, setters named on its fields.
- The full grammar is at the top of `osdc.py`; the built-in classes'
  interfaces are in `classes/` here.

The sample (`sample/`) is a complete plug-in: `make OSD_SDK=.. OSD_INC=...`.
