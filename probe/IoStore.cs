using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;


// Writer for a UE 5.1 IoStore container (TOC version 5), uncompressed, unencrypted, unsigned, with a directory index,
// plus the empty .pak the engine needs beside it. Layouts follow the game's own containers (see tocinfo and chdr).
public static class IoStore
{
    public record Package(string Path, ulong PackageId, int ExportCount, int ExportBundleCount, ulong[] Imports, byte[] Data);

    const int BlockSize = 0x10000;
    const uint None = 0xFFFFFFFF;

    static byte[] ChunkId(ulong id, byte type)
    {
        var b = new byte[12];
        BinaryPrimitives.WriteUInt64LittleEndian(b, id);
        // b[8..10] chunk index (big endian) = 0, b[10] padding
        b[11] = type;
        return b;
    }

    static ulong Hash(int seed, ReadOnlySpan<byte> id)
    {
        ulong h = seed != 0 ? (ulong)seed : 0xcbf29ce484222325UL;
        foreach (var x in id) h = unchecked(h * 0x00000100000001B3UL) ^ x;
        return h;
    }

    // The engine's lookup, as FIoStoreTocResource does it: returns the slot or -1.
    static int Lookup(int[] seeds, int chunkCount, Func<int, byte[]> idAt, ReadOnlySpan<byte> id)
    {
        if (seeds.Length == 0) return -1;
        int seed = seeds[Hash(0, id) % (ulong)seeds.Length];
        if (seed == 0) return -1;
        uint slot;
        if (seed < 0) { slot = (uint)(-seed - 1); if (slot >= chunkCount) return -1; }
        else slot = (uint)(Hash(seed, id) % (ulong)chunkCount);
        return id.SequenceEqual(idAt((int)slot)) ? (int)slot : -1;
    }

    // Seeds and the slot each chunk lands in. The seed count is free, so it grows until every bucket can be placed:
    // two chunks that share a bucket can collide under every seed (always, when the chunk count is a power of two,
    // because the hash's low bits then depend on the seed's low bits alone), and a larger seed count separates them.
    static (int[] seeds, int[] slotOf) BuildPerfectHash(List<byte[]> ids)
    {
        int n = ids.Count;
        for (int seedCount = n; ; seedCount++)
        {
            var seeds = new int[seedCount];
            var slotOf = Enumerable.Repeat(-1, n).ToArray();
            var taken = new bool[n];
            var buckets = Enumerable.Range(0, n).GroupBy(i => (int)(Hash(0, ids[i]) % (ulong)seedCount)).OrderByDescending(g => g.Count()).ToList();
            bool placed = true;
            foreach (var b in buckets.Where(g => g.Count() > 1))
            {
                var members = b.ToList();
                bool found = false;
                for (int seed = 1; seed <= 2000 && !found; seed++)
                {
                    var slots = members.Select(i => (int)(Hash(seed, ids[i]) % (ulong)n)).ToList();
                    if (slots.Distinct().Count() != slots.Count || slots.Any(s => taken[s])) continue;
                    for (int k = 0; k < members.Count; k++) { slotOf[members[k]] = slots[k]; taken[slots[k]] = true; }
                    seeds[b.Key] = seed;
                    found = true;
                }
                if (!found) { placed = false; break; }
            }
            if (!placed) continue;
            foreach (var b in buckets.Where(g => g.Count() == 1))
            {
                int slot = Array.IndexOf(taken, false);
                taken[slot] = true; slotOf[b.First()] = slot; seeds[b.Key] = -(slot + 1);
            }
            return (seeds, slotOf);
        }
    }

    static void WriteFString(BinaryWriter w, string s)
    {
        var bytes = Encoding.ASCII.GetBytes(s);
        w.Write(bytes.Length + 1); w.Write(bytes); w.Write((byte)0);
    }

    static void Write5(BinaryWriter w, ulong v, bool bigEndian)
    {
        Span<byte> b = stackalloc byte[5];
        for (int i = 0; i < 5; i++) b[bigEndian ? 4 - i : i] = (byte)(v >> (8 * i));
        w.Write(b);
    }

