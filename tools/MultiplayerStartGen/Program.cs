// MultiplayerStartGen - generates dist/mods/KenshiCoop/KenshiCoop.mod, the FCS data
// half of the KenshiCoop mod. It carries TWO co-op game starts:
//
//   "Multiplayer (Wanderer x2)"   vanilla Wanderer start, two wanderers, one squad each.
//   "Multiplayer+ (Wanderer x2)"  the same shape with money = 500000. The KenshiCoop
//                                 plugin recognises this start by its squad-2 leader
//                                 template and floors every stat on the player squad
//                                 at 50 (FCS cannot express per-skill values; a
//                                 Character record only carries the grouped
//                                 "combat stats"/"ranged stats"/... fields).
//
// The two names are a matched pair on purpose: same start, with and without the
// boost. The "+" variant shipped as "Wanderer+ x2" in v0.50 and was renamed for
// v0.51 - a DISPLAY name only, so no save is affected (see STRING IDs below).
//
// DESIGN RULE (inherited from the original start by zeroit789, PR #15): stay
// vanilla-equivalent. Squad 1 always reuses the untouched vanilla Wanderer
// SquadTemplate; each squad 2 gets a NEW template whose leader is an exact clone of
// the vanilla Wanderer Character. No faction, AI or combat data is altered.
//
// STRING IDs ARE FROZEN. Kenshi saves store the StringId of the template a character
// came from, so renaming a record orphans it in every existing save. Records 1-3 were
// first published under the old "KenshiCoop-MultiplayerStart" mod name and keep that
// suffix forever, even though the file now ships as KenshiCoop.mod. New records join
// the same namespace rather than starting a new one.
//
// REGENERATE:
//   Requires a .NET 9 SDK (OpenConstructionSet 4.1.0 targets net9.0). If `dotnet` is
//   not on PATH, install per-user:
//     powershell -NoProfile -ExecutionPolicy Bypass -Command "& $env:TEMP\dotnet-install.ps1 -Channel 9.0 -InstallDir \"$env:LOCALAPPDATA\Microsoft\dotnet\""
//
//   dotnet run                       # read the game data, write the .mod, verify it
//   dotnet run -- dump               # dump the .mod already on disk (no write)
//   dotnet run -- "D:\...\Kenshi"    # same as the first, with a non-default install
//
// Then deploy it into the game: scripts\deploy.cmd
using OpenConstructionSet.Data;
using OpenConstructionSet.Mods;

// ---- Identity constants -----------------------------------------------------
// The file we ship. Deliberately NOT the record namespace below.
const string OutModName = "KenshiCoop";
// The record namespace, frozen at the name the records were first published under.
const string RecordNamespace = "KenshiCoop-MultiplayerStart";

// Vanilla StringIds (verified against gamedata.base / rebirth.mod).
const string VanillaStartId = "1980-gamedata.base";    // NewGameStartoff "Wanderer"
const string VanillaSquadId = "45550-gamedata.base";   // SquadTemplate "startoff- Wanderer squad"
const string VanillaCharId = "1533662-rebirth.mod";    // Character "Wanderer"
const string HubTownId = "18919-Newwworld.mod";        // Town "The Hub" (vanilla Wanderer spawn)

// Money for the "Multiplayer+ (Wanderer x2)" start. Kenshi has ONE player faction
// wallet and KenshiCoop replicates it as a single shared pool, so this is 500k for
// the pair.
const int PlusStartMoney = 500000;

string Sid(int id) => $"{id}-{RecordNamespace}.mod";

// ---- Paths ------------------------------------------------------------------
string repoRoot = FindRepoRoot();
string outDir = Path.Combine(repoRoot, "dist", "mods", OutModName);
string outPath = Path.Combine(outDir, OutModName + ".mod");

string[] argList = args;
bool dumpMode = argList.Any(a => a.Equals("dump", StringComparison.OrdinalIgnoreCase));
string? kenshiDirArg = argList.FirstOrDefault(a => !a.Equals("dump", StringComparison.OrdinalIgnoreCase));

