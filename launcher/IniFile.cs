using System.IO;
using System.Text;

namespace YeeHawLauncher;

/// <summary>
/// Reads and writes xinput_harness.ini without disturbing anything the launcher
/// doesn't change: comments, blank lines, key order and keys it doesn't know about
/// all stay exactly as they were.
/// </summary>
/// <remarks>
/// The mod reads this file with the Win32 GetPrivateProfile* functions, so it's kept
/// as Latin-1 text with CRLF line ends. Keys are matched without regard to case, like
/// those functions do.
/// </remarks>
public sealed class IniFile
{
    private static readonly Encoding FileEncoding = Encoding.Latin1;
    private readonly List<string> _lines;

    /// <summary>Where the file lives on disk.</summary>
    public string Path { get; }

    /// <summary>Loads the file, or starts an empty one if it doesn't exist yet.</summary>
    public IniFile(string path)
    {
        Path = path;
        _lines = File.Exists(path)
            ? File.ReadAllText(path, FileEncoding).Replace("\r\n", "\n").Split('\n').ToList()
            : new List<string>();
        // A file ending in a newline splits into a final empty string: drop it, Save adds it back.
        if (_lines.Count > 0 && _lines[^1].Length == 0) _lines.RemoveAt(_lines.Count - 1);
    }

    /// <summary>The value of section.key, or null when the key isn't in the file.</summary>
    public string? Get(string section, string key)
    {
        int line = FindKey(section, key);
        return line < 0 ? null : _lines[line][(_lines[line].IndexOf('=') + 1)..];
    }

    /// <summary>
    /// Sets section.key. An existing line is rewritten in place; a new key goes at the end
    /// of its section (before any blank lines that close it); a new section goes at the end.
    /// </summary>
    public void Set(string section, string key, string value)
    {
        string text = $"{key}={value}";
        int line = FindKey(section, key);
        if (line >= 0) { _lines[line] = text; return; }

        int header = FindSection(section);
        if (header < 0)
        {
            if (_lines.Count > 0 && _lines[^1].Length > 0) _lines.Add("");
            _lines.Add($"[{section}]");
            _lines.Add(text);
            return;
        }
        int end = header + 1;
        while (end < _lines.Count && !IsSectionHeader(_lines[end])) end++;
        while (end > header + 1 && _lines[end - 1].Trim().Length == 0) end--;
        _lines.Insert(end, text);
    }

    /// <summary>
    /// Writes the file back. The first time the launcher saves over an existing file, the
    /// original is kept next to it as xinput_harness.before-launcher.ini.
    /// </summary>
    public void Save()
    {
        if (File.Exists(Path))
        {
            string backup = System.IO.Path.ChangeExtension(Path, null) + ".before-launcher.ini";
            if (!File.Exists(backup)) File.Copy(Path, backup);
        }
        File.WriteAllText(Path, string.Join("\r\n", _lines) + "\r\n", FileEncoding);
    }

    private static bool IsSectionHeader(string line) => line.TrimStart().StartsWith('[');

    private int FindSection(string section)
    {
        for (int i = 0; i < _lines.Count; i++)
            if (string.Equals(_lines[i].Trim(), $"[{section}]", StringComparison.OrdinalIgnoreCase)) return i;
        return -1;
    }

    private int FindKey(string section, string key)
    {
        int header = FindSection(section);
        if (header < 0) return -1;
        for (int i = header + 1; i < _lines.Count && !IsSectionHeader(_lines[i]); i++)
        {
            string line = _lines[i];
            int eq = line.IndexOf('=');
            if (eq <= 0 || line.TrimStart().StartsWith(';')) continue;
            if (string.Equals(line[..eq].Trim(), key, StringComparison.OrdinalIgnoreCase)) return i;
        }
        return -1;
    }
}
