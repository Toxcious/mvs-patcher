using System.Buffers.Binary;
using System.Reflection;
using System.Text;

// C# port of zen.js and evade.js (keep the three in step): grows a UE 5.1 zen package by giving a fighter's Evade
// component a launch entry of its own. See those files' header comments for the layouts.
public static class Zen
{
    public class Export { public int At; public long Off, Size; public string Name = ""; public int Data = -1; }
    public class Bundle { public int At; public long Serial; public uint First, Count; }
    public class Pkg
    {
        public byte[] Buf = Array.Empty<byte>();
        public int HeaderSize, CookedHeaderSize, Bundles, Graph, NameStart, NameEnd, ExportMap;
        public ulong HashVer;
        public List<ulong> Hashes = new();
        public List<string> Names = new();
        public List<Export> Exports = new();
        public List<Bundle> BundleHeaders = new();
    }

    static int I32(byte[] b, int p) => BinaryPrimitives.ReadInt32LittleEndian(b.AsSpan(p));
    static uint U32(byte[] b, int p) => BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(p));
    static long I64(byte[] b, int p) => BinaryPrimitives.ReadInt64LittleEndian(b.AsSpan(p));
    static void W32(byte[] b, int p, int v) => BinaryPrimitives.WriteInt32LittleEndian(b.AsSpan(p), v);
    static void W64(byte[] b, int p, long v) => BinaryPrimitives.WriteInt64LittleEndian(b.AsSpan(p), v);

    public static Pkg Parse(byte[] buf)
    {
        var z = new Pkg { Buf = buf };
        if (U32(buf, 0) != 0) throw new Exception("versioning info present: not handled");
        z.HeaderSize = I32(buf, 4); z.CookedHeaderSize = I32(buf, 20); z.ExportMap = I32(buf, 32); z.Bundles = I32(buf, 36); z.Graph = I32(buf, 40);
        int p = 44; z.NameStart = p;
        int num = I32(buf, p), strBytes = I32(buf, p + 4); z.HashVer = (ulong)I64(buf, p + 8);
        int h = p + 16; for (int i = 0; i < num; i++) z.Hashes.Add((ulong)I64(buf, h + 8 * i));
        int hd = h + 8 * num, sp = hd + 2 * num;
        for (int i = 0; i < num; i++)
        {
            int v = BinaryPrimitives.ReadUInt16BigEndian(buf.AsSpan(hd + 2 * i));
            if ((v & 0x8000) != 0) throw new Exception("wide name: not handled");
            int len = v & 0x7fff; z.Names.Add(Encoding.Latin1.GetString(buf, sp, len)); sp += len;
        }
        if (sp - (hd + 2 * num) != strBytes) throw new Exception("name strings do not add up");
        z.NameEnd = sp;
        if ((z.Bundles - z.ExportMap) % 72 != 0) throw new Exception("export map size is not a multiple of 72");
        int count = (z.Bundles - z.ExportMap) / 72;
        for (int i = 0; i < count; i++) { int e = z.ExportMap + 72 * i; z.Exports.Add(new Export { At = e, Off = I64(buf, e), Size = I64(buf, e + 8), Name = z.Names[I32(buf, e + 16)] }); }
        int pos = z.HeaderSize;
        for (int o = z.Bundles; o < z.Graph; o += 8) if (U32(buf, o + 4) == 1) { var x = z.Exports[I32(buf, o)]; x.Data = pos; pos += (int)x.Size; }
        int entries = (z.Graph - z.Bundles) / 8;
        for (int o = z.Graph, n = 0; n < entries; o += 16)
        {
            var b = new Bundle { At = o, Serial = I64(buf, o), First = U32(buf, o + 8), Count = U32(buf, o + 12) };
            if (b.First != n || b.Count == 0) throw new Exception("export bundle headers not as expected");
            z.BundleHeaders.Add(b); n += (int)b.Count;
            for (uint k = 0; k < b.Count; k++)
            {
                int e = z.Bundles + 8 * (int)(b.First + k);
                if (U32(buf, e + 4) != 1) continue;
                if (z.Exports[I32(buf, e)].Data - z.HeaderSize != b.Serial) throw new Exception("bundle serial offset does not match");
                break;
            }
        }
        return z;
    }

    // CUE4Parse's CityHash64 of the lower-cased name (how the engine hashes names).
    static MethodInfo? cityHash;
    static ulong NameHash(string s)
    {
        cityHash ??= typeof(CUE4Parse.FileProvider.DefaultFileProvider).Assembly.GetTypes().First(t => t.Name == "CityHash")
            .GetMethods().Where(m => m.Name == "CityHash64").OrderBy(m => m.GetParameters().Length == 1 && m.GetParameters()[0].ParameterType == typeof(byte[]) ? 0 : 1).First();
        var b = Encoding.ASCII.GetBytes(s.ToLowerInvariant());
        var r = cityHash.GetParameters().Length == 1 ? cityHash.Invoke(null, new object[] { b }) : cityHash.Invoke(null, new object[] { b, 0, (uint)b.Length });
        return Convert.ToUInt64(r);
    }

    public static (byte[] buf, int index) AddName(Pkg z, string name)
    {
        int have = z.Names.IndexOf(name);
        if (have >= 0) return (z.Buf, have);
        var names = new List<string>(z.Names) { name };
        var hashes = new List<ulong>(z.Hashes) { NameHash(name) };
        var ms = new MemoryStream();
        var head = new byte[16]; W32(head, 0, names.Count); W32(head, 4, names.Sum(n => n.Length)); W64(head, 8, (long)z.HashVer); ms.Write(head);
        foreach (var hh in hashes) { var t = new byte[8]; W64(t, 0, (long)hh); ms.Write(t); }
        foreach (var n in names) { var t = new byte[2]; BinaryPrimitives.WriteUInt16BigEndian(t, (ushort)n.Length); ms.Write(t); }
        foreach (var n in names) ms.Write(Encoding.Latin1.GetBytes(n));
        var batch = ms.ToArray();
        int grow = batch.Length - (z.NameEnd - z.NameStart);
        var buf = Concat(z.Buf.AsSpan(0, z.NameStart), batch, z.Buf.AsSpan(z.NameEnd));
        foreach (var o in new[] { 4, 24, 28, 32, 36, 40 }) W32(buf, o, I32(buf, o) + grow);
        return (buf, names.Count - 1);
    }

    public static byte[] SpliceExport(byte[] buf, int i, int at, int len, byte[] bytes)
    {
        var z = Parse(buf); var e = z.Exports[i];
        if (at < 0 || at + len > e.Size) throw new Exception("splice outside the export");
        var outb = Concat(buf.AsSpan(0, e.Data + at), bytes, buf.AsSpan(e.Data + at + len));
        long grow = bytes.Length - len;
        foreach (var x in z.Exports)
        {
            if (x == e) W64(outb, x.At + 8, x.Size + grow);
            else if (x.Off > e.Off) W64(outb, x.At, x.Off + grow);
        }
        foreach (var b in z.BundleHeaders) if (b.Serial > e.Data - z.HeaderSize) W64(outb, b.At, b.Serial + grow);
        Parse(outb);
        return outb;
    }

    static byte[] Concat(ReadOnlySpan<byte> a, ReadOnlySpan<byte> b, ReadOnlySpan<byte> c)
    {
        var r = new byte[a.Length + b.Length + c.Length];
        a.CopyTo(r); b.CopyTo(r.AsSpan(a.Length)); c.CopyTo(r.AsSpan(a.Length + b.Length));
        return r;
    }

    // ---- evade.js ----
    record struct Val(int Idx, bool Zero);
    static readonly int[] EvadeSizes = { 16, 16, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 0, 0, 0, 4, 4, -1 }; // -1 map, 0 not walked
    static readonly int[] LaunchSizes = { 16, 1, 1, 16, 1, 16, 1, 16, 16, 1, 1, 1, 1, 8, 8, 1, 1, 16, 16, 16 };
    static readonly Dictionary<string, int> MapIndex = new() { ["EvadeMovementLaunchParameters"] = 21, ["EvadeAttackLaunchParameters"] = 23 };

    static (List<Val> vals, int end) ReadHeader(byte[] b, int p)
    {
        var vals = new List<Val>(); var zeroed = new List<int>(); int idx = 0, maskBits = 0;
        for (; ; )
        {
            int v = BinaryPrimitives.ReadUInt16LittleEndian(b.AsSpan(p)); p += 2;
            int skip = v & 0x7f, n = v >> 9; bool hz = (v & 0x80) != 0, last = (v & 0x100) != 0;
            idx += skip;
            for (int k = 0; k < n; k++) { if (hz) zeroed.Add(vals.Count); vals.Add(new Val(idx++, false)); }
            if (hz) maskBits += n;
            if (last) break;
        }
        if (maskBits > 0)
        {
            for (int k = 0; k < zeroed.Count; k++) if ((b[p + (k >> 3)] & (1 << (k & 7))) != 0) vals[zeroed[k]] = vals[zeroed[k]] with { Zero = true };
            p += maskBits <= 8 ? 1 : maskBits <= 16 ? 2 : 4 * ((maskBits + 31) / 32);
        }
        return (vals, p);
    }

    static byte[] WriteHeader(List<Val> vals)
    {
        var frags = new List<(int skip, List<Val> vals)>(); int prev = 0;
        for (int i = 0; i < vals.Count;)
        {
            int skip = vals[i].Idx - prev, j = i;
            while (skip > 127) { frags.Add((127, new())); skip -= 127; }
            while (j + 1 < vals.Count && vals[j + 1].Idx == vals[j].Idx + 1 && j + 1 - i < 127) j++;
            frags.Add((skip, vals.GetRange(i, j - i + 1)));
            prev = vals[j].Idx + 1; i = j + 1;
        }
        if (frags.Count == 0) frags.Add((0, new()));
        var ms = new MemoryStream(); var mask = new List<bool>();
        for (int k = 0; k < frags.Count; k++)
        {
            var f = frags[k]; bool hz = f.vals.Any(v => v.Zero);
            if (hz) mask.AddRange(f.vals.Select(v => v.Zero));
            var t = new byte[2]; BinaryPrimitives.WriteUInt16LittleEndian(t, (ushort)(f.skip | (hz ? 0x80 : 0) | (k == frags.Count - 1 ? 0x100 : 0) | (f.vals.Count << 9))); ms.Write(t);
        }
        if (mask.Count > 0)
        {
            var m = new byte[mask.Count <= 8 ? 1 : mask.Count <= 16 ? 2 : 4 * ((mask.Count + 31) / 32)];
            for (int k = 0; k < mask.Count; k++) if (mask[k]) m[k >> 3] |= (byte)(1 << (k & 7));
            ms.Write(m);
        }
        return ms.ToArray();
    }

    class LaunchValue { public int Start, End, X = -1, Frames = -1; }
    static LaunchValue ReadLaunchValue(byte[] b, int p)
    {
        var (vals, q) = ReadHeader(b, p); var r = new LaunchValue { Start = p };
        foreach (var v in vals)
        {
            if (v.Zero) continue;
            if (v.Idx == 0)
            {
                var (inner, q2) = ReadHeader(b, q); q = q2;
                foreach (var w in inner) { if (w.Zero) continue; if (w.Idx == 0) r.X = q; if (w.Idx >= LaunchSizes.Length) throw new Exception("launch property " + w.Idx); q += LaunchSizes[w.Idx]; }
            }
            else if (v.Idx == 1) { r.Frames = q; q += 4; }
            else if (v.Idx == 2) q += 1;
            else throw new Exception("evade launch property " + v.Idx);
        }
        r.End = q; return r;
    }

    class Entry { public string Key = ""; public int At; public LaunchValue Value = new(); }
    class Map { public int At, Count, End; public List<Entry> Entries = new(); }
    static Map ReadMap(byte[] b, int p, List<string> names)
    {
        if (I32(b, p) != 0) throw new Exception("map removes keys: not handled");
        var m = new Map { At = p, Count = I32(b, p + 4) }; int q = p + 8;
        for (int i = 0; i < m.Count; i++)
        {
            uint num = U32(b, q + 4);
            var e = new Entry { Key = names[I32(b, q)] + (num != 0 ? "_" + (num - 1) : ""), At = q, Value = ReadLaunchValue(b, q + 8) };
            m.Entries.Add(e); q = e.Value.End;
        }
        m.End = q; return m;
    }

    class Evade { public int Index; public byte[] Data = Array.Empty<byte>(); public List<Val> Vals = new(); public int HeaderEnd; public Dictionary<int, Map> Maps = new(); public Dictionary<int, int> Pos = new(); }
    static Evade ReadEvade(Pkg z)
    {
        var hits = z.Exports.Select((e, i) => (e, i)).Where(t => t.e.Name == "Evade").ToList();
        if (hits.Count != 1) throw new Exception($"{hits.Count} exports named Evade");
        var x = z.Exports[hits[0].i];
        var ev = new Evade { Index = hits[0].i, Data = z.Buf.AsSpan(x.Data, (int)x.Size).ToArray() };
        (ev.Vals, ev.HeaderEnd) = ReadHeader(ev.Data, 0); int q = ev.HeaderEnd;
        foreach (var v in ev.Vals)
        {
            ev.Pos[v.Idx] = q;
            if (v.Zero) continue;
            int s = v.Idx < EvadeSizes.Length ? EvadeSizes[v.Idx] : 0;
            if (s == -1) { var m = ReadMap(ev.Data, q, z.Names); ev.Maps[v.Idx] = m; q = m.End; continue; }
            if (v.Idx > 21) break;
            if (s == 0) throw new Exception("evade property " + v.Idx + " not walked");
            q += s;
        }
        return ev;
    }

    // evade.js addEntry: the package gets its own `key` in `mapName`, a copy of `like`'s entry with X and frames set.
    public static byte[] AddEntry(byte[] buf, byte[] likeBuf, string mapName, string key, double? x, int? frames)
    {
        int mi = MapIndex[mapName];
        var le = ReadEvade(Parse(likeBuf));
        var src = (le.Maps.TryGetValue(mi, out var lm) ? lm.Entries.FirstOrDefault(e => e.Key == key) : null) ?? throw new Exception($"the model has no {key} in {mapName}");
        var value = le.Data.AsSpan(src.Value.Start, src.Value.End - src.Value.Start).ToArray();
        if (x != null) { if (src.Value.X < 0) throw new Exception("model entry stores no LaunchVelocity.X"); W64(value, src.Value.X - src.Value.Start, (long)Math.Round(x.Value * 4294967296.0)); }
        if (frames != null) { if (src.Value.Frames < 0) throw new Exception("model entry stores no DurationInFrames"); W32(value, src.Value.Frames - src.Value.Start, frames.Value); }

        var e0 = ReadEvade(Parse(buf));
        if (e0.Maps.TryGetValue(mi, out var m0) && m0.Entries.Any(e => e.Key == key)) throw new Exception($"already has its own {key}");
        var (named, nameIdx) = AddName(Parse(buf), key);
        var ev = ReadEvade(Parse(named));
        var pair = new byte[8]; W32(pair, 0, nameIdx);
        byte[] outb;
        if (ev.Maps.TryGetValue(mi, out var m))
        {
            var cnt = new byte[4]; W32(cnt, 0, m.Count + 1);
            var rep = new MemoryStream(); rep.Write(cnt); rep.Write(ev.Data.AsSpan(m.At + 8, m.End - m.At - 8)); rep.Write(pair); rep.Write(value);
            outb = SpliceExport(named, ev.Index, m.At + 4, m.End - m.At - 4, rep.ToArray());
        }
        else
        {
            int at = ev.HeaderEnd;
            foreach (var v in ev.Vals.Where(v => v.Idx < mi))
                if (!v.Zero) at = EvadeSizes[v.Idx] == -1 ? ev.Maps[v.Idx].End : ev.Pos[v.Idx] + EvadeSizes[v.Idx];
            var vals = ev.Vals.Append(new Val(mi, false)).OrderBy(v => v.Idx).ToList();
            var map = new byte[8]; W32(map, 4, 1);
            var rep = new MemoryStream(); rep.Write(WriteHeader(vals)); rep.Write(ev.Data.AsSpan(ev.HeaderEnd, at - ev.HeaderEnd)); rep.Write(map); rep.Write(pair); rep.Write(value);
            outb = SpliceExport(named, ev.Index, 0, at, rep.ToArray());
        }
        var ev3 = ReadEvade(Parse(outb));
        if (!ev3.Maps[mi].Entries.Any(e => e.Key == key)) throw new Exception("read-back: entry not found");
        return outb;
    }
}