// ---- Dump mode: inspect the .mod already on disk ----------------------------
if (dumpMode)
{
    if (!File.Exists(outPath)) { Console.WriteLine($"Not found: {outPath} - generate it first."); return 1; }
    DumpAll(await new ModFile(outPath).ReadDataAsync(), outPath);
    return 0;
}

string kenshiDir = ResolveKenshiDir(kenshiDirArg);
string dataDir = Path.Combine(kenshiDir, "data");
Console.WriteLine($"Kenshi install: {kenshiDir}");

// ---- 1) Read the base game data ---------------------------------------------
Console.WriteLine("Reading Kenshi base data...");
var baseData = await new ModFile(Path.Combine(dataDir, "gamedata.base")).ReadDataAsync();
var rebirth = await new ModFile(Path.Combine(dataDir, "rebirth.mod")).ReadDataAsync();
Console.WriteLine($"  gamedata.base: {baseData.Items.Count} items | rebirth.mod: {rebirth.Items.Count} items");

// Lookup by StringId within one file. Duplicates: last wins, as the game does.
static Item? Find(IEnumerable<Item> items, string stringId) =>
    items.LastOrDefault(i => i.StringId.Equals(stringId, StringComparison.OrdinalIgnoreCase));

var startBase = Find(baseData.Items, VanillaStartId);
var startOver = Find(rebirth.Items, VanillaStartId);
var squadBase = Find(baseData.Items, VanillaSquadId);
var squadOver = Find(rebirth.Items, VanillaSquadId);
var wandChar = Find(rebirth.Items, VanillaCharId);

if (startBase is null || startOver is null || squadBase is null || squadOver is null || wandChar is null)
{
    Console.WriteLine("ERROR: the expected vanilla records were not found:");
    Console.WriteLine($"  startBase={startBase is not null} startOver={startOver is not null} " +
                      $"squadBase={squadBase is not null} squadOver={squadOver is not null} char={wandChar is not null}");
    return 1;
}
Console.WriteLine($"  Vanilla OK: start \"{startOver.Name}\", squad \"{squadBase.Name}\", char \"{wandChar.Name}\"");

// ---- 2) Effective vanilla values = base record + rebirth override ------------
// Kenshi applies .mod files as deltas, so the values a player actually gets are the
// base record overlaid with the override. Reproduce that merge before cloning.
static Dictionary<string, object> MergeValues(Item baseItem, Item overrideItem)
{
    var merged = new Dictionary<string, object>(baseItem.Values);
    foreach (var kv in overrideItem.Values) merged[kv.Key] = kv.Value;
    return merged;
}
var startValues = MergeValues(startBase, startOver);   // includes rebirth's description
var squadValues = MergeValues(squadBase, squadOver);   // includes "dont multiply" = True

var newSave = new ItemSaveData(1, ItemChangeType.New);

// Clone the vanilla Wanderer Character verbatim (clothing, weapon, dialogue,
// personality, stat fields - everything). Only the name and StringId differ.
Item CloneWanderer(int id, string name) => new(
    ItemType.Character, id, name, Sid(id), newSave,
    new Dictionary<string, object>(wandChar.Values),
    wandChar.ReferenceCategories.Select(c => new ReferenceCategory(c)),
    []);

// Clone the effective vanilla squad template, pointing its leader at `leaderId`.
// v0 = 1 on the leader reference means one unit, matching vanilla.
Item CloneSquad(int id, string name, int leaderId) => new(
    ItemType.SquadTemplate, id, name, Sid(id), newSave,
    new Dictionary<string, object>(squadValues),
    [new ReferenceCategory("leader", [new Reference(Sid(leaderId), 1, 0, 0)])],
    []);

