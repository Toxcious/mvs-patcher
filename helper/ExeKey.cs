// The pak key, read from the player's own shipping exe at patch time (the port project's way: nothing ships with the
// key, and it is kept in memory only). The final build's key callback writes the 32 key bytes as eight
// `mov dword [rbp+d], imm32` stores right after its stack-cookie setup and `lea rdx,[rbp+..]`, and ends them with
// `cmp rcx,rdx`. The function is found by that shape, with no address of this build; the beta's exe masked its key
// and the port reads that differently. The key is only trusted once it opens the game's files (Patcher.Build checks).
public static class ExeKey
{
    const string ExeRel = @"MultiVersus\Binaries\Win64\MultiVersus-Win64-Shipping.exe";

    public static string FromGame(string game)
    {
        var exe = Path.Combine(game, ExeRel);
        if (!File.Exists(exe)) throw new Exception("no " + exe);
        return FromBytes(File.ReadAllBytes(exe));
    }

    // xor rax,rsp; mov [rbp+c],rax; lea rdx,[rbp+d]: the stack cookie, then the key callback's output buffer.
    static readonly int?[] Head = { 0x48, 0x33, 0xC4, 0x48, 0x89, 0x45, null, 0x48, 0x8D, 0x55, null };

    public static string FromBytes(byte[] b)
    {
        var found = new List<byte[]>();
        for (int i = 0; i + Head.Length < b.Length; i++)
        {
            if (b[i] != 0x48) continue;
            bool ok = true;
            for (int k = 0; k < Head.Length && ok; k++) ok = Head[k] == null || b[i + k] == Head[k];
            if (!ok) continue;
            var key = Stores(b, i + Head.Length);
            if (key != null) found.Add(key);
        }
        if (found.Count == 0) throw new Exception("the key could not be found in the game's exe (another game version?)");
        if (found.Count > 1) throw new Exception($"the key's place in the exe is not unique ({found.Count} matches; another game version?)");
        return "0x" + Convert.ToHexString(found[0]);
    }

    // Eight `C7 45 d imm32` stores at d, d+4, ... d+28 (a `48 8D 41 xx` may sit between them), then `48 3B CA`.
    static byte[]? Stores(byte[] b, int p)
    {
        var st = new List<(sbyte d, int at)>();
        while (p + 7 <= b.Length)
        {
            if (b[p] == 0xC7 && b[p + 1] == 0x45) { st.Add(((sbyte)b[p + 2], p + 3)); p += 7; }
            else if (b[p] == 0x48 && b[p + 1] == 0x8D && b[p + 2] == 0x41) p += 4;
            else if (b[p] == 0x48 && b[p + 1] == 0x3B && b[p + 2] == 0xCA) break;
            else return null;
            if (st.Count > 8) return null;
        }
        if (st.Count != 8) return null;
        st.Sort((x, y) => x.d.CompareTo(y.d));
        var key = new byte[32];
        for (int i = 0; i < 8; i++)
        {
            if (st[i].d != st[0].d + 4 * i) return null;
            Array.Copy(b, st[i].at, key, 4 * i, 4);
        }
        return key;
    }
}
