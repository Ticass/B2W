# Stage and link the WaW map through the OAT T6 BSP bridge.
#
#   powershell -ExecutionPolicy Bypass -File C:\WawConverter\tools\run_bridge.ps1
#
# Steps
#   0. Dump the WaW clipmap (v3: brushes, submodels, static model collision),
#      gfxworld and PathData (gameworldsp -> <map>.paths.json)
#      into the stage, and materials + images of the zones WaW loads with the
#      map (mod.ff, common.ff). The map zone only references some assets.
#   1. Dump stock BO2 materials + technique sets (+ shader_bin) from zm_nuked.ff
#      with the T6 OAT build. These are the donors for material translation and
#      the only source of compiled T6 technique sets the bridge can load.
#   2. stage-bridge: world FBX, collision FBX + brushes.json, static model
#      xmodels + models.json, world/model materials (per-material techset),
#      IWI images, scripts/rawfile, zone_source\<project>.zone.
#   3. bridge-link: run the T6 bridge Linker with ONLY the staged project and
#      the stock dump on the asset search path. Full log is written to a file.
param(
    [string]$Bo2 = 'C:\Program Files (x86)\Steam\steamapps\common\Call of Duty Black Ops II',
    [string]$Waw = 'C:\Program Files (x86)\Steam\steamapps\common\Call of Duty World at War',
    [string]$MapMod = 'C:\Users\thrif\AppData\Local\Activision\CoDWaW\mods\Nuketown Remastered 1.2',
    [string]$MapZone = 'nuketown',
    [string]$Stage = 'C:\WawConverter\work\final_stage',
    [string]$Project = 'zm_nuketown_waw',
    [string]$OatT4 = 'C:\WawConverter\vendor\OpenAssetTools\build\bin\Release_x86',
    [string]$OatT6 = 'C:\WawConverter\vendor\OpenAssetToolsT6\build\bin\Release_x86',
    [string]$StockZone = 'zm_nuked',
    [string]$Dump = 'C:\WawConverter\work\stock_t6_dump',
    [string]$WawDumps = 'C:\WawConverter\work\waw_zone_dumps',
    [string]$ModBuild = 'C:\WawConverter\work\mod_build',
    [switch]$Redump,
    # OPT-IN debugging aid, off by default (GUIDELINES 17): play unconverted WaW
    # effects as stock BO2 effects (compat/fx_fallback.json, each reported FX_FALLBACK)
    [switch]$FxFallback,
    # WaW Mod Tools (bin\linker_pc.exe). Effects in no compiled WaW zone are
    # compiled from their raw\fx sources with WaW's linker (reported
    # WAW_SOURCE_ASSET). On here because the user asked for them; the converter
    # itself keeps it opt-in (--waw-source-fx). -NoWawSourceFx turns it off.
    [string]$WawModTools = 'C:\WawConverter\wawModTools',
    [switch]$NoWawSourceFx,
    [string]$XwmaDecoder
)
$fxFallbackArgs = @()
if ($FxFallback) { $fxFallbackArgs = @('--fx-fallback') }
$sourceFxArgs = @('--waw-mod-tools', $WawModTools)
if (-not $NoWawSourceFx) {
    $sourceFxArgs += @('--waw-source-fx', '--waw-source-dumps', (Join-Path $WawDumps 'source'))
}
# native tools write progress to stderr; check exit codes explicitly instead
$ErrorActionPreference = 'Continue'
$env:PYTHONPATH = 'C:\WawConverter\src'
$wawSearch = "$MapMod;$(Join-Path $Waw 'main')"
if (-not $XwmaDecoder) {
    & (Join-Path $PSScriptRoot 'build_audio_decoder.ps1') | Out-Host
    $XwmaDecoder = Join-Path $PSScriptRoot 'bin\xaudio_wma_decoder.exe'
}
if (-not (Test-Path -LiteralPath $XwmaDecoder)) { throw "Native XWMA bridge missing: $XwmaDecoder" }

Write-Host "== 0. dumping WaW clipmap and companion zones"
& (Join-Path $OatT4 'Unlinker.exe') --no-color --search-path $wawSearch `
    --model-format GLTF --include-assets 'clipmap,gfxworld,gameworldsp,comworld,lightdef,fx,weapon,xanim,sound,loadedsound,rawfile,physpreset,snddriverglobals,xmodel' --output-folder $Stage (Join-Path $MapMod "$MapZone.ff") *> (Join-Path $Stage 'clip_dump.log')
if ($LASTEXITCODE) { throw "clipmap dump failed ($LASTEXITCODE)" }
# map scripts are plain rawfiles in the WaW map zone (zone graph, etc.)
& (Join-Path $OatT4 'Unlinker.exe') --no-color --search-path $wawSearch `
    --include-assets 'rawfile' --output-folder (Join-Path $WawDumps 'map_rawfiles') (Join-Path $MapMod "$MapZone.ff") *> (Join-Path $Stage 'rawfile_dump.log')