    static byte[] ContainerHeader(ulong containerId, List<Package> packages)
    {
        var ms = new MemoryStream(); var w = new BinaryWriter(ms);
        w.Write(0x496F436Eu); w.Write(2);           // signature, version (OptionalSegmentPackages)
        w.Write(containerId);
        w.Write(packages.Count);
        foreach (var p in packages) w.Write(p.PackageId);
        // Store entries: 24 bytes each (export count, bundle count, imported packages {num, offset from that field}, shader map hashes {0, 0}),
        // then the imported package ids.
        var blob = new MemoryStream(); var bw = new BinaryWriter(blob);
        int dataPos = 24 * packages.Count;
        for (int i = 0; i < packages.Count; i++)
        {
            var p = packages[i];
            bw.Write(p.ExportCount); bw.Write(p.ExportBundleCount);
            bw.Write(p.Imports.Length); bw.Write(p.Imports.Length == 0 ? 0 : dataPos - (24 * i + 8));
            bw.Write(0); bw.Write(0);
            dataPos += 8 * p.Imports.Length;
        }
        foreach (var p in packages) foreach (var id in p.Imports) bw.Write(id);
        w.Write((int)blob.Length); w.Write(blob.ToArray());
        w.Write(0); w.Write(0);                     // optional segment package ids, optional segment store entries
        w.Write(0);                                 // redirects name map: no names
        w.Write(0); w.Write(0);                     // localized packages, package redirects
        return ms.ToArray();
    }

    static byte[] DirectoryIndex(List<string> paths)
    {
        // paths[i] belongs to TOC entry i.
        var strings = new List<string>(); var stringIds = new Dictionary<string, uint>();
        uint Str(string s) { if (!stringIds.TryGetValue(s, out var id)) { id = (uint)strings.Count; strings.Add(s); stringIds[s] = id; } return id; }
        var dirs = new List<uint[]> { new[] { None, None, None, None } };   // name, first child, next sibling, first file
        var files = new List<uint[]>();                                       // name, next file, user data
        for (int i = 0; i < paths.Count; i++)
        {
            var parts = paths[i].Split('/');
            int dir = 0;
            foreach (var part in parts[..^1])
            {
                uint name = Str(part);
                uint child = dirs[dir][1]; uint last = None;
                while (child != None && dirs[(int)child][0] != name) { last = child; child = dirs[(int)child][2]; }
                if (child == None)
                {
                    child = (uint)dirs.Count; dirs.Add(new[] { name, None, None, None });
                    if (last == None) dirs[dir][1] = child; else dirs[(int)last][2] = child;
                }
                dir = (int)child;
            }
            uint f = (uint)files.Count; files.Add(new[] { Str(parts[^1]), None, (uint)i });
            if (dirs[dir][3] == None) dirs[dir][3] = f;
            else { uint cur = dirs[dir][3]; while (files[(int)cur][1] != None) cur = files[(int)cur][1]; files[(int)cur][1] = f; }
        }
        var ms = new MemoryStream(); var w = new BinaryWriter(ms);
        WriteFString(w, "../../../");
        w.Write(dirs.Count); foreach (var d in dirs) foreach (var x in d) w.Write(x);
        w.Write(files.Count); foreach (var f in files) foreach (var x in f) w.Write(x);
        w.Write(strings.Count); foreach (var s in strings) WriteFString(w, s);
        return ms.ToArray();
    }

    static byte[] EmptyPak()
    {
        // Pak version 11, no entries: primary index, path hash index, full directory index, footer.
        var pathHashIndex = new byte[8];   // empty path hash map, empty pruned directory index
        var fullDirIndex = new byte[4];    // empty directory map
        byte[] Primary(long self)
        {
            var ms = new MemoryStream(); var w = new BinaryWriter(ms);
            WriteFString(w, "../../../");
            w.Write(0);                      // entries
            w.Write(0UL);                    // path hash seed
            long size = 14 + 4 + 8 + 40 + 40 + 4 + 4;
            w.Write(1); w.Write(self + size); w.Write((long)pathHashIndex.Length); w.Write(SHA1.HashData(pathHashIndex));
            w.Write(1); w.Write(self + size + pathHashIndex.Length); w.Write((long)fullDirIndex.Length); w.Write(SHA1.HashData(fullDirIndex));
            w.Write(0);                      // encoded entries: no bytes
            w.Write(0);                      // entries that are not encoded
            if (ms.Length != size) throw new Exception("pak primary index size");
            return ms.ToArray();
        }
        var primary = Primary(0);
        var o = new MemoryStream(); var ow = new BinaryWriter(o);
        ow.Write(primary); ow.Write(pathHashIndex); ow.Write(fullDirIndex);
        ow.Write(new byte[16]);              // encryption key guid
        ow.Write((byte)0);                   // index not encrypted
        ow.Write(0x5A6F12E1u); ow.Write(11);
        ow.Write(0L); ow.Write((long)primary.Length); ow.Write(SHA1.HashData(primary));
        ow.Write(new byte[5 * 32]);          // compression method names
        return o.ToArray();
    }

