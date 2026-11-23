// Puts together the release: release/MVS Patcher/ (run and test it from there) and release/MVSPatcher-<Version>.zip
// (Version from MVSPatcher.csproj), the folder as players download it. Nothing is published from here. It no longer
// writes helper/dist (the old framework-dependent staging, which may still be open).
//   node helper/stage.js [build/<pak name>]     (default build/BetaFeel_Roster_90_P, the last make_pak.js build)
// It publishes MVSPatcher.exe itself: self-contained, one file (the .NET runtime inside, so players install nothing).
// The folder = MVSPatcher.exe, README.txt, THIRD-PARTY-NOTICES.txt, recipe.json (offsets and our bytes, moves of the
// player's own bytes, plus the hashes of the files they apply to; no game files), plugins/MvsPakLoader.asi and .ini,
// plugins/BetaFeel.asi and .ini (the client-side plugin, players' build and settings), loader/xinput1_3.dll and its
// licence. The zip opens to that one folder, so players do not spill files into Downloads.
// Build the plugin first (plugin\build.cmd); --no-zip skips the zip.
const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');
const root = path.join(__dirname, '..');
const args = process.argv.slice(2).filter(a => !a.startsWith('--'));
const build = path.resolve(root, args[0] || 'build/BetaFeel_Roster_90_P');
const bin = path.join(__dirname, 'bin', 'publish');
const rel = path.join(root, 'release');
const dist = path.join(rel, 'MVS Patcher');
const version = /<Version>([^<]+)<\/Version>/.exec(fs.readFileSync(path.join(__dirname, 'MVSPatcher.csproj'), 'utf8'))[1];
if (!fs.existsSync(path.join(build, 'recipe.json'))) throw new Error(`no recipe.json in ${build}; run make_pak.js first`);
fs.rmSync(bin, { recursive: true, force: true });
execFileSync('dotnet', ['publish', __dirname, '-c', 'Release', '-r', 'win-x64', '--self-contained', 'true',
    '-p:PublishSingleFile=true', '-p:IncludeNativeLibrariesForSelfExtract=true', '-p:EnableCompressionInSingleFile=true',
    '-p:DebugType=none', '-o', bin, '--nologo', '-v', 'q'], { stdio: 'inherit' });
if (fs.readdirSync(bin).join() !== 'MVSPatcher.exe') throw new Error('publish gave more than MVSPatcher.exe: ' + fs.readdirSync(bin).join(', '));
fs.rmSync(dist, { recursive: true, force: true });
fs.mkdirSync(dist, { recursive: true });
fs.copyFileSync(path.join(bin, 'MVSPatcher.exe'), path.join(dist, 'MVSPatcher.exe'));
for (const f of ['README.txt', 'THIRD-PARTY-NOTICES.txt'])
    fs.writeFileSync(path.join(dist, f), fs.readFileSync(path.join(__dirname, f), 'utf8').replace(/\r?\n/g, '\r\n'));
if (!fs.readFileSync(path.join(dist, 'README.txt'), 'utf8').startsWith(`MVS Patcher ${version}\r\n`)) throw new Error(`README.txt's first line is not "MVS Patcher ${version}"`);
fs.copyFileSync(path.join(build, 'recipe.json'), path.join(dist, 'recipe.json'));
fs.mkdirSync(path.join(dist, 'plugins'));
for (const f of ['MvsPakLoader.asi', 'MvsPakLoader.ini']) fs.copyFileSync(path.join(root, 'pakloader', f), path.join(dist, 'plugins', f));
// BetaFeel.asi (display only; the players' build, without the measuring aids) with the players' settings, and the ASI
// loader that loads plugins\*.asi: Ultimate ASI Loader x64 9.7.4 (ThirteenAG, MIT; its licence goes with it), copied
// from the local game folder (only read there); the path is set below.
fs.copyFileSync(path.join(root, 'plugin', 'BetaFeel.asi'), path.join(dist, 'plugins', 'BetaFeel.asi'));
fs.copyFileSync(path.join(root, 'plugin', 'BetaFeel.ship.ini'), path.join(dist, 'plugins', 'BetaFeel.ini'));
if (/^\[Dev\]/m.test(fs.readFileSync(path.join(dist, 'plugins', 'BetaFeel.ini'), 'utf8'))) throw new Error('the shipped BetaFeel.ini has a [Dev] section');
if (fs.readFileSync(path.join(dist, 'plugins', 'BetaFeel.asi')).includes('recorder:')) throw new Error('plugin/BetaFeel.asi is a developer build');
const win64 = 'C:/Program Files (x86)/Steam/steamapps/common/MultiVersus/MultiVersus/Binaries/Win64';
fs.mkdirSync(path.join(dist, 'loader'));
const LOADER_SHA = 'fa266e3513d02c08a1b808f28c10538a489eaffaa4b0707f7cc1066e71b5afd7';
const ldr = fs.readFileSync(path.join(win64, 'xinput1_3.dll'));
if (require('crypto').createHash('sha256').update(ldr).digest('hex') !== LOADER_SHA) throw new Error('xinput1_3.dll is not the Ultimate ASI Loader 9.7.4 this was checked with');
fs.writeFileSync(path.join(dist, 'loader', 'xinput1_3.dll'), ldr);
fs.copyFileSync(path.join(win64, 'LICENSE_Ultimate_ASI_Loader.txt'), path.join(dist, 'loader', 'LICENSE_Ultimate_ASI_Loader.txt'));
// Nothing from the game may be in here: no pak, utoc, ucas, uasset or uexp.
const bad = [];
(function walk(d) { for (const e of fs.readdirSync(d, { withFileTypes: true })) { const p = path.join(d, e.name); if (e.isDirectory()) walk(p); else if (/\.(pak|utoc|ucas|uasset|uexp|umap)$/i.test(e.name)) bad.push(p); } })(dist);
if (bad.length) throw new Error('game files in dist: ' + bad.join(', '));
let size = 0; (function walk(d) { for (const e of fs.readdirSync(d, { withFileTypes: true })) { const p = path.join(d, e.name); if (e.isDirectory()) walk(p); else size += fs.statSync(p).size; } })(dist);
console.log(`staged ${path.relative(root, dist)}: ${(size / 1048576).toFixed(1)} MB, recipe from ${path.relative(root, build)}`);
if (!process.argv.includes('--no-zip')) {
    const zip = path.join(rel, `MVSPatcher-${version}.zip`);
    fs.rmSync(zip, { force: true });
    // Windows' own tar (bsdtar): forward slashes in the zip, which every unzip tool reads (Compress-Archive in
    // PowerShell 5.1 writes backslashes)
    execFileSync(path.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'tar.exe'), ['-a', '-c', '-f', zip, '-C', rel, 'MVS Patcher'], { stdio: 'inherit' });
    console.log(`zipped ${path.relative(root, zip)}: ${(fs.statSync(zip).size / 1048576).toFixed(1)} MB`);
}