// Build a co-op start: vanilla Wanderer values, our description, and TWO squads.
// Reference ORDER matters - the first squad is the host's (squad tab 1).
Item MakeStart(int id, string name, string description, int squad2Id, int money)
{
    var values = new Dictionary<string, object>(startValues) { ["description"] = description };
    // Preserve the field's stored numeric type rather than forcing int.
    values["money"] = values.TryGetValue("money", out var existing) && existing is not null
        ? Convert.ChangeType(money, existing.GetType())!
        : money;
    return new Item(
        ItemType.NewGameStartoff, id, name, Sid(id), newSave, values,
        [
            new ReferenceCategory("squad",
            [
                new Reference(VanillaSquadId, 0, 0, 0), // squad 1: untouched vanilla template (host)
                new Reference(Sid(squad2Id), 0, 0, 0),  // squad 2: our clone (joining player)
            ]),
            new ReferenceCategory("town", [new Reference(HubTownId, 0, 0, 0)]),
        ],
        []);
}

// ---- 3) The records ----------------------------------------------------------
// 1-3: the original "Multiplayer (Wanderer x2)" start. Reproduced byte-for-byte in
// intent - these ids are referenced by saves already in the wild.
var wanderer2 = CloneWanderer(1, "Wanderer 2");
var wandererSquad2 = CloneSquad(2, "startoff- Wanderer squad 2 (co-op)", 1);
var wandererStart = MakeStart(3, "Multiplayer (Wanderer x2)",
    "Two lone wanderers with nothing but a few coins, a pair of pants each and a couple of rusty " +
    "swords, ready to venture out into the world together.  Designed for KenshiCoop: each wanderer " +
    "starts in their own squad, so the host controls squad 1 and the joining player controls squad 2.",
    squad2Id: 2, money: Convert.ToInt32(startValues["money"]));

// 4-6: the "Multiplayer+ (Wanderer x2)" start. Record 4 is a SEPARATE Character from
// record 1 even though both spawn a PC called "Wanderer 2" - the StringId is what tells
// the two starts apart, and record 4's is the MARKER the plugin matches on to apply the
// stat floor. Keep it in sync with WPX2_MARKER_SID in src/plugin/Plugin.cpp.
var plusWanderer = CloneWanderer(4, "Wanderer 2");
var plusSquad2 = CloneSquad(5, "startoff- Multiplayer+ squad 2 (co-op)", 4);
var plusStart = MakeStart(6, "Multiplayer+ (Wanderer x2)",
    "Two seasoned wanderers setting out together with a small fortune behind them.  The same start " +
    "as Multiplayer (Wanderer x2) - each wanderer in their own squad, so the host controls squad 1 " +
    "and the joining player controls squad 2 - but without the early grind.  The 500,000 cats are " +
    "the shared faction wallet both players spend from, and the KenshiCoop plugin raises every stat " +
    "on both characters to 50 on the first tick of a new game.",
    squad2Id: 5, money: PlusStartMoney);

Item[] items = [wanderer2, wandererSquad2, wandererStart, plusWanderer, plusSquad2, plusStart];

// ---- 4) Assemble and write ---------------------------------------------------
var header = new Header(1, "",
    "The data half of KenshiCoop: two co-op game starts. \"Multiplayer (Wanderer x2)\" is the " +
    "vanilla Wanderer start with two wanderers, each already in their own squad, so the host plays " +
    "squad 1 and the joining player takes squad 2. \"Multiplayer+ (Wanderer x2)\" is the same start " +
    "with 500,000 cats in the shared wallet and both characters levelled to 50 in every stat by the " +
    "plugin. Data-only mod; requires the KenshiCoop plugin for co-op.")
{
    Dependencies = ["gamedata.base", "Newwworld.mod", "rebirth.mod", "Dialogue.mod"],
};
var info = new ModInfoData
{
    ModName = OutModName + ".mod",
    Title = "KenshiCoop",
    Tags = ["Gameplay"],
};
var modData = new ModFileData(DataFileType.Mod, header, items.Length, items, info);