    public static void Write(string outDir, string name, List<Package> packages)
    {
        Directory.CreateDirectory(outDir);
        ulong containerId = BinaryPrimitives.ReadUInt64LittleEndian(SHA1.HashData(Encoding.UTF8.GetBytes(name.ToLowerInvariant())));
        var ids = packages.Select(p => ChunkId(p.PackageId, 1)).ToList();
        var datas = packages.Select(p => p.Data).ToList();
        var paths = packages.Select(p => p.Path).ToList();
        ids.Add(ChunkId(containerId, 6)); datas.Add(ContainerHeader(containerId, packages));

        // Put the chunks in their perfect-hash slots.
        var (seeds, slotOf) = BuildPerfectHash(ids);
        int n = ids.Count;
        var sIds = new byte[n][]; var sData = new byte[n][]; var sPath = new string?[n];
        for (int i = 0; i < n; i++) { sIds[slotOf[i]] = ids[i]; sData[slotOf[i]] = datas[i]; sPath[slotOf[i]] = i < paths.Count ? paths[i] : null; }
        for (int i = 0; i < n; i++) if (Lookup(seeds, n, k => sIds[k], sIds[i]) != i) throw new Exception("perfect hash self-check failed");

        // .ucas: 64 KiB blocks, each chunk starting on a block boundary of the uncompressed space, every block padded to 16 bytes.
        var ucas = new MemoryStream();
        var offLens = new List<(ulong off, ulong len)>(); var blocks = new List<(ulong off, int size)>();
        ulong virt = 0;
        for (int i = 0; i < n; i++)
        {
            offLens.Add((virt, (ulong)sData[i].Length));
            for (int p = 0; p < sData[i].Length; p += BlockSize)
            {
                int len = Math.Min(BlockSize, sData[i].Length - p);
                blocks.Add(((ulong)ucas.Length, len));
                ucas.Write(sData[i], p, len);
                while (ucas.Length % 16 != 0) ucas.WriteByte(0);
            }
            virt += (ulong)((sData[i].Length + BlockSize - 1) / BlockSize) * BlockSize;
        }

        // Directory index over the package chunks (user data = TOC entry index).
        var dirPaths = new List<string>(); var dirToc = new List<int>();
        for (int i = 0; i < n; i++) if (sPath[i] != null) { dirPaths.Add(sPath[i]!); dirToc.Add(i); }
        var dirIndex = DirectoryIndex(dirPaths);
        // DirectoryIndex numbered the files 0..k-1 in order; rewrite user data to the real TOC entry indices.
        {
            int pos = 4 + 10;                                   // mount point FString
            int dirCount = BitConverter.ToInt32(dirIndex, pos); pos += 4 + 16 * dirCount;
            int fileCount = BitConverter.ToInt32(dirIndex, pos); pos += 4;
            for (int f = 0; f < fileCount; f++)
            {
                int k = BitConverter.ToInt32(dirIndex, pos + 12 * f + 8);
                BinaryPrimitives.WriteInt32LittleEndian(dirIndex.AsSpan(pos + 12 * f + 8), dirToc[k]);
            }
        }

        var toc = new MemoryStream(); var w = new BinaryWriter(toc);
        w.Write(Encoding.ASCII.GetBytes("-==--==--==--==-"));
        w.Write((byte)5); w.Write((byte)0); w.Write((ushort)0);
        w.Write(144u);                      // header size
        w.Write((uint)n);                   // entries
        w.Write((uint)blocks.Count); w.Write(12u);
        w.Write(0u); w.Write(32u);          // compression method names: none, 32 bytes each
        w.Write((uint)BlockSize);
        w.Write((uint)dirIndex.Length);
        w.Write(1u);                        // partitions
        w.Write(containerId);
        w.Write(new byte[16]);              // encryption key guid
        w.Write((byte)8); w.Write((byte)0); w.Write((ushort)0);   // flags: Indexed
        w.Write((uint)seeds.Length);
        w.Write(ulong.MaxValue);            // partition size
        w.Write(0u);                        // chunks without perfect hash
        w.Write(0u);
        w.Write(new byte[40]);
        if (toc.Length != 144) throw new Exception("toc header size");
        foreach (var id in sIds) w.Write(id);
        foreach (var (off, len) in offLens) { Write5(w, off, true); Write5(w, len, true); }
        foreach (var s in seeds) w.Write(s);
        foreach (var (off, size) in blocks)
        {
            Write5(w, off, false);
            w.Write((byte)size); w.Write((byte)(size >> 8)); w.Write((byte)(size >> 16));
            w.Write((byte)size); w.Write((byte)(size >> 8)); w.Write((byte)(size >> 16));
            w.Write((byte)0);
        }
        w.Write(dirIndex);
        foreach (var d in sData) { w.Write(SHA1.HashData(d)); w.Write(new byte[12]); w.Write((byte)0); }

        File.WriteAllBytes(Path.Combine(outDir, name + ".utoc"), toc.ToArray());
        File.WriteAllBytes(Path.Combine(outDir, name + ".ucas"), ucas.ToArray());
        File.WriteAllBytes(Path.Combine(outDir, name + ".pak"), EmptyPak());
        Console.Error.WriteLine($"wrote {name}.utoc ({toc.Length} bytes), .ucas ({ucas.Length}), .pak; container id {containerId:x16}, {n} chunks");
    }

