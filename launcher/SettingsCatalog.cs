namespace YeeHawLauncher;

/// <summary>One ini key a control writes: the value to write when the control is "on" and when it's "off".</summary>
public sealed record IniWrite(string Section, string Key, string On, string Off);

/// <summary>Something shown as one row in a settings group.</summary>
public abstract record Setting(string Label, string Help)
{
    /// <summary>True for settings both co-op players must set the same (the mod refuses to connect otherwise).</summary>
    public bool MustMatch { get; init; }
}

/// <summary>An on/off switch. It can write several keys at once (e.g. all the distance culls).</summary>
public sealed record Toggle(string Label, string Help, IniWrite[] Writes, bool Default) : Setting(Label, Help)
{
    /// <summary>A switch that writes one key: "1" when on, "0" when off.</summary>
    public static Toggle Of(string section, string key, string label, string help, bool @default = false) =>
        new(label, help, new[] { new IniWrite(section, key, "1", "0") }, @default);
}

/// <summary>A drop-down list. Each option shows a friendly name and writes its value.</summary>
public sealed record Choice(string Section, string Key, string Label, string Help, (string Name, string Value)[] Options,
                            string Default) : Setting(Label, Help);

/// <summary>A free text box (used for the partner's Steam ID).</summary>
public sealed record TextSetting(string Section, string Key, string Label, string Help, string Default = "")
    : Setting(Label, Help);

/// <summary>A titled box of settings on a tab.</summary>
public sealed record Group(string Title, Setting[] Items);

/// <summary>A one-click set of values for the graphics tab.</summary>
public sealed record Preset(string Name, string Help, (string Section, string Key, string Value)[] Values);

/// <summary>One tab of the launcher. Icon is a Segoe Fluent Icons glyph for the sidebar.</summary>
public sealed record Tab(string Title, string Subtitle, string Icon, Group[] Groups, Preset[]? Presets = null);

/// <summary>
/// Everything the launcher shows, in the order it's shown. Each entry names the ini
/// key it edits; the mod itself documents every key in harness\xinput_harness.ini.
/// </summary>
public static class SettingsCatalog
{
    private const string Net = "netplay", Gfx = "graphics", Harness = "harness";

    private static readonly (string, string)[] OnOff = { ("Off", "0"), ("On", "1") };