Directory.CreateDirectory(outDir);
await new ModFile(outPath).WriteDataAsync(modData);
Console.WriteLine($"\nWrote: {outPath} ({new FileInfo(outPath).Length} bytes)");

// The shipped mod folder is just the DLL + RE_Kenshi.json + the .mod (see
// scripts\deploy.cmd). OCS also emits a launcher .info alongside the data file;
// drop it so the folder keeps the layout deploy and the mod kit expect.
foreach (var stray in Directory.GetFiles(outDir, "_*.info"))
{
    File.Delete(stray);
    Console.WriteLine($"Removed generated launcher file: {Path.GetFileName(stray)}");
}

// ---- 5) Verify by re-reading what we just wrote ------------------------------
var verify = await new ModFile(outPath).ReadDataAsync();
DumpAll(verify, outPath);

var errors = new List<string>();
if (verify.Items.Count != items.Length)
    errors.Add($"expected {items.Length} items, found {verify.Items.Count}");

// Every StringId we have ever published must still be present and of the right type.
(string Sid, ItemType Type)[] expected =
[
    (Sid(1), ItemType.Character), (Sid(2), ItemType.SquadTemplate), (Sid(3), ItemType.NewGameStartoff),
    (Sid(4), ItemType.Character), (Sid(5), ItemType.SquadTemplate), (Sid(6), ItemType.NewGameStartoff),
];
foreach (var (sid, type) in expected)
{
    var found = verify.Items.FirstOrDefault(i => i.StringId == sid);
    if (found is null) errors.Add($"missing record {sid} (existing saves reference it)");
    else if (found.Type != type) errors.Add($"record {sid} is {found.Type}, expected {type}");
}

// Character clones must carry the full vanilla Wanderer payload.
int wandRefs = wandChar.ReferenceCategories.Sum(c => c.References.Count);
foreach (var sid in new[] { Sid(1), Sid(4) })
{
    var c = verify.Items.FirstOrDefault(i => i.StringId == sid);
    if (c is null) continue;
    if (c.Values.Count != wandChar.Values.Count)
        errors.Add($"{sid}: {c.Values.Count} values (vanilla Wanderer has {wandChar.Values.Count})");
    if (c.ReferenceCategories.Sum(x => x.References.Count) != wandRefs)
        errors.Add($"{sid}: {c.ReferenceCategories.Sum(x => x.References.Count)} refs (vanilla has {wandRefs})");
}

// Squad templates must keep the vanilla values and lead with our clone.
foreach (var (sid, leaderSid) in new[] { (Sid(2), Sid(1)), (Sid(5), Sid(4)) })
{
    var s = verify.Items.FirstOrDefault(i => i.StringId == sid);
    if (s is null) continue;
    if (s.Values.Count != squadValues.Count)
        errors.Add($"{sid}: {s.Values.Count} values (expected {squadValues.Count})");
    var leader = s.ReferenceCategories.FirstOrDefault(c => c.Name == "leader")?.References.SingleOrDefault();
    if (leader is null || leader.TargetId != leaderSid || leader.Value0 != 1)
        errors.Add($"{sid}: 'leader' does not point at {leaderSid} with v0=1");
    if (!Equals(s.Values.TryGetValue("dont multiply", out var dm) ? dm : null, true))
        errors.Add($"{sid}: missing 'dont multiply'=True (vanilla effective value)");
}