    // Parses the plain parts of a .utoc and checks that every chunk is found through the perfect hash.
    public static int TocInfo(string file)
    {
        var b = File.ReadAllBytes(file);
        uint U(int o) => BitConverter.ToUInt32(b, o);
        int version = b[16]; uint headerSize = U(20), entries = U(24), blockCount = U(28), blockEntry = U(32), methods = U(36), methodLen = U(40);
        uint blockSize = U(44), dirSize = U(48), partitions = U(52); ulong containerId = BitConverter.ToUInt64(b, 56);
        byte flags = b[80]; uint seedCount = U(84); ulong partSize = BitConverter.ToUInt64(b, 88); uint overflow = U(96);
        Console.WriteLine($"version {version}, header {headerSize}, entries {entries}, blocks {blockCount} x {blockEntry}, methods {methods} x {methodLen}, block size {blockSize:x}");
        Console.WriteLine($"dir index {dirSize}, partitions {partitions} of {partSize:x}, container {containerId:x16}, flags {flags:x2}, seeds {seedCount}, without perfect hash {overflow}");
        int pos = (int)headerSize;
        int idsAt = pos; pos += 12 * (int)entries;
        int offAt = pos; pos += 10 * (int)entries;
        var seeds = new int[seedCount]; for (int i = 0; i < seedCount; i++) seeds[i] = BitConverter.ToInt32(b, pos + 4 * i);
        pos += 4 * (int)seedCount;
        var noHash = new HashSet<int>(); for (int i = 0; i < overflow; i++) noHash.Add(BitConverter.ToInt32(b, pos + 4 * i));
        pos += 4 * (int)overflow;
        int blocksAt = pos; pos += 12 * (int)blockCount;
        for (int i = 0; i < methods; i++) Console.WriteLine("method: " + Encoding.ASCII.GetString(b, pos + (int)methodLen * i, (int)methodLen).TrimEnd('\0'));
        pos += (int)(methods * methodLen);
        if ((flags & 4) != 0) { int hashSize = BitConverter.ToInt32(b, pos); pos += 4 + 2 * hashSize + 20 * (int)blockCount; }
        pos += (int)dirSize;
        pos += 33 * (int)entries;
        Console.WriteLine($"sections end at {pos}, file is {b.Length} bytes: {(pos == b.Length ? "layout matches" : "LAYOUT MISMATCH")}");
        int found = 0, viaOverflow = 0;
        for (int i = 0; i < entries; i++)
        {
            var id = b.AsSpan(idsAt + 12 * i, 12);
            int slot = Lookup(seeds, (int)entries, k => b.AsSpan(idsAt + 12 * k, 12).ToArray(), id);
            if (slot == i) found++; else if (noHash.Contains(i)) viaOverflow++;
        }
        Console.WriteLine($"perfect hash: {found} of {entries} chunks found in their own slot, {viaOverflow} listed as without perfect hash");
        for (int i = 0; i < Math.Min(4, (int)entries); i++)
        {
            ulong off = 0, len = 0;
            for (int k = 0; k < 5; k++) { off = off << 8 | b[offAt + 10 * i + k]; len = len << 8 | b[offAt + 10 * i + 5 + k]; }
            Console.WriteLine($"  chunk {i}: id {Convert.ToHexString(b, idsAt + 12 * i, 12)} offset {off:x} length {len}");
        }
        for (int i = 0; i < Math.Min(3, (int)blockCount); i++) Console.WriteLine($"  block {i}: {Convert.ToHexString(b, blocksAt + 12 * i, 12)}");
        return pos == b.Length && found + viaOverflow == entries ? 0 : 1;
    }
}
