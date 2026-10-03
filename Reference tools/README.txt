BLACK OPS II LIGHTING -- the lighting-only parts of the COD Universal Map Porting Tool
Map porter project by aidenwrld
===================================================================================

These files write a ported map's lighting in Black Ops II's formats. In the tool they read Black Ops
(BO1, "t5") lighting on one side and write Black Ops II ("t6") on the other. For a World at War ("t4")
port, keep the Black Ops II writing side and replace the Black Ops reading side with World at War's.

Left out on purpose (Black Ops only):
  - lightmap_t5.py  -- reads Black Ops' world lightmap layout
  - bo1_grade.py    -- fits Black Ops' colour look (LUT + bloom) into Black Ops II visions
  - the lit-shader converter (shaders/lit.py, hlsl3d.py, sm3.py) -- carries Black Ops' own shaders

The folder keeps the tool's package layout (codport/...) so the imports work as they are. Python 3.10+;
lightgrid.py's coefficient fitting uses numpy.


THE FILES
---------

codport/bo2/lightgrid.py -- the baked light grid (lights models, characters and players)
  KEEP (Black Ops II side):  LightGrid, LightGridSample, LightGridEntry, encode_coeff, encode_t6_colors,
                             fit_t6_coeffs, sh_basis, grid_basis_dirs, write_light_grid, read_light_grid
                             (output: BSP/map_lightgrid.bin)
  REPLACE (Black Ops side):  decode_t5_colors, collect_light_grid -- they read Black Ops' grid out of the
                             decoded world. Write World at War's equivalent and feed LightGrid.

codport/bo2/primary_lights.py -- the map's own lights (spot, omni, the sun) and their light-def images
  KEEP:     PrimaryLights, LightDef, write_primary_lights, dds_to_iwi
  REPLACE:  convert_light, read_light_defs, collect_primary_lights -- Black Ops' light fields and units

codport/bo2/hero_lights.py -- lights the weapon in the player's hands (the world's hero lights)
  KEEP:     write_hero_lights, sun_direction
  CHECK:    sun_hero_light uses t5_sun_colour (Black Ops reads `suncolor` as sRGB); confirm how World at
            War reads it before reusing. source_hero_lights reads Black Ops' hero lights -- replace.

codport/bo2/exposure.py -- exposure and exposure volumes (how bright each area renders)
  KEEP:     Exposure, ExposureVolume, t6_hdr_control, write_exposure
  REPLACE:  t6_exposure / t5_hdr_control (Black Ops' unit: hdrControl0.x = exposure / 8) and
            collect_exposure. World at War has no HDR exposure of the same kind -- work out what its
            brightness maps to before porting this one.

codport/gsc/client_sun.py -- the sun's brightness reference, as a client script helper
  KEEP:     helper_source (writes the Black Ops II client script)
  REPLACE:  sun_reference (reads Black Ops' worldspawn keys)

codport/gsc/client_fog.py -- the map's fog, as client script for Black Ops II

codport/convert/vision.py -- Black Ops II vision files (colour grading) for the map
  All Black Ops II side: recovers a stock vision as a template and writes vision/<map>.vision.

Support files (only so the above load; not lighting themselves):
  codport/formats/dds.py, codport/codir/*.py  -- DDS reading, used by primary_lights.dds_to_iwi
  codport/core/errors.py                      -- the tool's finding/severity types, used by vision.py
  codport/bo2/native_world.py                 -- NOT the tool's world builder: just the two helpers
                                                 hero_lights.py imports from it


HOW THE TOOL CHECKED THESE
--------------------------
Formats were read from the games themselves, not guessed: shader disassembly of the stock technique
sets (D3DDisassemble), the Black Ops II engine decompile, and LAN-only frame captures comparing the
two games draw for draw. Do the same for World at War's side before trusting any conversion factor.