// Both starts: two squads in the right order, the vanilla town, vanilla game values.
foreach (var (sid, squad2Sid, money) in new[]
         {
             (Sid(3), Sid(2), Convert.ToInt32(startValues["money"])),
             (Sid(6), Sid(5), PlusStartMoney),
         })
{
    var st = verify.Items.FirstOrDefault(i => i.StringId == sid);
    if (st is null) continue;
    var squads = st.ReferenceCategories.FirstOrDefault(c => c.Name == "squad")?.References ?? [];
    if (squads.Count != 2) errors.Add($"{sid}: {squads.Count} 'squad' refs (expected 2)");
    else
    {
        if (squads[0].TargetId != VanillaSquadId) errors.Add($"{sid}: squad 1 is not the vanilla host template");
        if (squads[1].TargetId != squad2Sid) errors.Add($"{sid}: squad 2 is not {squad2Sid}");
    }
    var town = st.ReferenceCategories.FirstOrDefault(c => c.Name == "town")?.References.SingleOrDefault();
    if (town is null || town.TargetId != HubTownId) errors.Add($"{sid}: 'town' is not vanilla The Hub");
    if (Convert.ToInt32(st.Values.TryGetValue("money", out var m) ? m : 0) != money)
        errors.Add($"{sid}: money is {(m ?? "null")}, expected {money}");
    // Everything except money and description stays exactly as the vanilla Wanderer start.
    foreach (var key in new[] { "difficulty", "start pos X", "start pos Z", "style", "force start pos" })
        if (!Equals(st.Values.TryGetValue(key, out var v) ? v : null, startBase.Values[key]))
            errors.Add($"{sid}: value '{key}' differs from the vanilla Wanderer start");
}

Console.WriteLine();
if (errors.Count > 0)
{
    Console.WriteLine("VERIFICATION FAILED:");
    foreach (var e in errors) Console.WriteLine($"  - {e}");
    return 1;
}
Console.WriteLine($"VERIFICATION OK: {items.Length} records, both starts wired to two squads, " +
                  $"legacy StringIds intact, \"Multiplayer+ (Wanderer x2)\" money = {PlusStartMoney}.");
return 0;

// ---- Helpers -----------------------------------------------------------------

// The repo root is the nearest ancestor of the build output that holds dist/mods/<mod>.
static string FindRepoRoot()
{
    for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir is not null; dir = dir.Parent)
        if (Directory.Exists(Path.Combine(dir.FullName, "dist", "mods", "KenshiCoop")))
            return dir.FullName;
    throw new DirectoryNotFoundException(
        $"could not find the repo root (no dist/mods/KenshiCoop above {AppContext.BaseDirectory})");
}

// The install holding gamedata.base. Same default the scripts/ drivers use.
static string ResolveKenshiDir(string? explicitDir)
{
    string[] candidates = explicitDir is not null
        ? [explicitDir]
        :
        [
            @"C:\Program Files (x86)\Steam\steamapps\common\Kenshi",
            @"C:\Program Files\Steam\steamapps\common\Kenshi",
        ];
    foreach (var c in candidates)
        if (File.Exists(Path.Combine(c, "data", "gamedata.base")))
            return c;
    throw new DirectoryNotFoundException(
        "could not find a Kenshi install with data\\gamedata.base. Tried: " + string.Join("; ", candidates) +
        ". Pass the install path as an argument: dotnet run -- \"D:\\path\\to\\Kenshi\"");
}

// Full dump of a .mod, for eyeballing a generated file.
static void DumpAll(ModFileData d, string path)
{
    Console.WriteLine($"\n=== DUMP {Path.GetFileName(path)}: {d.Items.Count} items, lastId={d.LastId} ===");
    Console.WriteLine($"HEADER deps=[{string.Join("; ", d.Header.Dependencies)}]");
    foreach (var it in d.Items)
    {
        Console.WriteLine($"\n[{it.Type}] \"{it.Name}\" ({it.StringId}) save={it.SaveData}");
        foreach (var kv in it.Values.OrderBy(k => k.Key))
        {
            var s = kv.Value?.ToString() ?? "";
            Console.WriteLine($"    val {kv.Key} = {(s.Length > 90 ? s[..90] + "..." : s)}");
        }
        foreach (var cat in it.ReferenceCategories)
            foreach (var r in cat.References)
                Console.WriteLine($"    ref {cat.Name} -> {r.TargetId} [v0={r.Value0} v1={r.Value1} v2={r.Value2}]");
    }
}
