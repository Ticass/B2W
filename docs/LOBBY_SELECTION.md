# Converted maps in the Zombies globe menu

`build-mod` registers every converted project in `zm/mapstable.csv` and
`zm/gametypestable.csv`. One start location (`default`) represents the full
WaW map, with `zclassic` as its default four-player co-op mode. Custom entries
use content index zero, so selection does not depend on owning a BO2 DLC.
Stock entries remain intact and repeated staging does not duplicate a map.

The tables are linked into both `mod.ff` and a small frontend `mod_load.ff`.
Plutonium loads the latter while selecting a mod, before a map is running;
the gameplay copy keeps the metadata available when returning to the lobby.
Packaging requires and installs both files. The native globe/start-location
menus continue to handle party settings and starting the match.

OAT sorts independent asset-search templates. The gameplay build supplies a
table-only priority layer; shader activation also carries the generated tables.
The frontend build uses its project folder as the first lookup root. Both final
fastfiles are extracted and checked for the custom map/location/default mode,
so successfully linking stock tables cannot silently pass the build.

WaW maps do not supply BO2 globe illustrations. The menu uses the existing
BO2 empty map frame for custom-map background/card materials and a location
dot; the project identifier supplies the label. This is frontend scaffolding,
not a replacement for converted world assets. A future source artwork importer
can replace these generated material aliases without changing registration.

To select a converted map: load its mod in Zombies, open Custom Games and
Change Map, choose its project name on the globe, then choose its play area
and Classic mode. Everyone joining should install/load the same converted mod.
The packaging command prints a lobby launch command; direct LAN/devmap startup
is available explicitly through `launch_command(..., direct_map=True)`.

Build verification covers table preservation, valid counts/indices, matching
location/default-mode keys, idempotence, required frontend packaging, and
extracting the installed frontend tables/materials. An actual online co-op
session remains a separate runtime check.