    public static readonly Tab[] Tabs =
    {
        new("Partner Up", "How you're playing today", "", new[]
        {
            new Group("Saddle up", new Setting[]
            {
                new Choice(Harness, "mode", "Game mode",
                    "Solo plays the game normally with the graphics improvements. Online co-op plays together with a friend over Steam.",
                    new[] { ("Solo", "passthrough"), ("Online co-op", "netplay") }, "passthrough"),
                new Choice(Net, "role", "Your part",
                    "One of you hosts (player 1, whose save you both play) and the other joins (player 2).",
                    new[] { ("Host (player 1)", "host"), ("Join (player 2)", "client") }, "host"),
                new TextSetting(Net, "peer_steamid", "Partner's Steam ID",
                    "Optional. Leave it empty to host for any friend, or to join through a Steam invite or Join Game. " +
                    "With an ID (the 17-digit number in their Steam profile's address), you connect only to that partner."),
            }),
            new Group("On the trail", new Setting[]
            {
                Toggle.Of(Net, "own_screen", "Full screen each",
                    "Each player sees only their own character, full screen, instead of split screen.", true) with { MustMatch = true },
                Toggle.Of(Net, "drop_in", "Drop-in",
                    "Your partner can join while you're in the middle of a level; both games reload it together.", true),
                new Choice(Net, "local_pad", "Your controller",
                    "Which controller on this PC is yours.",
                    new[] { ("Controller 1", "0"), ("Controller 2", "1"), ("Controller 3", "2"), ("Controller 4", "3") }, "0"),
                new Choice(Net, "input_delay", "Input delay",
                    "Frames between pressing a button and it happening on both screens. More hides a slower connection.",
                    new[] { ("2 frames (close by)", "2"), ("4 frames (normal)", "4"), ("6 frames", "6"), ("8 frames (far away)", "8") },
                    "4") with { MustMatch = true },
            }),
        }),

        new("Dress It Up", "How the game looks", "", new[]
        {
            new Group("Smooth edges", new Setting[]
            {
                Toggle.Of(Gfx, "enabled", "Graphics improvements",
                    "Turns every graphics setting on this tab on or off at once.", true),
                Toggle.Of(Gfx, "smaa", "SMAA anti-aliasing",
                    "Smooths jagged edges before the HUD is drawn, so text stays sharp."),
                new Choice(Gfx, "smaa_preset", "SMAA quality", "Higher is smoother and slightly slower.",
                    new[] { ("Low", "low"), ("Medium", "medium"), ("High", "high"), ("Ultra", "ultra") }, "medium"),
                new Choice(Gfx, "anisotropy", "Texture sharpness", "Keeps floors and roads sharp at an angle.",
                    new[] { ("Game default", "0"), ("2x", "2"), ("4x", "4"), ("8x", "8"), ("16x", "16") }, "0"),
            }),
            new Group("Shadows", new Setting[]
            {
                new Choice(Gfx, "shadow_res", "Shadow detail", "How sharp shadows are (the game's own is 1x).",
                    new[] { ("1x (game)", "1"), ("2x", "2"), ("3x", "3"), ("4x (max)", "4") }, "1"),
                Toggle.Of(Gfx, "soft_shadows", "Soft shadow edges", "Smooth shadow edges instead of jagged steps."),
                Toggle.Of(Gfx, "soft_char_shadow", "Soft character shadows", "A smooth edge on the blob shadow under each character."),
                new Toggle("Fix shadow glitches", "Fixes lit triangles on cliffs and the hard line where near shadows end.",
                    new[]
                    {
                        new IniWrite(Gfx, "shadow_bias_fix", "2", "0"),
                        new IniWrite(Gfx, "shadow_cascade_blend", "1", "0"),
                        new IniWrite(Gfx, "shadow_noclamp_vs", "1", "0"),
                    }, false),
            }),
            new Group("Distance and detail", new Setting[]
            {
                new Choice(Gfx, "lod_scale", "Character detail distance", "How far away characters stay fully detailed.",
                    new[] { ("Game", "1"), ("2x", "2"), ("4x", "4"), ("8x (max)", "8") }, "1") with { MustMatch = true },
                new Choice(Gfx, "object_fade_scale", "Object draw distance", "How far away objects and scenery stay visible.",
                    new[] { ("Game", "1"), ("2x", "2"), ("4x", "4"), ("8x", "8"), ("16x (max)", "16") }, "1") with { MustMatch = true },
                new Choice(Gfx, "world_lod_scale", "World detail distance", "How far away the world keeps its detailed models.",
                    new[] { ("Game", "1"), ("2x", "2"), ("4x", "4"), ("8x (max)", "8") }, "1") with { MustMatch = true },
                new Toggle("Keep small objects far away", "Small props and their shadows never pop out in the distance.",
                    new[]
                    {
                        new IniWrite(Gfx, "cull_pop", "0", ""), new IniWrite(Gfx, "cull_fade", "0", ""),
                        new IniWrite(Gfx, "shadow_cull_near", "0", ""), new IniWrite(Gfx, "shadow_cull_mid", "0", ""),
                        new IniWrite(Gfx, "shadow_cull_far", "0", ""), new IniWrite(Gfx, "shadow_no_contrib", "1", "0"),
                    }, false) { MustMatch = true },
            }),
            new Group("Light and effects", new Setting[]
            {
                Toggle.Of(Gfx, "full_res_reflection", "Full-resolution reflections", "Reflections at full instead of half resolution."),
                Toggle.Of(Gfx, "full_res_glow", "Full-resolution glow", "Glow effects at full instead of half resolution."),
                Toggle.Of(Gfx, "better_ao", "Better ambient shadows", "Less noisy soft shadows in corners and under objects."),
                new Choice(Gfx, "volumetric_res", "Light shafts", "Sun rays at full or the game's quarter resolution.",
                    new[] { ("Game (1/4)", "4"), ("Full", "1") }, "4"),
            }),
        },
        new[]
        {
            new Preset("Game", "The game as it shipped.", new[]
            {
                (Gfx, "smaa", "0"), (Gfx, "anisotropy", "0"), (Gfx, "shadow_res", "1"), (Gfx, "soft_shadows", "0"),
                (Gfx, "soft_char_shadow", "0"), (Gfx, "shadow_bias_fix", "0"), (Gfx, "shadow_cascade_blend", "0"),
                (Gfx, "shadow_noclamp_vs", "0"), (Gfx, "lod_scale", "1"), (Gfx, "object_fade_scale", "1"),
                (Gfx, "world_lod_scale", "1"), (Gfx, "full_res_reflection", "0"), (Gfx, "full_res_glow", "0"),
                (Gfx, "better_ao", "0"), (Gfx, "volumetric_res", "4"),
            }),
            new Preset("Pretty", "Clear improvements that run on most PCs.", new[]
            {
                (Gfx, "smaa", "1"), (Gfx, "smaa_preset", "medium"), (Gfx, "anisotropy", "8"), (Gfx, "shadow_res", "2"),
                (Gfx, "soft_shadows", "1"), (Gfx, "soft_char_shadow", "1"), (Gfx, "shadow_bias_fix", "2"),
                (Gfx, "shadow_cascade_blend", "1"), (Gfx, "shadow_noclamp_vs", "1"), (Gfx, "lod_scale", "4"),
                (Gfx, "object_fade_scale", "4"), (Gfx, "world_lod_scale", "4"), (Gfx, "full_res_reflection", "1"),
                (Gfx, "full_res_glow", "1"), (Gfx, "better_ao", "1"), (Gfx, "volumetric_res", "4"),
            }),
            new Preset("Yee Haw!", "Everything at maximum.", new[]
            {
                (Gfx, "smaa", "1"), (Gfx, "smaa_preset", "high"), (Gfx, "anisotropy", "16"), (Gfx, "shadow_res", "4"),
                (Gfx, "soft_shadows", "1"), (Gfx, "soft_char_shadow", "1"), (Gfx, "shadow_bias_fix", "2"),
                (Gfx, "shadow_cascade_blend", "1"), (Gfx, "shadow_noclamp_vs", "1"), (Gfx, "lod_scale", "8"),
                (Gfx, "object_fade_scale", "16"), (Gfx, "world_lod_scale", "8"), (Gfx, "full_res_reflection", "1"),
                (Gfx, "full_res_glow", "1"), (Gfx, "better_ao", "1"), (Gfx, "volumetric_res", "1"),
            }),
        }),

        new("Extras", "Bonus bits", "", new[]
        {
            new Group("Fun stuff", new Setting[]
            {
                Toggle.Of("debug_menu", "enabled", "Debug menu",
                    "Hold LB + RB and press Y in game for cheats, level select and more (locked in online co-op).", true),
                Toggle.Of(Harness, "restore_hidden_levels", "Restore Wii-only levels",
                    "Brings back levels that only shipped on Wii, like the shooting gallery.", true) with { MustMatch = true },
                Toggle.Of("audio", "vocal_theme", "Vocal \"You've Got a Friend in Me\"",
                    "Randy Newman's vocal on the story board and in the Toy Barn, as in the 2010 game. Needs the 2010 PC game installed; otherwise the instrumental plays."),
                Toggle.Of(Harness, "script_fixes_2010", "2010 script fixes",
                    "Bug fixes from the 2010 PC release: aiming after a landing, the ball throw, TNT in water, co-op ground pounds and more.") with { MustMatch = true },
                Toggle.Of(Harness, "test_save", "Test save",
                    "Plays on a separate copy of your save with achievements off, for trying things out. Your real save and its Steam Cloud copy aren't touched."),
            }),
        }),
    };
}