if ($LASTEXITCODE) { throw "rawfile dump failed ($LASTEXITCODE)" }
# gameplay scripts of the zones WaW loads with the map (mod.ff), and the stock
# WaW framework scripts (gscport tells the map's own scripts from the framework)
$modRaw = Join-Path $WawDumps 'mod_rawfiles'
if ($Redump -or -not (Test-Path (Join-Path $modRaw 'maps'))) {
    & (Join-Path $OatT4 'Unlinker.exe') --no-color --search-path $wawSearch `
        --include-assets 'rawfile' --output-folder $modRaw (Join-Path $MapMod 'mod.ff') *> "$modRaw.log"
    if ($LASTEXITCODE) { throw "mod.ff rawfile dump failed ($LASTEXITCODE)" }
}
$stockScripts = Join-Path $WawDumps 'stock_scripts'
if ($Redump -or -not (Test-Path (Join-Path $stockScripts '.complete'))) {
    foreach ($z in @('common', 'nazi_zombie_prototype', 'nazi_zombie_asylum', 'nazi_zombie_sumpf', 'nazi_zombie_factory')) {
        $zff = Join-Path $Waw "zone\english\$z.ff"
        if (-not (Test-Path $zff)) { continue }
        & (Join-Path $OatT4 'Unlinker.exe') --no-color --search-path (Join-Path $Waw 'main') `
            --include-assets 'rawfile' --output-folder $stockScripts $zff *> "$stockScripts`_$z.log"
        if ($LASTEXITCODE) { throw "stock script dump of $z failed ($LASTEXITCODE)" }
    }
    New-Item -ItemType File -Force (Join-Path $stockScripts '.complete') | Out-Null
}
# Zones WaW loads with the map, in lookup order: the map's patch zone, the
# mod's mod.ff, then the stock zones every WaW map loads.
$companions = @(
    @{ name = "${MapZone}_patch"; ff = (Join-Path $MapMod "${MapZone}_patch.ff") },
    @{ name = 'mod'; ff = (Join-Path $MapMod 'mod.ff') },
    @{ name = 'common'; ff = (Join-Path $Waw 'zone\english\common.ff') },
    @{ name = 'code_post_gfx'; ff = (Join-Path $Waw 'zone\english\code_post_gfx.ff') }
)
$extraRoots = @()
$lightingCode = Join-Path $WawDumps 'lighting_code'
if ($Redump -or -not (Test-Path (Join-Path $lightingCode '.lighting_assets_v1'))) {
    & (Join-Path $OatT4 'Unlinker.exe') --no-color --search-path $wawSearch --include-assets 'image,lightdef' `
        --output-folder $lightingCode (Join-Path $Waw 'zone\english\code_post_gfx.ff') *> "$lightingCode.log"
    if ($LASTEXITCODE) { throw "lighting code assets dump failed ($LASTEXITCODE)" }
    New-Item -ItemType File -Force (Join-Path $lightingCode '.lighting_assets_v1') | Out-Null
}
$extraRoots += @('--extra-root', $lightingCode)
New-Item -ItemType Directory -Force $WawDumps | Out-Null
# The map zone's own materials carry the world (lightmapped) techniques and
# their programs; step 0 dumps no materials, so dump them here (after $Stage
# in lookup order: identical material files, adds waw_techniquesets/shader_bin).
$mapMaterials = Join-Path $WawDumps 'map_materials'
if ($Redump -or -not (Test-Path (Join-Path $mapMaterials '.dump_v9_lightdefs'))) {
    & (Join-Path $OatT4 'Unlinker.exe') --no-color --search-path $wawSearch `
        --include-assets 'material' --output-folder $mapMaterials (Join-Path $MapMod "$MapZone.ff") *> "$mapMaterials.log"
    if ($LASTEXITCODE) { throw "map material dump failed ($LASTEXITCODE)" }
    New-Item -ItemType File -Force (Join-Path $mapMaterials '.dump_v9_lightdefs') | Out-Null
}
$extraRoots += @('--extra-root', $mapMaterials)
foreach ($c in $companions) {
    if (-not (Test-Path $c.ff)) { continue }
    $out = Join-Path $WawDumps $c.name
    # v9: source light definitions; v8: shader argument bindings.
    if ($Redump -or -not (Test-Path (Join-Path $out '.dump_v9_lightdefs'))) {
        & (Join-Path $OatT4 'Unlinker.exe') --no-color --search-path $wawSearch --image-format DDS --model-format GLTF `
            --include-assets 'material,image,xmodel,fx,weapon,xanim,sound,loadedsound,rawfile,comworld,lightdef,physpreset,snddriverglobals' --output-folder $out $c.ff *> "$out.log"
        if ($LASTEXITCODE) { throw "dump of $($c.ff) failed ($LASTEXITCODE)" }
        New-Item -ItemType File -Force (Join-Path $out '.dump_v9_lightdefs') | Out-Null
    }
    $extraRoots += @('--extra-root', $out)
}

$stockFf = Join-Path $Bo2 "zone\all\$StockZone.ff"
if (-not (Test-Path $stockFf)) { throw "stock zone not found: $stockFf" }
if ($Redump -or -not (Test-Path (Join-Path $Dump 'techniquesets\.complete'))) {
    # zm_nuked references (does not include) some of its technique sets; they
    # live in the zones every zombies map loads with it.
    foreach ($zone in @($StockZone, 'common_zm', 'code_post_gfx_zm')) {
        Write-Host "== 1. dumping materials + technique sets from $zone.ff"
        & (Join-Path $OatT6 'Unlinker.exe') --no-color `
            --search-path (Join-Path $Bo2 'zone\all') `
            --include-assets 'material,techniqueset' `
            --output-folder $Dump `
            (Join-Path $Bo2 "zone\all\$zone.ff") *> (Join-Path $Dump "..\stock_t6_dump_$zone.log")
        if ($LASTEXITCODE) { throw "Unlinker failed on $zone ($LASTEXITCODE)" }
    }
    New-Item -ItemType File -Force (Join-Path $Dump 'techniquesets\.complete') | Out-Null
}
if ($Redump -or -not (Test-Path (Join-Path $Dump 'zbarrier\.barrier_complete'))) {
    Write-Host '== 1b. dumping stock BO2 barrier and its render assets from zm_prison.ff'
    & (Join-Path $OatT6 'Unlinker.exe') --no-color `
        --search-path (Join-Path $Bo2 'zone\all') `
        --image-format IWI --include-assets 'zbarrier,xmodel,material,image,techniqueset' --output-folder $Dump `
        (Join-Path $Bo2 'zone\all\zm_prison.ff') *> (Join-Path $Dump '..\stock_t6_barrier.log')
    if ($LASTEXITCODE) { throw "stock barrier dump failed ($LASTEXITCODE)" }
    New-Item -ItemType File -Force (Join-Path $Dump 'zbarrier\.barrier_complete') | Out-Null
}

# Material equivalents need real pixels, not just the donor's technique metadata.
if ($Redump -or -not (Test-Path (Join-Path $Dump '.material_images_v1'))) {
    & (Join-Path $OatT6 'Unlinker.exe') --no-color --search-path (Join-Path $Bo2 'zone\all') `
        --include-assets 'image' --image-format IWI --output-folder $Dump $stockFf `
        *> (Join-Path $Dump '..\stock_t6_material_images.log')
    if ($LASTEXITCODE) { throw "stock material image dump failed ($LASTEXITCODE)" }
    New-Item -ItemType File -Force (Join-Path $Dump '.material_images_v1') | Out-Null
}

if ($Redump -or -not (Test-Path (Join-Path $Dump 'sounddriverglobals\singleton.w2bsdg'))) {
    # A map cannot replace the driver (BO2 drops on a second snddriverglobals),
    # so converted aliases are bound to the stock driver's curves by shape.
    Write-Host '== 1c. dumping the stock BO2 sound driver from code_post_gfx_zm.ff'
    & (Join-Path $OatT6 'Unlinker.exe') --no-color `
        --search-path (Join-Path $Bo2 'zone\all') `
        --include-assets 'snddriverglobals' --output-folder $Dump `
        (Join-Path $Bo2 'zone\all\code_post_gfx_zm.ff') *> (Join-Path $Dump '..\stock_t6_sound_driver.log')
    if ($LASTEXITCODE) { throw "stock sound driver dump failed ($LASTEXITCODE)" }
}

Write-Host "== 2. staging $Project"
python -m waw2bo2.cli stage-bridge $Stage $Project `
    --gfx (Join-Path $Stage "waw2bo2\maps\$MapZone.d3dbsp.gfx.bin") `
    --clip (Join-Path $Stage "waw2bo2\maps\$MapZone.d3dbsp.clip.bin") `
    @extraRoots `
    --iwd-dir $MapMod --iwd-dir (Join-Path $Waw 'main') `
    --waw-map-script (Join-Path $WawDumps "map_rawfiles\maps\$MapZone.gsc") `
    --waw-script-root $modRaw --waw-stock-scripts $stockScripts --t6-unlinker (Join-Path $OatT6 'Unlinker.exe') `
    @fxFallbackArgs @sourceFxArgs `
    --waw-root $Waw --t4-unlinker (Join-Path $OatT4 'Unlinker.exe') --waw-stock-dumps (Join-Path $WawDumps 'stock') `
    --xwma-decoder $XwmaDecoder `
    --approximate-sound-curves `
    --stock-materials (Join-Path $Dump 'materials') `
    --techset-dump $Dump `
    --bo2 $Bo2 `
    --script-template (Join-Path $Stage 'zone_raw\bridge') --script-template-name bridge
if ($LASTEXITCODE) {
    Write-Host "staging reported errors; see $Stage\zone_raw\$Project\bridge_stage.report.json"
    exit $LASTEXITCODE
}

# The 32-bit bridge linker exhausts its 2 GB address space on this map
# (3842 static models + 34k brushes) and crashes nondeterministically.
# Large-address-aware gives it 4 GB on 64-bit Windows; a rebuild resets it.
$editbin = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio', 'C:\Program Files (x86)\Microsoft Visual Studio' `
    -Recurse -Filter editbin.exe -ErrorAction SilentlyContinue | Where-Object FullName -like '*Hostx64\x86*' | Select-Object -First 1
if (-not $editbin) { throw "editbin.exe not found (install the MSVC x86 tools)" }
& $editbin.FullName /nologo /LARGEADDRESSAWARE (Join-Path $OatT6 'Linker.exe')
if ($LASTEXITCODE) { throw "editbin failed ($LASTEXITCODE)" }

Write-Host "== 3a. linking gameplay mod.ff with the BO2 mod tools linker"
python -m waw2bo2.cli build-mod $Stage $Project --bo2 $Bo2 --work $ModBuild --unlinker (Join-Path $OatT6 'Unlinker.exe') --linker (Join-Path $OatT6 'Linker.exe') --techset-dump $Dump
if ($LASTEXITCODE) { exit $LASTEXITCODE }

Write-Host "== 3b. compiling map scripts with the BO2 mod tools linker"
python -m waw2bo2.cli compile-scripts $Stage $Project --bo2 $Bo2 --unlinker (Join-Path $OatT6 'Unlinker.exe')
if ($LASTEXITCODE) { exit $LASTEXITCODE }

Write-Host "== 4. linking $Project with the T6 bridge"
python -m waw2bo2.cli bridge-link $Stage $Project `
    --linker (Join-Path $OatT6 'Linker.exe') `
    --techset-dump $Dump `
    --log (Join-Path $Stage "$Project`_bridge.log")
if ($LASTEXITCODE) { exit $LASTEXITCODE }

Write-Host "== 5. packaging the Plutonium mod"
python -m waw2bo2.cli package $Stage $Project --bo2 $Bo2 --work $ModBuild
if ($LASTEXITCODE) { exit $LASTEXITCODE }

# T6 resolves material constant/texture arguments with unbounded table scans
# (crashes 0x77C253, 0x7777F9, 0x77C173): replay them on what was installed.
Write-Host "== 6. auditing installed material arguments"
$installed = Join-Path $env:LOCALAPPDATA "Plutonium\storage\t6\mods\$Project"
foreach ($zone in @((Join-Path $Stage "zone_out\$Project\$Project.ff"), (Join-Path $installed 'mod.ff'))) {
    $audit = Join-Path $Stage ("audit_" + [IO.Path]::GetFileNameWithoutExtension($zone))
    if (Test-Path $audit) { Remove-Item -Recurse -Force $audit }
    & (Join-Path $OatT6 'Unlinker.exe') --no-color --search-path (Join-Path $Bo2 'zone\all') `
        --include-assets 'material,techniqueset' --output-folder $audit $zone *> "$audit.log"
    if ($LASTEXITCODE) { throw "audit unlink of $zone failed ($LASTEXITCODE)" }
    python (Join-Path $PSScriptRoot 'audit_material_args.py') $audit $Dump
    if ($LASTEXITCODE) { throw "material argument audit failed for $zone (would crash the game)" }
}
exit 0
